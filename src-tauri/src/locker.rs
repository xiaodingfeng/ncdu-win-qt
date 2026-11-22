//! Files-in-use detection and process closing, mirroring the Qt build's
//! WinApi helpers: Windows Restart Manager plus a process-image scan, a
//! never-close list, and a polite-then-force shutdown.

use std::cmp::Reverse;
use std::collections::BinaryHeap;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use serde::Serialize;
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_MORE_DATA, ERROR_SUCCESS, HANDLE, INVALID_HANDLE_VALUE,
    WAIT_TIMEOUT,
};
use windows_sys::Win32::Security::{
    EqualSid, GetLengthSid, GetTokenInformation, TokenUser, PSID, TOKEN_QUERY, TOKEN_USER,
};
use windows_sys::Win32::Storage::FileSystem::SYNCHRONIZE;
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::RemoteDesktop::ProcessIdToSessionId;
use windows_sys::Win32::System::RestartManager::{
    RmEndSession, RmGetList, RmRegisterResources, RmStartSession, RM_PROCESS_INFO,
    CCH_RM_SESSION_KEY,
};
use windows_sys::Win32::System::Threading::{
    GetCurrentProcessId, OpenProcess, OpenProcessToken, QueryFullProcessImageNameW, TerminateProcess,
    WaitForSingleObject, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    EnumWindows, GetWindowThreadProcessId, PostMessageW, WM_CLOSE,
};

const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x10;
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const PROBE_WALK_CAP: usize = 200_000;
const DEFAULT_MAX_FILES: usize = 800;
const DEFAULT_GRACE_MS: u64 = 4_000;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LockingProcess {
    pub pid: u32,
    pub name: String,
    pub exe_path: String,
    pub safe_to_close: bool,
    pub block_key: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CloseOutcome {
    pub pid: u32,
    pub name: String,
    pub result: String,
    pub block_key: String,
}

fn wide(value: &str) -> Vec<u16> {
    OsStr::new(value).encode_wide().chain(std::iter::once(0)).collect()
}

fn utf16_lossy(buffer: &[u16]) -> String {
    let end = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
    String::from_utf16_lossy(&buffer[..end])
}

fn normalized(path: &str) -> String {
    path.trim()
        .replace('/', "\\")
        .trim_end_matches('\\')
        .to_lowercase()
}

fn path_inside_directory(path: &str, dir: &str) -> bool {
    let file = normalized(path);
    let root = normalized(dir);
    if file.is_empty() || root.is_empty() {
        return false;
    }
    file == root || file.starts_with(&format!("{root}\\"))
}

fn file_name(path: &str) -> String {
    Path::new(path)
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// Processes this application will never close on the user's behalf.
pub(crate) fn never_close_names() -> &'static [&'static str] {
    &[
        // Machine- or session-critical.
        "system",
        "registry",
        "smss.exe",
        "csrss.exe",
        "wininit.exe",
        "winlogon.exe",
        "services.exe",
        "lsass.exe",
        "svchost.exe",
        "fontdrvhost.exe",
        "dwm.exe",
        "audiodg.exe",
        "sihost.exe",
        "ctfmon.exe",
        "taskhostw.exe",
        "runtimebroker.exe",
        "searchindexer.exe",
        // The shell: restarting it loses the taskbar and every Explorer window.
        "explorer.exe",
        // Security software: killing it is pointless and invites quarantine.
        "msmpeng.exe",
        "nissrv.exe",
        "securityhealthservice.exe",
        "msseces.exe",
        "usysdiag.exe",
        "hipsdaemon.exe",
        "qqpctray.exe",
        "qqpcsrv.exe",
        "360tray.exe",
        "360safe.exe",
        "zhudongfangyu.exe",
    ]
}

pub(crate) fn is_never_close_name(name_or_path: &str) -> bool {
    let base = file_name(name_or_path).to_lowercase();
    !base.is_empty() && never_close_names().contains(&base.as_str())
}

fn process_image(pid: u32) -> Option<String> {
    if pid == 0 {
        return None;
    }
    unsafe {
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if process.is_null() {
            return None;
        }
        let mut buffer = [0u16; 1024];
        let mut size = buffer.len() as u32;
        let ok = QueryFullProcessImageNameW(process, 0, buffer.as_mut_ptr(), &mut size);
        CloseHandle(process);
        if ok == 0 {
            return None;
        }
        Some(String::from_utf16_lossy(&buffer[..size as usize]))
    }
}

unsafe fn token_sid(token: HANDLE) -> Option<Vec<u8>> {
    let mut size = 0u32;
    GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut size);
    if size == 0 {
        return None;
    }
    let mut buffer = vec![0u8; size as usize];
    let ok = GetTokenInformation(
        token,
        TokenUser,
        buffer.as_mut_ptr() as *mut core::ffi::c_void,
        size,
        &mut size,
    );
    if ok == 0 {
        return None;
    }
    let user = &*(buffer.as_ptr() as *const TOKEN_USER);
    let sid = user.User.Sid;
    if sid.is_null() {
        return None;
    }
    let length = GetLengthSid(sid) as usize;
    if length == 0 {
        return None;
    }
    let mut copy = vec![0u8; length];
    std::ptr::copy_nonoverlapping(sid as *const u8, copy.as_mut_ptr(), length);
    Some(copy)
}

unsafe fn process_sid(pid: u32) -> Option<Vec<u8>> {
    let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
    if process.is_null() {
        return None;
    }
    let mut token: HANDLE = std::ptr::null_mut();
    let opened = OpenProcessToken(process, TOKEN_QUERY, &mut token);
    CloseHandle(process);
    if opened == 0 {
        return None;
    }
    let sid = token_sid(token);
    CloseHandle(token);
    sid
}

fn current_sid() -> Option<&'static [u8]> {
    static SID: OnceLock<Option<Vec<u8>>> = OnceLock::new();
    SID.get_or_init(|| unsafe { process_sid(GetCurrentProcessId()) })
        .as_deref()
}

fn same_user(pid: u32) -> Option<bool> {
    let ours = current_sid()?;
    let theirs = unsafe { process_sid(pid) }?;
    let same = unsafe { EqualSid(ours.as_ptr() as PSID, theirs.as_ptr() as PSID) };
    Some(same != 0)
}

fn closable_process(pid: u32, exe_path: &str, name: &str) -> (bool, String) {
    let refuse = |key: &str| (false, key.to_string());
    if pid == 0 || pid == unsafe { GetCurrentProcessId() } {
        return refuse("proc_close.block_self");
    }
    if is_never_close_name(exe_path) || is_never_close_name(name) {
        return refuse("proc_close.block_system");
    }
    let mut theirs = 0u32;
    let mut ours = 0u32;
    unsafe {
        ProcessIdToSessionId(pid, &mut theirs);
        ProcessIdToSessionId(GetCurrentProcessId(), &mut ours);
    }
    if theirs != ours {
        return refuse("proc_close.block_session");
    }
    match same_user(pid) {
        Some(true) => (true, String::new()),
        Some(false) => refuse("proc_close.block_user"),
        None => refuse("proc_close.block_unknown"),
    }
}

fn describe_process(pid: u32, friendly_name: &str) -> LockingProcess {
    let exe_path = process_image(pid).unwrap_or_default();
    let mut name = friendly_name.to_string();
    if name.is_empty() {
        name = if exe_path.is_empty() {
            pid.to_string()
        } else {
            file_name(&exe_path)
        };
    }
    let (safe_to_close, block_key) = closable_process(pid, &exe_path, &name);
    LockingProcess {
        pid,
        name,
        exe_path,
        safe_to_close,
        block_key,
    }
}

/// The most recently modified `cap` files under `dir`, bounded by a walk cap.
fn newest_files_under(dir: &Path, cap: usize) -> Vec<PathBuf> {
    if cap == 0 {
        return Vec::new();
    }
    let mut heap: BinaryHeap<Reverse<(u64, PathBuf)>> = BinaryHeap::with_capacity(cap);
    let mut examined = 0usize;
    let mut pending = vec![dir.to_path_buf()];
    'walk: while let Some(current) = pending.pop() {
        let Ok(entries) = std::fs::read_dir(&current) else {
            continue;
        };
        for entry in entries.flatten() {
            let Ok(metadata) = std::fs::symlink_metadata(entry.path()) else {
                continue;
            };
            let attributes = metadata.file_attributes();
            if attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
                continue;
            }
            if attributes & FILE_ATTRIBUTE_DIRECTORY != 0 {
                pending.push(entry.path());
                continue;
            }
            examined += 1;
            if examined > PROBE_WALK_CAP {
                break 'walk;
            }
            let candidate = (metadata.last_write_time(), entry.path());
            if heap.len() < cap {
                heap.push(Reverse(candidate));
            } else if let Some(Reverse(oldest)) = heap.peek() {
                if candidate.0 > oldest.0 {
                    heap.pop();
                    heap.push(Reverse(candidate));
                }
            }
        }
    }
    heap.into_iter().map(|Reverse((_, path))| path).collect()
}

fn push_unique(procs: &mut Vec<LockingProcess>, process: LockingProcess) {
    if procs.iter().any(|existing| existing.pid == process.pid) {
        return;
    }
    procs.push(process);
}

fn processes_running_from(dir: &str) -> Vec<LockingProcess> {
    let mut found = Vec::new();
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snapshot == INVALID_HANDLE_VALUE {
            return found;
        }
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        if Process32FirstW(snapshot, &mut entry) != 0 {
            loop {
                let pid = entry.th32ProcessID;
                if pid != 0 && pid != 4 {
                    if let Some(image) = process_image(pid) {
                        if path_inside_directory(&image, dir) {
                            push_unique(&mut found, describe_process(pid, ""));
                        }
                    }
                }
                if Process32NextW(snapshot, &mut entry) == 0 {
                    break;
                }
            }
        }
        CloseHandle(snapshot);
    }
    found
}

/// Who is holding `dir` open. Restart Manager reports processes holding the
/// newest files; a process-image scan catches programs running from inside it.
pub(crate) fn processes_locking_dir(dir: &str, max_files: usize) -> Vec<LockingProcess> {
    let mut procs = Vec::new();
    let files = newest_files_under(Path::new(dir), max_files.max(1));
    let native: Vec<Vec<u16>> = files.iter().map(|file| wide(&file.to_string_lossy())).collect();
    let pointers: Vec<*const u16> = native.iter().map(|file| file.as_ptr()).collect();

    let mut session = 0u32;
    let mut key = [0u16; CCH_RM_SESSION_KEY as usize + 1];
    let start = unsafe { RmStartSession(&mut session, 0, key.as_mut_ptr()) };
    if start != ERROR_SUCCESS {
        return processes_running_from(dir);
    }

    let mut registered = true;
    if !pointers.is_empty() {
        let rc = unsafe {
            RmRegisterResources(
                session,
                pointers.len() as u32,
                pointers.as_ptr(),
                0,
                std::ptr::null(),
                0,
                std::ptr::null(),
            )
        };
        if rc != ERROR_SUCCESS {
            registered = false;
            log::warn!("[locker] RmRegisterResources failed rc={rc} files={}", pointers.len());
        }
    }

    if registered {
        let mut info: Vec<RM_PROCESS_INFO> = vec![unsafe { std::mem::zeroed() }; 64];
        let mut reasons = vec![0u32; 64];
        let mut needed = 0u32;
        let mut got = info.len() as u32;
        let mut rc = unsafe {
            RmGetList(
                session,
                &mut needed,
                &mut got,
                info.as_mut_ptr(),
                reasons.as_mut_ptr(),
            )
        };
        if rc == ERROR_MORE_DATA && needed > got {
            info = vec![unsafe { std::mem::zeroed() }; needed as usize];
            reasons = vec![0u32; needed as usize];
            got = info.len() as u32;
            rc = unsafe {
                RmGetList(
                    session,
                    &mut needed,
                    &mut got,
                    info.as_mut_ptr(),
                    reasons.as_mut_ptr(),
                )
            };
        }
        if rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA {
            for item in info.iter().take(got as usize) {
                let pid = item.Process.dwProcessId;
                if pid == 0 {
                    continue;
                }
                push_unique(&mut procs, describe_process(pid, &utf16_lossy(&item.strAppName)));
            }
        } else {
            log::warn!("[locker] RmGetList failed rc={rc}");
        }
    }

    unsafe {
        RmEndSession(session);
    }

    for process in processes_running_from(dir) {
        push_unique(&mut procs, process);
    }
    procs
}

/// Lock detection for one selected path (file or directory).
pub(crate) fn processes_locking_path(path: &str, max_files: usize) -> Vec<LockingProcess> {
    let target = Path::new(path);
    if target.is_dir() {
        return processes_locking_dir(path, max_files);
    }
    if !target.exists() {
        return Vec::new();
    }
    let file = wide(path);
    let pointers = [file.as_ptr()];
    let mut procs = Vec::new();
    let mut session = 0u32;
    let mut key = [0u16; CCH_RM_SESSION_KEY as usize + 1];
    if unsafe { RmStartSession(&mut session, 0, key.as_mut_ptr()) } != ERROR_SUCCESS {
        return procs;
    }
    let rc = unsafe {
        RmRegisterResources(
            session,
            1,
            pointers.as_ptr(),
            0,
            std::ptr::null(),
            0,
            std::ptr::null(),
        )
    };
    if rc == ERROR_SUCCESS {
        let mut info: Vec<RM_PROCESS_INFO> = vec![unsafe { std::mem::zeroed() }; 64];
        let mut reasons = vec![0u32; 64];
        let mut needed = 0u32;
        let mut got = info.len() as u32;
        let mut result = unsafe {
            RmGetList(
                session,
                &mut needed,
                &mut got,
                info.as_mut_ptr(),
                reasons.as_mut_ptr(),
            )
        };
        if result == ERROR_MORE_DATA && needed > got {
            info = vec![unsafe { std::mem::zeroed() }; needed as usize];
            reasons = vec![0u32; needed as usize];
            got = info.len() as u32;
            result = unsafe {
                RmGetList(
                    session,
                    &mut needed,
                    &mut got,
                    info.as_mut_ptr(),
                    reasons.as_mut_ptr(),
                )
            };
        }
        if result == ERROR_SUCCESS || result == ERROR_MORE_DATA {
            for item in info.iter().take(got as usize) {
                let pid = item.Process.dwProcessId;
                if pid == 0 {
                    continue;
                }
                push_unique(&mut procs, describe_process(pid, &utf16_lossy(&item.strAppName)));
            }
        }
    }
    unsafe {
        RmEndSession(session);
    }
    procs
}

struct CloseEnumContext {
    pid: u32,
    sent: u32,
}

unsafe extern "system" fn close_enum(hwnd: *mut core::ffi::c_void, lparam: isize) -> i32 {
    let context = &mut *(lparam as *mut CloseEnumContext);
    let mut owner = 0u32;
    GetWindowThreadProcessId(hwnd, &mut owner);
    if owner == context.pid && PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0 {
        context.sent += 1;
    }
    1
}

fn request_close_process(pid: u32) -> u32 {
    if pid == 0 {
        return 0;
    }
    let mut context = CloseEnumContext { pid, sent: 0 };
    unsafe {
        EnumWindows(Some(close_enum), &mut context as *mut _ as isize);
    }
    context.sent
}

fn process_alive(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    unsafe {
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, 0, pid);
        if process.is_null() {
            return false;
        }
        let result = WaitForSingleObject(process, 0);
        CloseHandle(process);
        result == WAIT_TIMEOUT
    }
}

fn terminate_process(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    unsafe {
        let process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, 0, pid);
        if process.is_null() {
            log::warn!(
                "[locker] OpenProcess(terminate) pid={pid} err={}",
                GetLastError()
            );
            return false;
        }
        let ok = TerminateProcess(process, 1) != 0;
        if ok {
            WaitForSingleObject(process, 5_000);
        } else {
            log::warn!(
                "[locker] TerminateProcess pid={pid} err={}",
                GetLastError()
            );
        }
        CloseHandle(process);
        ok
    }
}

fn close_processes(procs: &[LockingProcess], grace_ms: u64) -> Vec<CloseOutcome> {
    let mut outcomes = Vec::with_capacity(procs.len());
    let mut waiting = Vec::new();
    for (index, process) in procs.iter().enumerate() {
        let outcome = if !process.safe_to_close {
            CloseOutcome {
                pid: process.pid,
                name: process.name.clone(),
                result: "refused".to_string(),
                block_key: process.block_key.clone(),
            }
        } else if !process_alive(process.pid) {
            CloseOutcome {
                pid: process.pid,
                name: process.name.clone(),
                result: "exited".to_string(),
                block_key: String::new(),
            }
        } else {
            request_close_process(process.pid);
            waiting.push(index);
            CloseOutcome {
                pid: process.pid,
                name: process.name.clone(),
                result: "survived".to_string(),
                block_key: String::new(),
            }
        };
        outcomes.push(outcome);
    }

    let mut elapsed = 0u64;
    while !waiting.is_empty() && elapsed < grace_ms {
        std::thread::sleep(std::time::Duration::from_millis(30));
        elapsed += 30;
        waiting.retain(|index| {
            let alive = process_alive(procs[*index].pid);
            if !alive {
                outcomes[*index].result = "exited".to_string();
            }
            alive
        });
    }

    for index in waiting {
        let process = &procs[index];
        let _ = terminate_process(process.pid);
        let gone = !process_alive(process.pid);
        outcomes[index].result = if gone { "killed" } else { "survived" }.to_string();
    }
    outcomes
}

/// Writes the "do not delete" marker the app leaves inside relocated folders.
pub(crate) fn write_dont_delete_marker(dir: &str) -> bool {
    const MARKER_NAME: &str = "!请勿删除、移动或重命名文件夹.ico";
    const MARKER: &[u8] = include_bytes!("../resources/dont_delete_folder.ico");
    let target = Path::new(dir).join(MARKER_NAME);
    if target.exists() {
        return true;
    }
    std::fs::write(target, MARKER).is_ok()
}

/// Removes the marker once a folder is restored to its original location.
pub(crate) fn remove_dont_delete_marker(dir: &str) {
    const MARKER_NAME: &str = "!请勿删除、移动或重命名文件夹.ico";
    let _ = std::fs::remove_file(Path::new(dir).join(MARKER_NAME));
}

#[tauri::command]
pub async fn locked_processes(
    paths: Vec<String>,
    max_files: Option<usize>,
) -> Result<Vec<LockingProcess>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let cap = max_files.unwrap_or(DEFAULT_MAX_FILES);
        let mut procs = Vec::new();
        for path in paths.iter().filter(|path| !path.trim().is_empty()).take(8) {
            for process in processes_locking_path(path, cap) {
                push_unique(&mut procs, process);
            }
        }
        procs
    })
    .await
    .map_err(|err| err.to_string())
}

#[tauri::command]
pub async fn close_locking_processes(
    pids: Vec<u32>,
    grace_ms: Option<u64>,
) -> Result<Vec<CloseOutcome>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let mut procs = Vec::new();
        for pid in pids {
            push_unique(&mut procs, describe_process(pid, ""));
        }
        close_processes(&procs, grace_ms.unwrap_or(DEFAULT_GRACE_MS))
    })
    .await
    .map_err(|err| err.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn never_close_list_matches_bare_names_and_paths() {
        assert!(is_never_close_name("C:\\Windows\\explorer.exe"));
        assert!(is_never_close_name("EXPLORER.EXE"));
        assert!(is_never_close_name("System"));
        assert!(!is_never_close_name("C:\\Tools\\myapp.exe"));
    }

    #[test]
    fn path_prefix_checks_directory_boundaries() {
        assert!(path_inside_directory("C:\\Apps\\Tool\\tool.exe", "C:\\Apps\\Tool"));
        assert!(path_inside_directory("C:\\Apps\\Tool", "C:\\Apps\\Tool\\"));
        assert!(!path_inside_directory("C:\\Apps\\Toolbox\\tool.exe", "C:\\Apps\\Tool"));
        assert!(!path_inside_directory("", "C:\\Apps"));
    }

    #[test]
    fn current_process_belongs_to_the_current_user() {
        assert_eq!(same_user(std::process::id()), Some(true));
    }

    #[test]
    #[ignore = "opens an exclusive file handle; run manually on Windows"]
    fn restart_manager_reports_an_exclusive_handle() {
        use std::os::windows::fs::OpenOptionsExt;
        let dir = std::env::temp_dir().join("ncduwin-locker-test");
        let _ = std::fs::create_dir_all(&dir);
        let file = dir.join("locked.dat");
        std::fs::write(&file, b"locked").expect("write");
        let handle = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .share_mode(0)
            .open(&file)
            .expect("open");
        let procs = processes_locking_path(&file.to_string_lossy(), 200);
        drop(handle);
        assert!(
            procs.iter().any(|process| process.pid == std::process::id()),
            "expected this test process in {procs:?}"
        );
    }
}
