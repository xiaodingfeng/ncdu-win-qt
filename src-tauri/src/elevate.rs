//! Restricted elevated helper: the main process asks Windows to launch this
//! same executable with `--elevated-helper <pipe> <token>` and talks to it over
//! a one-shot named pipe. The helper accepts a fixed set of operations and
//! re-validates every path; it never runs arbitrary commands from the UI.

use std::ffi::OsStr;
use std::fs;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::process::CommandExt;
use std::path::{Component, Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, ReadFile, WriteFile, OPEN_EXISTING, PIPE_ACCESS_DUPLEX,
};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PIPE_READMODE_BYTE, PIPE_TYPE_BYTE,
    PIPE_UNLIMITED_INSTANCES, PIPE_WAIT,
};
use windows_sys::Win32::UI::Shell::ShellExecuteW;
use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_WRITE};
use winreg::RegKey;

const HELPER_FLAG: &str = "--elevated-helper";
const ERROR_PIPE_CONNECTED: u32 = 535;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(60);
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const CREATE_UNICODE_ENVIRONMENT: u32 = 0x0000_0400;
const WU_KEY: &str = r"SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate";
const AU_KEY: &str = r"SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU";
const LEGACY_KEY: &str = r"SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update";
const UPDATE_TASKS: &[&str] = &[
    r"\Microsoft\Windows\WindowsUpdate\Scheduled Start",
    r"\Microsoft\Windows\WindowsUpdate\sih",
    r"\Microsoft\Windows\WindowsUpdate\sihproxy",
    r"\Microsoft\Windows\UpdateOrchestrator\Schedule Scan",
    r"\Microsoft\Windows\UpdateOrchestrator\Schedule Scan Static Task",
    r"\Microsoft\Windows\UpdateOrchestrator\UpdateModelTask",
    r"\Microsoft\Windows\UpdateOrchestrator\USO_UxBroker",
];

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub(crate) enum HelperOp {
    SetUpdateEnabled { enable: bool },
    NetworkFix,
    FlushDns,
    CreateJunction { source: String, target: String },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct HelperResponse {
    ok: bool,
    needs_reboot: bool,
    message: String,
    total: u32,
}

impl HelperResponse {
    fn ok_with(total: u32) -> Self {
        Self {
            ok: true,
            needs_reboot: false,
            message: String::new(),
            total,
        }
    }

    fn failed(message: impl Into<String>) -> Self {
        Self {
            ok: false,
            needs_reboot: false,
            message: message.into(),
            total: 0,
        }
    }
}

pub(crate) struct ElevateOutcome {
    pub ok: bool,
    pub needs_reboot: bool,
    pub message: String,
    pub total: u32,
}

fn wide(value: &str) -> Vec<u16> {
    OsStr::new(value)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn token() -> String {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_nanos())
        .unwrap_or_default();
    format!("{:x}{:x}{:x}", std::process::id(), nanos, nanos >> 17)
}

fn close_handle(handle: HANDLE) {
    unsafe {
        CloseHandle(handle);
    }
}

fn take_and_close(slot: &Arc<AtomicIsize>) {
    let value = slot.swap(0, Ordering::SeqCst);
    if value != 0 {
        close_handle(value as HANDLE);
    }
}

fn write_all(handle: HANDLE, bytes: &[u8]) -> Result<(), String> {
    let mut written = 0usize;
    while written < bytes.len() {
        let mut count = 0u32;
        let ok = unsafe {
            WriteFile(
                handle,
                bytes[written..].as_ptr(),
                (bytes.len() - written) as u32,
                &mut count,
                std::ptr::null_mut(),
            )
        };
        if ok == 0 || count == 0 {
            return Err(format!("pipe write failed (error {})", unsafe { GetLastError() }));
        }
        written += count as usize;
    }
    Ok(())
}

fn read_line(handle: HANDLE) -> Result<String, String> {
    let mut bytes = Vec::new();
    let mut buffer = [0u8; 1];
    loop {
        let mut count = 0u32;
        let ok = unsafe {
            ReadFile(
                handle,
                buffer.as_mut_ptr(),
                1,
                &mut count,
                std::ptr::null_mut(),
            )
        };
        if ok == 0 {
            if bytes.is_empty() {
                return Err(format!("pipe read failed (error {})", unsafe { GetLastError() }));
            }
            break;
        }
        if count == 0 {
            break;
        }
        if buffer[0] == b'\n' {
            break;
        }
        bytes.push(buffer[0]);
        if bytes.len() > 64 * 1024 {
            return Err("pipe message is too large".to_string());
        }
    }
    String::from_utf8(bytes).map_err(|err| format!("pipe message is not UTF-8: {err}"))
}

fn connect_pipe(name: &str, timeout: Duration) -> Result<HANDLE, String> {
    let path = wide(name);
    let started = Instant::now();
    loop {
        let handle = unsafe {
            CreateFileW(
                path.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                std::ptr::null(),
                OPEN_EXISTING,
                0,
                std::ptr::null_mut(),
            )
        };
        if handle != INVALID_HANDLE_VALUE && !handle.is_null() {
            return Ok(handle);
        }
        if started.elapsed() >= timeout {
            return Err("cannot connect to the elevation pipe".to_string());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// True when this process was started by `run_elevated`.
pub(crate) fn is_helper_invocation() -> bool {
    std::env::args().any(|arg| arg == HELPER_FLAG)
}

/// Handles `--elevated-helper <pipe> <token>` and exits.
pub(crate) fn run_helper_from_args() -> i32 {
    let args: Vec<String> = std::env::args().collect();
    let Some(index) = args.iter().position(|arg| arg == HELPER_FLAG) else {
        return 1;
    };
    let Some(pipe_name) = args.get(index + 1) else {
        return 1;
    };
    let Some(expected_token) = args.get(index + 2) else {
        return 1;
    };
    let result = (|| -> Result<HelperResponse, String> {
        let handle = connect_pipe(pipe_name, Duration::from_secs(15))?;
        let hello = read_line(handle)?;
        if hello.trim() != expected_token {
            let _ = write_all(
                handle,
                format!("{}\n", serde_json::to_string(&HelperResponse::failed("token mismatch")).unwrap_or_default()).as_bytes(),
            );
            close_handle(handle);
            return Ok(HelperResponse::failed("helper token mismatch"));
        }
        let request = read_line(handle)?;
        let op: HelperOp =
            serde_json::from_str(&request).map_err(|err| format!("invalid helper request: {err}"))?;
        let response = execute(op);
        let _ = write_all(
            handle,
            format!("{}\n", serde_json::to_string(&response).unwrap_or_default()).as_bytes(),
        );
        close_handle(handle);
        Ok(response)
    })();
    match result {
        Ok(response) => {
            if response.ok {
                0
            } else {
                1
            }
        }
        Err(_) => 1,
    }
}

/// Runs one fixed operation through the elevated helper. Returns `Err` when
/// elevation was cancelled or the helper could not be started.
pub(crate) fn run_elevated(op: HelperOp) -> Result<ElevateOutcome, String> {
    let exe = std::env::current_exe()
        .map_err(|err| format!("cannot locate the current executable: {err}"))?;
    let secret = token();
    let pipe_name = format!(
        r"\\.\pipe\ncduwin-elevate-{}-{}",
        std::process::id(),
        secret
    );
    let slot = Arc::new(AtomicIsize::new(0));
    let finished = Arc::new(AtomicBool::new(false));

    let server = {
        let pipe_name = pipe_name.clone();
        let secret = secret.clone();
        let slot = Arc::clone(&slot);
        let finished = Arc::clone(&finished);
        std::thread::spawn(move || -> Result<HelperResponse, String> {
            let name = wide(&pipe_name);
            let handle = unsafe {
                CreateNamedPipeW(
                    name.as_ptr(),
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    PIPE_UNLIMITED_INSTANCES,
                    4096,
                    4096,
                    0,
                    std::ptr::null(),
                )
            };
            if handle == INVALID_HANDLE_VALUE || handle.is_null() {
                return Err(format!(
                    "cannot create the elevation pipe (error {})",
                    unsafe { GetLastError() }
                ));
            }
            slot.store(handle as isize, Ordering::SeqCst);

            let connected = unsafe { ConnectNamedPipe(handle, std::ptr::null_mut()) };
            if connected == 0 {
                let error = unsafe { GetLastError() };
                if error != ERROR_PIPE_CONNECTED {
                    if slot.swap(0, Ordering::SeqCst) != 0 {
                        close_handle(handle);
                    }
                    return Err(format!("helper did not connect (error {error})"));
                }
            }

            let result = (|| -> Result<HelperResponse, String> {
                // The helper verifies this one-shot token, then reads the operation.
                write_all(handle, format!("{secret}\n").as_bytes())?;
                let request =
                    serde_json::to_string(&op).map_err(|err| format!("cannot encode request: {err}"))?;
                write_all(handle, format!("{request}\n").as_bytes())?;
                let response = read_line(handle)?;
                serde_json::from_str(&response)
                    .map_err(|err| format!("invalid helper response: {err}"))
            })();

            unsafe {
                DisconnectNamedPipe(handle);
            }
            if slot.swap(0, Ordering::SeqCst) != 0 {
                close_handle(handle);
            }
            finished.store(true, Ordering::SeqCst);
            result
        })
    };

    // Watchdog: unblock the server thread if UAC never shows or the helper hangs.
    {
        let slot = Arc::clone(&slot);
        let finished = Arc::clone(&finished);
        std::thread::spawn(move || {
            let started = Instant::now();
            while started.elapsed() < CONNECT_TIMEOUT {
                if finished.load(Ordering::SeqCst) {
                    return;
                }
                std::thread::sleep(Duration::from_millis(250));
            }
            take_and_close(&slot);
        });
    }

    let arguments = format!("{HELPER_FLAG} \"{pipe_name}\" \"{secret}\"");
    let exe_wide = wide(&exe.to_string_lossy());
    let arguments_wide = wide(&arguments);
    let verb = wide("runas");
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            exe_wide.as_ptr(),
            arguments_wide.as_ptr(),
            std::ptr::null(),
            0,
        )
    };
    if result as isize <= 32 {
        take_and_close(&slot);
        let _ = server.join();
        return Err("elevation was cancelled".to_string());
    }

    let response = server.join().map_err(|_| "the helper thread panicked".to_string())??;
    Ok(ElevateOutcome {
        ok: response.ok,
        needs_reboot: response.needs_reboot,
        message: response.message,
        total: response.total,
    })
}

// ---------------------------------------------------------------------------
// Helper-side operations (fixed, re-validated)
// ---------------------------------------------------------------------------

/// Runs a console program without ever flashing a window and captures its
/// output so failures can explain themselves.
fn run_quiet(command: &str, args: &[&str]) -> Result<String, String> {
    let output = Command::new(command)
        .args(args)
        .creation_flags(CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT)
        .output()
        .map_err(|err| format!("cannot start {command}: {err}"))?;
    let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
    let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
    if output.status.success() {
        Ok(stdout)
    } else {
        let detail = if stderr.is_empty() { stdout } else { stderr };
        Err(format!(
            "{command} exited with {:?}: {detail}",
            output.status.code()
        ))
    }
}

fn run(command: &str, args: &[&str]) -> Result<(), String> {
    run_quiet(command, args).map(|_| ())
}

/// Service state changes can fail transiently while the service control
/// manager is restarting a service; retry once before reporting a failure.
fn run_retry(command: &str, args: &[&str]) -> Result<(), String> {
    let mut last = String::new();
    for _ in 0..2 {
        match run(command, args) {
            Ok(()) => return Ok(()),
            Err(err) => {
                last = err;
                std::thread::sleep(Duration::from_millis(250));
            }
        }
    }
    Err(last)
}

fn set_dword(key: &str, name: &str, value: u32) -> Result<(), String> {
    let root = RegKey::predef(HKEY_LOCAL_MACHINE);
    let (key, _) = root
        .create_subkey(key)
        .map_err(|err| format!("cannot open HKLM\\{key}: {err}"))?;
    key.set_value(name, &value)
        .map_err(|err| format!("cannot write {name}: {err}"))
}

fn delete_value(key: &str, name: &str) {
    if let Ok(key) = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey_with_flags(key, KEY_WRITE) {
        let _ = key.delete_value(name);
    }
}

fn set_service_start(service: &str, value: u32) {
    let key = format!(r"SYSTEM\CurrentControlSet\Services\{service}");
    let _ = set_dword(&key, "Start", value);
}

fn hosts_path() -> Option<PathBuf> {
    std::env::var_os("WINDIR")
        .map(|windir| PathBuf::from(windir).join("System32/drivers/etc/hosts"))
}

fn hosts_blocks_updates() -> bool {
    let Some(path) = hosts_path() else {
        return false;
    };
    let Ok(text) = fs::read_to_string(path) else {
        return false;
    };
    text.lines().any(|line| {
        let entry = line.split('#').next().unwrap_or("").trim().to_lowercase();
        !entry.is_empty()
            && (entry.contains("windowsupdate")
                || entry.contains("update.microsoft")
                || entry.contains("delivery.mp.microsoft"))
    })
}

/// Removes update-blocking hosts entries, keeping a backup next to the file.
fn repair_hosts() -> Result<(), String> {
    let Some(path) = hosts_path() else {
        return Ok(());
    };
    let Ok(text) = fs::read_to_string(&path) else {
        return Ok(());
    };
    let lines: Vec<&str> = text
        .lines()
        .filter(|line| {
            let entry = line.split('#').next().unwrap_or("").trim().to_lowercase();
            !(entry.contains("windowsupdate")
                || entry.contains("update.microsoft")
                || entry.contains("delivery.mp.microsoft"))
        })
        .collect();
    if lines.len() == text.lines().count() {
        return Ok(());
    }
    let backup = path.with_extension("ncduwin-bak");
    fs::copy(&path, &backup).map_err(|err| format!("cannot back up hosts: {err}"))?;
    fs::write(&path, format!("{}\r\n", lines.join("\r\n")))
        .map_err(|err| format!("cannot rewrite hosts: {err}"))
}

fn toggle_tasks(enable: bool) {
    for task in UPDATE_TASKS {
        let _ = run(
            "schtasks.exe",
            &[
                "/Change",
                "/TN",
                task,
                if enable { "/Enable" } else { "/Disable" },
            ],
        );
    }
}

fn enable_updates() -> HelperResponse {
    let mut errors = Vec::new();
    for (key, name) in [
        (AU_KEY, "NoAutoUpdate"),
        (AU_KEY, "UseWUServer"),
        (WU_KEY, "DoNotConnectToWindowsUpdateInternetLocations"),
        (WU_KEY, "DisableWindowsUpdateAccess"),
        (WU_KEY, "SetDisableUXWUAccess"),
        (WU_KEY, "WUServer"),
        (WU_KEY, "WUStatusServer"),
        (LEGACY_KEY, "AUOptions"),
        (LEGACY_KEY, "NoAutoUpdate"),
    ] {
        delete_value(key, name);
    }
    for (service, start) in [
        ("wuauserv", "demand"),
        ("UsoSvc", "auto"),
        ("WaaSMedicSvc", "demand"),
        ("BITS", "delayed-auto"),
    ] {
        if let Err(err) = run_retry("sc.exe", &["config", service, "start=", start]) {
            errors.push(err);
        }
    }
    set_service_start("wuauserv", 3);
    set_service_start("UsoSvc", 2);
    set_service_start("WaaSMedicSvc", 3);
    set_service_start("BITS", 2);
    toggle_tasks(true);
    let _ = run("sc.exe", &["start", "wuauserv"]);
    let _ = run("sc.exe", &["start", "BITS"]);
    if hosts_blocks_updates() {
        if let Err(err) = repair_hosts() {
            errors.push(err);
        }
    }
    // Judge by the state that actually remains, not by individual command
    // exit codes: a transient failure that still leaves updates enabled must
    // not be reported as an error.
    let status = crate::sysopt::query_status();
    let mut response = if status.blocked {
        errors.extend(status.reasons.iter().cloned());
        HelperResponse::failed(errors.join("; "))
    } else {
        // 9 policy deletions + 4 service configs + 4 registry writes +
        // 3 scheduled tasks + 2 service starts.
        HelperResponse::ok_with(22)
    };
    response.needs_reboot = true;
    response
}

fn disable_updates() -> HelperResponse {
    let mut errors = Vec::new();
    for (key, name, value) in [
        (AU_KEY, "NoAutoUpdate", 1u32),
        (WU_KEY, "DoNotConnectToWindowsUpdateInternetLocations", 1),
        (WU_KEY, "DisableWindowsUpdateAccess", 1),
        (WU_KEY, "SetDisableUXWUAccess", 1),
        (LEGACY_KEY, "AUOptions", 1),
        (LEGACY_KEY, "NoAutoUpdate", 1),
    ] {
        if let Err(err) = set_dword(key, name, value) {
            errors.push(err);
        }
    }
    for service in ["wuauserv", "UsoSvc", "WaaSMedicSvc"] {
        if let Err(err) = run_retry("sc.exe", &["config", service, "start=", "disabled"]) {
            errors.push(err);
        }
        set_service_start(service, 4);
    }
    let _ = run("sc.exe", &["stop", "wuauserv"]);
    let _ = run("sc.exe", &["stop", "UsoSvc"]);
    let _ = run("sc.exe", &["stop", "WaaSMedicSvc"]);
    toggle_tasks(false);
    let status = crate::sysopt::query_status();
    let mut response = if status.blocked {
        // 6 registry values + 3 service configs + 3 stops + 3 tasks.
        HelperResponse::ok_with(15)
    } else {
        errors.extend(status.reasons.iter().cloned());
        HelperResponse::failed(errors.join("; "))
    };
    response.needs_reboot = true;
    response
}

fn network_fix() -> HelperResponse {
    let commands: [(&str, &[&str]); 6] = [
        ("netsh.exe", &["winsock", "reset"]),
        ("netsh.exe", &["int", "ip", "reset"]),
        ("netsh.exe", &["int", "ipv6", "reset"]),
        ("netsh.exe", &["interface", "ip", "delete", "arpcache"]),
        ("netsh.exe", &["advfirewall", "reset"]),
        ("ipconfig.exe", &["/flushdns"]),
    ];
    let mut errors = Vec::new();
    for (command, args) in commands {
        if let Err(err) = run(command, args) {
            errors.push(err);
        }
    }
    let reset = r#"Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | Set-DnsClientServerAddress -ResetServerAddresses; Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object { Set-NetIPInterface -InterfaceIndex $_.ifIndex -Dhcp Enabled; Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4 -PrefixOrigin Manual -ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue }"#;
    if let Err(err) = run(
        "powershell.exe",
        &[
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            reset,
        ],
    ) {
        errors.push(err);
    }
    let _ = run("ipconfig.exe", &["/renew"]);
    let mut response = if errors.is_empty() {
        // 6 netsh commands + 1 PowerShell reset + 1 renew.
        HelperResponse::ok_with(8)
    } else {
        HelperResponse::failed(errors.join("; "))
    };
    response.needs_reboot = true;
    response
}

fn flush_dns() -> HelperResponse {
    match run("ipconfig.exe", &["/flushdns"]) {
        Ok(()) => HelperResponse::ok_with(1),
        Err(err) => HelperResponse::failed(err),
    }
}

fn valid_junction_path(path: &str) -> bool {
    let path = Path::new(path);
    path.is_absolute()
        && !path
            .components()
            .any(|component| matches!(component, Component::ParentDir))
}

fn create_junction(source: &str, target: &str) -> HelperResponse {
    if !valid_junction_path(source) || !valid_junction_path(target) {
        return HelperResponse::failed("invalid junction path");
    }
    if !Path::new(target).is_dir() {
        return HelperResponse::failed("the junction target does not exist");
    }
    if Path::new(source).exists() {
        return HelperResponse::failed("the junction source already exists");
    }
    match run("cmd.exe", &["/C", "mklink", "/J", source, target]) {
        Ok(()) => HelperResponse::ok_with(1),
        Err(err) => HelperResponse::failed(err),
    }
}

fn execute(op: HelperOp) -> HelperResponse {
    match op {
        HelperOp::SetUpdateEnabled { enable } => {
            if enable {
                enable_updates()
            } else {
                disable_updates()
            }
        }
        HelperOp::NetworkFix => network_fix(),
        HelperOp::FlushDns => flush_dns(),
        HelperOp::CreateJunction { source, target } => create_junction(&source, &target),
    }
}

/// Creates a junction through the elevated helper, used when a direct
/// `mklink /J` is denied.
pub(crate) fn create_junction_elevated(source: &Path, target: &Path) -> Result<(), String> {
    let outcome = run_elevated(HelperOp::CreateJunction {
        source: source.to_string_lossy().into_owned(),
        target: target.to_string_lossy().into_owned(),
    })?;
    if outcome.ok {
        Ok(())
    } else if outcome.message.is_empty() {
        Err("elevated junction creation failed".to_string())
    } else {
        Err(outcome.message)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn junction_paths_must_be_absolute_and_contain_no_parent_dir() {
        assert!(valid_junction_path(r"C:\Users\me\AppData\Local\App"));
        assert!(!valid_junction_path(r"relative\folder"));
        assert!(!valid_junction_path(r"C:\Users\..\Windows"));
    }
}
