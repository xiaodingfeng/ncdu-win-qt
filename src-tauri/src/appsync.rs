use std::ffi::OsStr;
use std::fs;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, State};
use windows_sys::core::GUID;
use windows_sys::Win32::System::Com::{
    CoInitializeEx, CoTaskMemFree, CoUninitialize, CLSIDFromString, COINIT_APARTMENTTHREADED,
};
use windows_sys::Win32::UI::Shell::{SHGetKnownFolderPath, SHSetKnownFolderPath, KF_FLAG_DEFAULT};

const USER_SHELL_FOLDERS: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;

/// Windows' own library folders — every folder whose location Explorer can move
/// through its Properties > Location tab. Mirrors the old KnownFolderTable.h.
struct KnownFolder {
    id: &'static str,
    name_key: &'static str,
    folder: &'static str,
    guid: &'static str,
    reg_value: &'static str,
    profile_relative: &'static str,
    always_list: bool,
}

const KNOWN_FOLDERS: &[KnownFolder] = &[
    KnownFolder {
        id: "win_download",
        name_key: "app_sync.app.win_download",
        folder: "Downloads",
        guid: "{374DE290-123F-4565-9164-39C4925E467B}",
        reg_value: "{374DE290-123F-4565-9164-39C4925E467B}",
        profile_relative: "Downloads",
        always_list: true,
    },
    KnownFolder {
        id: "win_documents",
        name_key: "app_sync.app.win_documents",
        folder: "Documents",
        guid: "{FDD39AD0-238F-46AF-ADB4-6C85480369C7}",
        reg_value: "Personal",
        profile_relative: "Documents",
        always_list: true,
    },
    KnownFolder {
        id: "win_desktop",
        name_key: "app_sync.app.win_desktop",
        folder: "Desktop",
        guid: "{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}",
        reg_value: "Desktop",
        profile_relative: "Desktop",
        always_list: true,
    },
    KnownFolder {
        id: "win_pictures",
        name_key: "app_sync.app.win_pictures",
        folder: "Pictures",
        guid: "{33E28130-4E1E-4676-835A-98395C3BC3BB}",
        reg_value: "My Pictures",
        profile_relative: "Pictures",
        always_list: true,
    },
    KnownFolder {
        id: "win_videos",
        name_key: "app_sync.app.win_videos",
        folder: "Videos",
        guid: "{18989B1D-99B5-455B-841C-AB7C74E4DDFC}",
        reg_value: "My Video",
        profile_relative: "Videos",
        always_list: true,
    },
    KnownFolder {
        id: "win_music",
        name_key: "app_sync.app.win_music",
        folder: "Music",
        guid: "{4BD8D571-6D19-48D3-BE97-422220080E43}",
        reg_value: "My Music",
        profile_relative: "Music",
        always_list: true,
    },
    KnownFolder {
        id: "win_3dobjects",
        name_key: "app_sync.app.win_3dobjects",
        folder: "3D Objects",
        guid: "{31C0DD25-9439-4F12-BF41-7FF4EDA38722}",
        reg_value: "{31C0DD25-9439-4F12-BF41-7FF4EDA38722}",
        profile_relative: "3D Objects",
        always_list: false,
    },
    KnownFolder {
        id: "win_contacts",
        name_key: "app_sync.app.win_contacts",
        folder: "Contacts",
        guid: "{56784854-C6CB-462B-8169-88E350ACB882}",
        reg_value: "{56784854-C6CB-462B-8169-88E350ACB882}",
        profile_relative: "Contacts",
        always_list: false,
    },
    KnownFolder {
        id: "win_favorites",
        name_key: "app_sync.app.win_favorites",
        folder: "Favorites",
        guid: "{1777F761-68AD-4D8A-87BD-30B759FA33DD}",
        reg_value: "Favorites",
        profile_relative: "Favorites",
        always_list: false,
    },
    KnownFolder {
        id: "win_links",
        name_key: "app_sync.app.win_links",
        folder: "Links",
        guid: "{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}",
        reg_value: "{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}",
        profile_relative: "Links",
        always_list: false,
    },
    KnownFolder {
        id: "win_savedgames",
        name_key: "app_sync.app.win_savedgames",
        folder: "Saved Games",
        guid: "{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}",
        reg_value: "{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}",
        profile_relative: "Saved Games",
        always_list: false,
    },
    KnownFolder {
        id: "win_searches",
        name_key: "app_sync.app.win_searches",
        folder: "Searches",
        guid: "{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}",
        reg_value: "{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}",
        profile_relative: "Searches",
        always_list: false,
    },
];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveFolder {
    pub id: String,
    pub name_key: String,
    /// Language-independent subfolder name used by "move everything to one root".
    pub folder: String,
    pub path: String,
    /// Windows' original location, used by "restore default".
    pub default_path: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SavePair {
    pub from: String,
    pub to: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Leftover {
    pub from: String,
    pub to: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MoveContentsResult {
    pub moved_bytes: u64,
    pub leftovers: Vec<Leftover>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct MoveProgress {
    copied: u64,
    total: u64,
    path: String,
}

#[derive(Default)]
pub struct AppSyncState {
    cancel: Mutex<Option<Arc<AtomicBool>>>,
}

fn wide(value: &OsStr) -> Vec<u16> {
    value.encode_wide().chain(std::iter::once(0)).collect()
}

fn guid_of(text: &str) -> Option<GUID> {
    let wide = wide(OsStr::new(text));
    let mut guid: GUID = unsafe { std::mem::zeroed() };
    let hr = unsafe { CLSIDFromString(wide.as_ptr(), &mut guid) };
    if hr >= 0 {
        Some(guid)
    } else {
        None
    }
}

fn take_wide_string(raw: *mut u16) -> String {
    let mut len = 0usize;
    unsafe {
        while *raw.add(len) != 0 {
            len += 1;
        }
        String::from_utf16_lossy(std::slice::from_raw_parts(raw, len))
    }
}

/// Runs `f` with COM initialised, balancing the uninit exactly.
fn with_com<R>(f: impl FnOnce() -> R) -> R {
    let hr = unsafe { CoInitializeEx(std::ptr::null(), COINIT_APARTMENTTHREADED as u32) };
    let value = f();
    if hr >= 0 {
        unsafe { CoUninitialize() };
    }
    value
}

fn native_path(raw: &str) -> PathBuf {
    PathBuf::from(raw.replace('/', "\\"))
}

fn clean_path(raw: &str) -> String {
    let text = raw.replace('/', "\\");
    let trimmed = text.trim_end_matches('\\');
    if trimmed.is_empty() || (trimmed.len() == 2 && trimmed.as_bytes()[1] == b':') {
        format!("{trimmed}\\")
    } else {
        trimmed.to_string()
    }
}

fn expand_env_vars(value: &str) -> String {
    let chars: Vec<char> = value.chars().collect();
    let mut out = String::with_capacity(value.len());
    let mut index = 0;
    while index < chars.len() {
        if chars[index] == '%' {
            if let Some(end) = (index + 2..chars.len()).find(|&i| chars[i] == '%') {
                let name: String = chars[index + 1..end].iter().collect();
                if let Ok(expanded) = std::env::var(&name) {
                    out.push_str(&expanded);
                    index = end + 1;
                    continue;
                }
            }
        }
        out.push(chars[index]);
        index += 1;
    }
    out
}

fn known_folder_path(entry: &KnownFolder) -> String {
    if let Some(guid) = guid_of(entry.guid) {
        let mut raw: *mut u16 = std::ptr::null_mut();
        let resolved = with_com(|| {
            let hr = unsafe {
                SHGetKnownFolderPath(
                    &guid,
                    KF_FLAG_DEFAULT as u32,
                    std::ptr::null_mut(),
                    &mut raw,
                )
            };
            if hr < 0 || raw.is_null() {
                None
            } else {
                let text = take_wide_string(raw);
                unsafe { CoTaskMemFree(raw as *const _) };
                Some(text)
            }
        });
        if let Some(path) = resolved {
            let native = clean_path(&path);
            if native_path(&native).is_dir() {
                return native;
            }
        }
    }

    if let Ok(key) = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER)
        .open_subkey(USER_SHELL_FOLDERS)
    {
        if let Ok(value) = key.get_value::<String, _>(entry.reg_value) {
            let native = clean_path(&expand_env_vars(&value));
            if native_path(&native).is_dir() {
                return native;
            }
        }
    }

    if let Ok(profile) = std::env::var("USERPROFILE") {
        let fallback = clean_path(&format!("{profile}\\{}", entry.profile_relative));
        if native_path(&fallback).is_dir() {
            return fallback;
        }
    }
    String::new()
}

/// Writes the location the way Explorer's own Location tab does: the registry
/// value is rewritten and the running session picks it up immediately.
fn set_known_folder_path(entry: &KnownFolder, target: &str) -> bool {
    if let Some(guid) = guid_of(entry.guid) {
        let wide = wide(OsStr::new(target));
        let ok = with_com(|| unsafe {
            SHSetKnownFolderPath(
                &guid,
                KF_FLAG_DEFAULT as u32,
                std::ptr::null_mut(),
                wide.as_ptr(),
            ) >= 0
        });
        if ok {
            return true;
        }
    }
    // Fallback: a plain registry write (the change then applies at the next
    // logon or Explorer restart, like editing the value by hand).
    let hkcu = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER);
    match hkcu.create_subkey(USER_SHELL_FOLDERS) {
        Ok((key, _)) => key.set_value(entry.reg_value, &target.to_string()).is_ok(),
        Err(_) => false,
    }
}

/// The default location Windows would use when the folder is not redirected.
fn default_folder_path(entry: &KnownFolder) -> String {
    match std::env::var("USERPROFILE") {
        Ok(profile) => clean_path(&format!("{profile}\\{}", entry.profile_relative)),
        Err(_) => String::new(),
    }
}

#[tauri::command]
pub fn save_folders() -> Vec<SaveFolder> {
    KNOWN_FOLDERS
        .iter()
        .filter_map(|entry| {
            let path = known_folder_path(entry);
            if path.is_empty() && !entry.always_list {
                return None;
            }
            Some(SaveFolder {
                id: entry.id.to_string(),
                name_key: entry.name_key.to_string(),
                folder: entry.folder.to_string(),
                path,
                default_path: default_folder_path(entry),
            })
        })
        .collect()
}

#[tauri::command]
pub fn set_save_folder(id: String, new_path: String) -> Result<(), String> {
    let entry = KNOWN_FOLDERS
        .iter()
        .find(|entry| entry.id == id)
        .ok_or_else(|| "app_sync.fail".to_string())?;
    let target = clean_path(&new_path);
    if target.is_empty() {
        return Err("app_sync.fail".to_string());
    }
    let dir = native_path(&target);
    if !dir.is_absolute() {
        return Err("app_sync.fail".to_string());
    }
    fs::create_dir_all(&dir).map_err(|_| "app_sync.fail".to_string())?;
    if set_known_folder_path(entry, &target) {
        Ok(())
    } else {
        Err("app_sync.fail".to_string())
    }
}

struct MoveContext {
    app: AppHandle,
    cancel: Arc<AtomicBool>,
    copied: Arc<AtomicU64>,
    total: u64,
    last_emit: Mutex<Instant>,
}

impl MoveContext {
    fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }

    fn add(&self, bytes: u64, path: &Path) {
        let copied = self.copied.fetch_add(bytes, Ordering::Relaxed) + bytes;
        let mut last = match self.last_emit.lock() {
            Ok(last) => last,
            Err(_) => return,
        };
        if last.elapsed() < Duration::from_millis(150) {
            return;
        }
        *last = Instant::now();
        let _ = self.app.emit(
            "appsync://progress",
            MoveProgress {
                copied,
                total: self.total,
                path: path.to_string_lossy().into_owned(),
            },
        );
    }
}

fn dir_size_no_reparse(path: &Path) -> u64 {
    let mut total = 0u64;
    let mut pending = vec![path.to_path_buf()];
    while let Some(current) = pending.pop() {
        let Ok(entries) = fs::read_dir(&current) else {
            continue;
        };
        for entry in entries.flatten() {
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            let attrs = std::os::windows::fs::MetadataExt::file_attributes(&metadata);
            if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
                continue;
            }
            if metadata.is_dir() {
                pending.push(entry.path());
            } else {
                total += metadata.len();
            }
        }
    }
    total
}

/// Moves the contents of `src` into `dst`, removing every source file once its
/// copy is confirmed — Explorer's move semantics. Reparse points stay put.
fn move_recursive(src: &Path, dst: &Path, ctx: &MoveContext) {
    if ctx.cancelled() {
        return;
    }
    let Ok(entries) = fs::read_dir(src) else {
        return;
    };
    for entry in entries.flatten() {
        if ctx.cancelled() {
            return;
        }
        let child_src = entry.path();
        let child_dst = dst.join(entry.file_name());
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        let attrs = std::os::windows::fs::MetadataExt::file_attributes(&metadata);
        if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            continue;
        }
        if metadata.is_dir() {
            if fs::create_dir_all(&child_dst).is_err() {
                continue;
            }
            move_recursive(&child_src, &child_dst, ctx);
            let _ = fs::remove_dir(&child_src);
        } else {
            let size = metadata.len();
            // Only drop the source file once the copy exists with the same size.
            match fs::copy(&child_src, &child_dst) {
                Ok(copied) if copied == size => {
                    if fs::remove_file(&child_src).is_ok() {
                        ctx.add(size, &child_src);
                    }
                }
                _ => {}
            }
        }
    }
}

fn run_move(app: AppHandle, pairs: Vec<SavePair>, cancel: Arc<AtomicBool>) -> MoveContentsResult {
    let mut usable = Vec::new();
    let mut total = 0u64;
    for pair in pairs {
        let from = native_path(&clean_path(&pair.from));
        let to = native_path(&clean_path(&pair.to));
        if !from.is_dir() || !to.is_absolute() {
            continue;
        }
        let from_key = clean_path(&pair.from).to_lowercase();
        let to_key = clean_path(&pair.to).to_lowercase();
        if from_key == to_key
            || to_key.starts_with(&format!("{from_key}\\"))
            || from_key.starts_with(&format!("{to_key}\\"))
        {
            continue;
        }
        total += dir_size_no_reparse(&from);
        usable.push((from, to));
    }

    let ctx = MoveContext {
        app: app.clone(),
        cancel: cancel.clone(),
        copied: Arc::new(AtomicU64::new(0)),
        total,
        last_emit: Mutex::new(Instant::now() - Duration::from_secs(1)),
    };

    for (from, to) in &usable {
        if ctx.cancelled() {
            break;
        }
        if fs::create_dir_all(to).is_err() {
            continue;
        }
        move_recursive(from, to, &ctx);
        let _ = fs::remove_dir(from);
    }

    let leftovers = usable
        .iter()
        .filter(|(from, _)| {
            from.is_dir()
                && fs::read_dir(from)
                    .map(|mut entries| entries.next().is_some())
                    .unwrap_or(false)
        })
        .map(|(from, to)| Leftover {
            from: from.to_string_lossy().into_owned(),
            to: to.to_string_lossy().into_owned(),
        })
        .collect::<Vec<_>>();

    for (_, to) in &usable {
        if to.is_dir() {
            crate::locker::write_dont_delete_marker(&to.to_string_lossy());
        }
    }

    let copied = ctx.copied.load(Ordering::Relaxed);
    let _ = app.emit(
        "appsync://progress",
        MoveProgress {
            copied,
            total: total.max(copied),
            path: String::new(),
        },
    );

    MoveContentsResult {
        moved_bytes: copied,
        leftovers,
    }
}

#[tauri::command]
pub async fn move_save_contents(
    app: AppHandle,
    state: State<'_, AppSyncState>,
    pairs: Vec<SavePair>,
) -> Result<MoveContentsResult, String> {
    if pairs.is_empty() {
        return Ok(MoveContentsResult {
            moved_bytes: 0,
            leftovers: Vec::new(),
        });
    }
    let cancel = Arc::new(AtomicBool::new(false));
    {
        let mut slot = state.cancel.lock().map_err(|_| "app_sync.fail".to_string())?;
        *slot = Some(cancel.clone());
    }
    let handle = tauri::async_runtime::spawn_blocking({
        let app = app.clone();
        let cancel = cancel.clone();
        move || run_move(app, pairs, cancel)
    });
    let result = handle.await.map_err(|err| err.to_string())?;
    if let Ok(mut slot) = state.cancel.lock() {
        *slot = None;
    }
    Ok(result)
}

#[tauri::command]
pub fn cancel_save_move(state: State<'_, AppSyncState>) -> Result<(), String> {
    if let Ok(slot) = state.cancel.lock() {
        if let Some(cancel) = slot.as_ref() {
            cancel.store(true, Ordering::Relaxed);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn env_vars_expand_like_windows_does() {
        std::env::set_var("NCDU_APPSYNC_TEST", r"C:\Temp");
        assert_eq!(
            expand_env_vars(r"%NCDU_APPSYNC_TEST%\Downloads"),
            r"C:\Temp\Downloads"
        );
        assert_eq!(expand_env_vars("no tokens"), "no tokens");
    }

    #[test]
    fn paths_drop_trailing_separators_but_keep_roots() {
        assert_eq!(clean_path("C:\\Data\\"), "C:\\Data");
        assert_eq!(clean_path("C:/Data/Sub/"), "C:\\Data\\Sub");
        assert_eq!(clean_path("C:\\"), "C:\\");
    }

    #[test]
    fn known_folder_table_is_unique_and_complete() {
        let mut ids: Vec<&str> = KNOWN_FOLDERS.iter().map(|entry| entry.id).collect();
        let count = ids.len();
        ids.sort_unstable();
        ids.dedup();
        assert_eq!(ids.len(), count);
        assert!(KNOWN_FOLDERS
            .iter()
            .all(|entry| entry.name_key.starts_with("app_sync.app.")));
    }

    #[test]
    #[ignore = "reads the live user profile"]
    fn known_folders_resolve_on_this_machine() {
        let folders = save_folders();
        assert!(folders.iter().any(|folder| folder.id == "win_documents"));
        for folder in &folders {
            if folder.path.is_empty() {
                assert!(folder.id == "win_3dobjects", "{} came back empty", folder.id);
            }
        }
    }

    #[test]
    #[ignore = "calls SHSetKnownFolderPath on the live profile (same path, no change)"]
    fn writing_the_current_path_back_succeeds() {
        let entry = KNOWN_FOLDERS
            .iter()
            .find(|entry| entry.id == "win_documents")
            .expect("documents entry");
        let current = known_folder_path(entry);
        assert!(!current.is_empty(), "documents folder not found");
        assert!(set_known_folder_path(entry, &current));
    }
}
