use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::process::Command;

use serde::Serialize;
use tauri::State;
use windows_sys::Win32::UI::Shell::{
    ShellExecuteW, SHFileOperationW, SHFILEOPSTRUCTW, SHEmptyRecycleBinW, SHQueryRecycleBinW,
    SHQUERYRBINFO, FO_DELETE, FOF_ALLOWUNDO, FOF_NOCONFIRMATION, FOF_NOERRORUI, FOF_SILENT,
};

use crate::locker::{self, LockingProcess};
use crate::scan::{self, ScanStore};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteFailure {
    pub path: String,
    pub message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub locked_by: Option<Vec<LockingProcess>>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteResult {
    pub deleted: usize,
    pub deleted_paths: Vec<String>,
    pub failed: Vec<DeleteFailure>,
}

fn wide(value: &OsStr) -> Vec<u16> {
    value.encode_wide().chain(std::iter::once(0)).collect()
}

pub(crate) fn is_protected(path: &str) -> bool {
    let trimmed = path.trim().trim_end_matches(['\\', '/']);
    trimmed.is_empty()
        || (trimmed.len() == 2 && trimmed.as_bytes()[1] == b':')
        || trimmed == "\\\\"
}

fn clear_readonly(path: &Path) {
    let metadata = match std::fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(_) => return,
    };
    if metadata.file_type().is_symlink() {
        return;
    }
    let mut permissions = metadata.permissions();
    if permissions.readonly() {
        permissions.set_readonly(false);
        let _ = std::fs::set_permissions(path, permissions);
    }
    if metadata.is_dir() {
        let mut pending = vec![path.to_path_buf()];
        while let Some(current) = pending.pop() {
            let Ok(entries) = std::fs::read_dir(&current) else {
                continue;
            };
            for entry in entries.flatten() {
                let Ok(child_meta) = entry.metadata() else {
                    continue;
                };
                if child_meta.file_type().is_symlink() {
                    continue;
                }
                let mut child_perms = child_meta.permissions();
                if child_perms.readonly() {
                    child_perms.set_readonly(false);
                    let _ = std::fs::set_permissions(entry.path(), child_perms);
                }
                if child_meta.is_dir() {
                    pending.push(entry.path());
                }
            }
        }
    }
}

pub(crate) fn move_to_recycle_bin(paths: &[String]) -> Result<(), String> {
    let mut from: Vec<u16> = Vec::new();
    for path in paths {
        from.extend(OsStr::new(path).encode_wide());
        from.push(0);
    }
    from.push(0);

    let mut operation: SHFILEOPSTRUCTW = unsafe { std::mem::zeroed() };
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.as_ptr() as *mut u16;
    operation.fFlags = (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI) as u16;

    let code = unsafe { SHFileOperationW(&mut operation) };
    if code == 0 && operation.fAnyOperationsAborted == 0 {
        Ok(())
    } else {
        Err(format!("recycle failed (code {code})"))
    }
}

pub(crate) fn permanent_delete(path: &str) -> Result<(), String> {
    if is_protected(path) {
        return Err("refusing to delete a top-level drive or folder".to_string());
    }
    let target = Path::new(path);
    let metadata = std::fs::symlink_metadata(target).map_err(|err| err.to_string())?;
    clear_readonly(target);
    if metadata.is_dir() && !metadata.file_type().is_symlink() {
        std::fs::remove_dir_all(target).map_err(|err| err.to_string())
    } else {
        std::fs::remove_file(target).map_err(|err| err.to_string())
    }
}

/// Size and item count of one drive's recycle bin, mirroring
/// WinApi::queryRecycleBin in the Qt build.
pub(crate) fn query_recycle_bin(drive_root: &str) -> (u64, u32) {
    let mut info: SHQUERYRBINFO = unsafe { std::mem::zeroed() };
    info.cbSize = std::mem::size_of::<SHQUERYRBINFO>() as u32;
    let path = wide(OsStr::new(drive_root));
    let code = unsafe { SHQueryRecycleBinW(path.as_ptr(), &mut info) };
    if code != 0 {
        return (0, 0);
    }
    (info.i64Size.max(0) as u64, info.i64NumItems.max(0) as u32)
}

/// Empties one drive's recycle bin without any confirmation UI.
pub(crate) fn empty_recycle_bin(drive_root: &str) -> bool {
    const SHERB_NOCONFIRMATION: u32 = 0x1;
    const SHERB_NOPROGRESSUI: u32 = 0x2;
    const SHERB_NOSOUND: u32 = 0x4;
    let path = wide(OsStr::new(drive_root));
    let code = unsafe {
        SHEmptyRecycleBinW(
            std::ptr::null_mut(),
            path.as_ptr(),
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND,
        )
    };
    code >= 0
}

/// Removes entries that are already covered by a selected ancestor so the
/// recycle operation does not process the same subtree twice.
fn dedupe(paths: &[String]) -> Vec<String> {
    let mut sorted: Vec<String> = paths
        .iter()
        .map(|path| path.trim().to_string())
        .filter(|path| !path.is_empty())
        .collect();
    sorted.sort_by_key(|path| path.to_lowercase().len());
    let mut result: Vec<String> = Vec::new();
    for path in sorted {
        let lower = path.to_lowercase();
        let covered = result.iter().any(|parent| {
            let parent_lower = parent.to_lowercase();
            lower == parent_lower
                || lower.starts_with(&format!("{parent_lower}\\"))
                || lower.starts_with(&format!("{parent_lower}/"))
        });
        if !covered {
            result.push(path);
        }
    }
    result
}

#[tauri::command]
pub fn delete_paths(
    store: State<'_, ScanStore>,
    paths: Vec<String>,
    permanent: bool,
) -> Result<DeleteResult, String> {
    if paths.is_empty() {
        return Err("no paths selected".to_string());
    }
    log::info!(
        "delete {} path(s) ({})",
        paths.len(),
        if permanent { "permanent" } else { "recycle" }
    );

    let candidates = dedupe(&paths);
    let mut result = DeleteResult {
        deleted: 0,
        deleted_paths: Vec::new(),
        failed: Vec::new(),
    };

    if permanent {
        for path in candidates {
            if is_protected(&path) {
                result.failed.push(DeleteFailure {
                    path,
                    message: "refusing to delete a top-level drive or folder".to_string(),
                    locked_by: None,
                });
                continue;
            }
            match permanent_delete(&path) {
                Ok(()) => {
                    result.deleted += 1;
                    result.deleted_paths.push(path);
                }
                Err(message) => result.failed.push(DeleteFailure {
                    path,
                    message,
                    locked_by: None,
                }),
            }
        }
    } else {
        let deletable: Vec<String> = candidates
            .iter()
            .filter(|path| !is_protected(path))
            .cloned()
            .collect();
        for path in candidates.iter().filter(|path| is_protected(path)) {
            result.failed.push(DeleteFailure {
                path: path.clone(),
                message: "refusing to delete a top-level drive or folder".to_string(),
                locked_by: None,
            });
        }
        if !deletable.is_empty() {
            match move_to_recycle_bin(&deletable) {
                Ok(()) => {
                    result.deleted = deletable.len();
                    result.deleted_paths = deletable;
                }
                Err(message) => {
                    for path in deletable {
                        result.failed.push(DeleteFailure {
                            path,
                            message: message.clone(),
                            locked_by: None,
                        });
                    }
                }
            }
        }
    }

    if !result.deleted_paths.is_empty() {
        scan::remove_paths(&store, &result.deleted_paths);
    }
    // Name the programs holding what could not be deleted, so the UI can offer
    // to close them instead of leaving the user with a bare "failed" message.
    for failure in result.failed.iter_mut().take(4) {
        if failure.message.to_lowercase().contains("refusing") {
            continue;
        }
        let procs = locker::processes_locking_path(&failure.path, 200);
        if !procs.is_empty() {
            failure.locked_by = Some(procs);
        }
    }
    Ok(result)
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DirItem {
    pub name: String,
    pub path: String,
    pub size: u64,
    pub is_dir: bool,
}

/// First `limit` children of a folder, largest first. Used by the cleanup
/// details dialog the same way the Qt panel's "view contents" does.
#[tauri::command]
pub fn list_dir(path: String, limit: Option<usize>) -> Result<Vec<DirItem>, String> {
    let limit = limit.unwrap_or(50);
    let entries = std::fs::read_dir(&path).map_err(|err| err.to_string())?;
    let mut items: Vec<DirItem> = entries
        .flatten()
        .filter_map(|entry| {
            let meta = entry.metadata().ok()?;
            Some(DirItem {
                name: entry.file_name().to_string_lossy().into_owned(),
                path: entry.path().to_string_lossy().into_owned(),
                size: meta.len(),
                is_dir: meta.is_dir(),
            })
        })
        .collect();
    items.sort_by(|a, b| b.size.cmp(&a.size));
    items.truncate(limit);
    Ok(items)
}

#[tauri::command]
pub fn open_path(path: String) -> Result<(), String> {
    let file = wide(OsStr::new(&path));
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            std::ptr::null(),
            file.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
        )
    };
    if result as isize > 32 {
        Ok(())
    } else {
        Err(format!("cannot open: {path}"))
    }
}

#[tauri::command]
pub fn reveal_path(path: String) -> Result<(), String> {
    Command::new("explorer")
        .arg(format!("/select,{path}"))
        .spawn()
        .map(|_| ())
        .map_err(|err| err.to_string())
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileTimes {
    pub created: Option<u64>,
    pub modified: Option<u64>,
    pub accessed: Option<u64>,
}

fn millis(time: std::io::Result<std::time::SystemTime>) -> Option<u64> {
    time.ok()
        .and_then(|value| value.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|value| value.as_millis() as u64)
}

#[tauri::command]
pub fn file_times(path: String) -> Result<FileTimes, String> {
    let metadata = std::fs::metadata(&path).map_err(|err| err.to_string())?;
    Ok(FileTimes {
        created: millis(metadata.created()),
        modified: millis(metadata.modified()),
        accessed: millis(metadata.accessed()),
    })
}

