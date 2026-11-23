use std::collections::HashSet;
use std::fs;
use std::os::windows::fs::MetadataExt;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter};
use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

use crate::locker;

const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const OLD_SUFFIX: &str = ".ncduwin-old";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const CREATE_UNICODE_ENVIRONMENT: u32 = 0x0400_0000;

const SKIP_NAMES: &[&str] = &[
    "microsoft",
    "packages",
    "temp",
    "tmp",
    "programs",
    "connecteddevicesplatform",
    "comms",
    "d3dscache",
    "publishers",
    "virtualstore",
    "placeholdertilelogofolder",
    "elevateddiagnostics",
    "application data",
    "history",
    "temporary internet files",
];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppDir {
    pub path: String,
    pub name: String,
    pub size: u64,
    pub linked: bool,
    pub link_target: String,
    pub moved: bool,
    pub installed: bool,
    pub residue: bool,
    pub backup: String,
    pub state: String,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct JournalEntry {
    pub source: String,
    pub target: String,
    pub phase: String,
    pub backup: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Journal {
    pub entries: Vec<JournalEntry>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MoveProgress {
    pub index: u32,
    pub total: u32,
    pub path: String,
    pub phase: String,
    pub percent: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MoveItemResult {
    pub path: String,
    pub ok: bool,
    pub message: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MoveFinished {
    pub moved: u32,
    pub failed: u32,
    pub items: Vec<MoveItemResult>,
}

fn journal_path() -> PathBuf {
    let base = std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("cn.fengzhengx.ncduwin").join("move-journal.json")
}

/// The old Qt build wrote `moved_dirs.json` into its own AppData folder, in a
/// path-keyed object shape. Those entries still describe live junctions.
fn legacy_journal_path() -> PathBuf {
    let base = std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("NcduWin").join("moved_dirs.json")
}

fn legacy_entry(value: &serde_json::Value) -> Option<JournalEntry> {
    let source = value.get("path")?.as_str()?.trim();
    let target = value.get("target")?.as_str()?.trim();
    if source.is_empty() || target.is_empty() {
        return None;
    }
    let phase = value.get("phase").and_then(|item| item.as_str()).unwrap_or("");
    // Older entries only carried a target; they were written after the copy
    // had been verified, so "linked" is the right reading.
    let phase = match phase {
        "" | "linked" | "residue" => "linked",
        _ => return None,
    };
    Some(JournalEntry {
        source: source.to_string(),
        target: target.to_string(),
        phase: phase.to_string(),
        backup: value
            .get("old")
            .or_else(|| value.get("backup"))
            .and_then(|item| item.as_str())
            .unwrap_or("")
            .to_string(),
    })
}

fn load_legacy_entries() -> Vec<JournalEntry> {
    let Ok(text) = fs::read_to_string(legacy_journal_path()) else {
        return Vec::new();
    };
    // The old build wrote a JSON array in newer releases and a path-keyed
    // object in older ones; accept both shapes.
    let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) else {
        return Vec::new();
    };
    match value {
        serde_json::Value::Array(items) => items.iter().filter_map(legacy_entry).collect(),
        serde_json::Value::Object(map) => map.values().filter_map(legacy_entry).collect(),
        _ => Vec::new(),
    }
}

/// Custom scan roots remembered by the old build, so moved folders outside the
/// standard AppData folders are found again after the upgrade.
fn legacy_custom_roots() -> Vec<PathBuf> {
    let key = RegKey::predef(HKEY_CURRENT_USER).open_subkey(r"Software\NcduWin");
    let Ok(key) = key else {
        return Vec::new();
    };
    if let Ok(list) = key.get_value::<Vec<String>, _>("MoveScanCustomDirs") {
        return list
            .into_iter()
            .map(PathBuf::from)
            .filter(|path| path.is_absolute())
            .collect();
    }
    key.get_value::<String, _>("MoveScanCustomDirs")
        .ok()
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .into_iter()
        .collect()
}

fn read_journal(path: &Path) -> Journal {
    let Ok(text) = fs::read_to_string(path) else {
        return Journal::default();
    };
    serde_json::from_str(&text).unwrap_or_default()
}

fn load_journal() -> Journal {
    let mut journal = read_journal(&journal_path());
    let legacy = load_legacy_entries();
    if legacy.is_empty() {
        return journal;
    }
    let known: HashSet<String> = journal
        .entries
        .iter()
        .map(|entry| normalize(&entry.source))
        .collect();
    for entry in legacy {
        if !known.contains(&normalize(&entry.source)) {
            journal.entries.push(entry);
        }
    }
    journal
}

fn link_target(path: &Path) -> String {
    fs::read_link(path)
        .ok()
        .map(|value| strip_verbatim(&value.to_string_lossy()))
        .or_else(|| {
            fs::canonicalize(path)
                .ok()
                .map(|value| strip_verbatim(&value.to_string_lossy()))
        })
        .unwrap_or_default()
}

fn save_journal(journal: &Journal) {
    let path = journal_path();
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Ok(text) = serde_json::to_string_pretty(journal) {
        let temp = path.with_extension("json.tmp");
        if fs::write(&temp, text).is_ok() {
            let _ = fs::rename(&temp, &path);
        }
    }
}

fn normalize(path: &str) -> String {
    let mut text = path.replace('\\', "/").to_lowercase();
    while text.len() > 3 && text.ends_with('/') {
        text.pop();
    }
    text
}

fn is_reparse(path: &Path) -> bool {
    fs::symlink_metadata(path)
        .map(|meta| meta.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0)
        .unwrap_or(false)
}

/// Removes the `\\?\` / `\??\` prefixes produced by `canonicalize` and junction
/// reads, which robocopy rejects with "invalid parameter" (exit code 16).
fn strip_verbatim(raw: &str) -> String {
    if let Some(rest) = raw.strip_prefix(r"\\?\UNC\") {
        format!(r"\\{rest}")
    } else if let Some(rest) = raw.strip_prefix(r"\\?\") {
        rest.to_string()
    } else if let Some(rest) = raw.strip_prefix(r"\??\") {
        rest.to_string()
    } else {
        raw.to_string()
    }
}

fn dir_stats(path: &Path) -> (u64, u64) {
    let Ok(meta) = fs::symlink_metadata(path) else {
        return (0, 0);
    };
    if !meta.is_dir() || is_reparse(path) {
        return if meta.is_file() { (meta.len(), 1) } else { (0, 0) };
    }
    let mut bytes = 0u64;
    let mut files = 0u64;
    let mut pending = vec![path.to_path_buf()];
    while let Some(current) = pending.pop() {
        let Ok(entries) = fs::read_dir(&current) else {
            continue;
        };
        for entry in entries.flatten() {
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            if meta.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
                continue;
            }
            if meta.is_dir() {
                pending.push(entry.path());
            } else {
                bytes += meta.len();
                files += 1;
            }
        }
    }
    (bytes, files)
}

fn dir_size(path: &Path) -> u64 {
    dir_stats(path).0
}

fn drive_of(path: &str) -> String {
    let text = path.replace('/', "\\");
    if text.len() >= 2 && text.as_bytes()[1] == b':' {
        text[..2].to_lowercase()
    } else if text.starts_with("\\\\") {
        "unc".to_string()
    } else {
        String::new()
    }
}

fn default_roots() -> Vec<PathBuf> {
    let roaming = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_default();
    let local = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_default();
    let mut roots = Vec::new();
    if !roaming.as_os_str().is_empty() {
        roots.push(roaming);
    }
    if !local.as_os_str().is_empty() {
        roots.push(local.clone());
        let low = local
            .parent()
            .map(|parent| parent.join("LocalLow"))
            .unwrap_or_else(|| local.join("LocalLow"));
        if low.exists() {
            roots.push(low);
        }
    }
    if let Some(user) = std::env::var_os("USERPROFILE") {
        roots.push(PathBuf::from(user).join("Documents"));
    }
    roots
}

fn installed_programs() -> Vec<String> {
    let mut names = Vec::new();
    let paths = [
        (HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        (HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        (HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    ];
    for (root, path) in paths {
        let Ok(key) = RegKey::predef(root).open_subkey_with_flags(path, KEY_READ) else {
            continue;
        };
        for name in key.enum_keys().flatten() {
            let Ok(sub) = key.open_subkey_with_flags(&name, KEY_READ) else {
                continue;
            };
            if let Ok(display) = sub.get_value::<String, _>("DisplayName") {
                if !display.trim().is_empty() {
                    names.push(display);
                }
            }
        }
    }
    names
}

fn normalize_name(value: &str) -> String {
    value
        .to_lowercase()
        .chars()
        .filter(|ch| ch.is_alphanumeric())
        .collect()
}

fn matches_installed(folder: &str, installed: &[String]) -> bool {
    let target = normalize_name(folder);
    if target.len() < 3 {
        return false;
    }
    installed.iter().any(|name| {
        let installed = normalize_name(name);
        installed.contains(&target) || target.contains(&installed)
    })
}

fn collect_dirs(root: &Path) -> Vec<AppDir> {
    let Ok(entries) = fs::read_dir(root) else {
        return Vec::new();
    };
    let paths: Vec<PathBuf> = entries
        .flatten()
        .filter_map(|entry| {
            let path = entry.path();
            // Junctions/symlinks are directories too, but `DirEntry::metadata`
            // may report the link itself; keep everything that is either a
            // real directory or a reparse point.
            let is_dir = fs::symlink_metadata(&path)
                .map(|meta| meta.is_dir())
                .unwrap_or(false);
            if is_dir || is_reparse(&path) {
                Some(path)
            } else {
                None
            }
        })
        .collect();
    paths
        .par_iter()
        .filter_map(|path| {
            let name = path.file_name()?.to_string_lossy().into_owned();
            let lower = name.to_lowercase();
            if is_reparse(path) {
                let target = link_target(path);
                return Some(AppDir {
                    path: path.to_string_lossy().into_owned(),
                    name,
                    size: dir_size(Path::new(&target)),
                    linked: true,
                    link_target: target,
                    moved: true,
                    installed: false,
                    residue: false,
                    backup: String::new(),
                    state: "moved".to_string(),
                    error: None,
                });
            }
            if SKIP_NAMES.contains(&lower.as_str())
                || lower == "ncduwin"
                || lower == "cn.fengzhengx.ncduwin"
                || lower.starts_with('.')
            {
                return None;
            }
            Some(AppDir {
                path: path.to_string_lossy().into_owned(),
                name,
                size: dir_size(path),
                linked: false,
                link_target: String::new(),
                moved: false,
                installed: false,
                residue: false,
                backup: String::new(),
                state: "normal".to_string(),
                error: None,
            })
        })
        .collect()
}

#[tauri::command]
pub fn appmove_default_roots() -> Result<Vec<String>, String> {
    Ok(default_roots()
        .into_iter()
        .map(|path| path.to_string_lossy().into_owned())
        .collect())
}

#[tauri::command]
pub async fn scan_app_dirs(custom_roots: Option<Vec<String>>) -> Result<Vec<AppDir>, String> {
    // Directory sizing walks the whole tree, so it must not run on the UI
    // thread (a synchronous command blocks the WebView and freezes the app).
    tauri::async_runtime::spawn_blocking(move || scan_app_dirs_inner(custom_roots))
        .await
        .map_err(|err| err.to_string())?
}

fn scan_app_dirs_inner(custom_roots: Option<Vec<String>>) -> Result<Vec<AppDir>, String> {
    let roots: Vec<PathBuf> = match custom_roots {
        Some(list) if !list.is_empty() => list.into_iter().map(PathBuf::from).collect(),
        _ => {
            let mut roots = default_roots();
            for legacy in legacy_custom_roots() {
                let key = normalize(&legacy.to_string_lossy());
                if !roots
                    .iter()
                    .any(|root| normalize(&root.to_string_lossy()) == key)
                {
                    roots.push(legacy);
                }
            }
            roots
        }
    };
    if roots.is_empty() {
        return Ok(Vec::new());
    }
    let installed = installed_programs();
    let journal = load_journal();

    let mut seen: HashSet<String> = HashSet::new();
    let mut dirs: Vec<AppDir> = Vec::new();

    for root in &roots {
        let mut entries = collect_dirs(root);
        for dir in entries.iter_mut() {
            dir.installed = matches_installed(&dir.name, &installed);
            seen.insert(normalize(&dir.path));
        }
        dirs.extend(entries);

        // Anything left behind by an interrupted move.
        if let Ok(entries) = fs::read_dir(root) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().into_owned();
                let lower = name.to_lowercase();
                if !lower.contains(OLD_SUFFIX) {
                    continue;
                }
                let path = entry.path();
                if !path.is_dir() {
                    continue;
                }
                let key = normalize(&path.to_string_lossy());
                if !seen.insert(key) {
                    continue;
                }
                let base = lower
                    .split(OLD_SUFFIX)
                    .next()
                    .unwrap_or(&lower)
                    .to_string();
                dirs.push(AppDir {
                    path: path.to_string_lossy().into_owned(),
                    name: base,
                    size: dir_size(&path),
                    linked: false,
                    link_target: String::new(),
                    moved: false,
                    installed: false,
                    residue: true,
                    backup: String::new(),
                    state: "residue".to_string(),
                    error: None,
                });
            }
        }
    }

    // Junctioned app folders, including ones whose source root was never
    // enumerated above (custom roots, or a root that is now gone).
    for entry in &journal.entries {
        if entry.phase != "linked" {
            continue;
        }
        if seen.contains(&normalize(&entry.source)) {
            continue;
        }
        let source = Path::new(&entry.source);
        if !source.exists() {
            continue;
        }
        let target = if entry.target.is_empty() {
            link_target(source)
        } else {
            entry.target.clone()
        };
        let name = source
            .file_name()
            .map(|value| value.to_string_lossy().into_owned())
            .unwrap_or_else(|| entry.source.clone());
        dirs.push(AppDir {
            path: entry.source.clone(),
            name,
            size: dir_size(Path::new(&target)),
            linked: true,
            link_target: target,
            moved: true,
            installed: false,
            residue: false,
            backup: entry.backup.clone(),
            state: "moved".to_string(),
            error: None,
        });
    }

    // Mark junctions and compute their real size.
    for dir in dirs.iter_mut() {
        let path = Path::new(&dir.path);
        if dir.state == "moved" || !path.exists() {
            continue;
        }
        if is_reparse(path) {
            dir.linked = true;
            dir.moved = true;
            dir.state = "moved".to_string();
            dir.link_target = link_target(path);
            dir.size = dir_size(Path::new(&dir.link_target));
        }
    }

    dirs.retain(|dir| {
        dir.state == "moved"
            || dir.state == "residue"
            || drive_of(&dir.path) == drive_of(&roots[0].to_string_lossy())
    });
    dirs.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    Ok(dirs)
}

// ---------------------------------------------------------------------------
// Move / restore / residue cleanup
// ---------------------------------------------------------------------------

fn robocopy() -> PathBuf {
    let windir = std::env::var_os("WINDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("C:/Windows"));
    windir.join("System32/robocopy.exe")
}

fn run_robocopy(source: &Path, destination: &Path) -> Result<(), String> {
    let source = PathBuf::from(strip_verbatim(&source.to_string_lossy()));
    let destination = PathBuf::from(strip_verbatim(&destination.to_string_lossy()));
    let _ = fs::create_dir_all(&destination);
    let status = Command::new(robocopy())
        .arg(&source)
        .arg(&destination)
        .args([
            "/E", "/COPY:DT", "/XJ", "/R:1", "/W:1", "/MT:16", "/NFL", "/NDL", "/NJH", "/NJS",
        ])
        .creation_flags(CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT)
        .status()
        .map_err(|err| format!("cannot start robocopy: {err}"))?;
    let code = status.code().unwrap_or(16);
    if code & 0x1C != 0 {
        return Err(format!("robocopy failed with exit code {code}"));
    }
    Ok(())
}

/// Drops a source tree that has already been copied. Sending it to the Recycle
/// Bin is a single rename per item, which is far faster than deleting every
/// file (and keeps the Qt build's recoverable behaviour).
fn discard_tree(path: &Path) -> bool {
    let text = path.to_string_lossy().into_owned();
    if crate::ops::move_to_recycle_bin(&[text]).is_ok() {
        return true;
    }
    fs::remove_dir_all(path).is_ok()
}

fn create_junction(link: &Path, target: &Path) -> Result<(), String> {
    let output = Command::new("cmd.exe")
        .args(["/C", "mklink", "/J"])
        .arg(link)
        .arg(target)
        .creation_flags(CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT)
        .output()
        .map_err(|err| format!("cannot start mklink: {err}"))?;
    if output.status.success() {
        return Ok(());
    }
    let direct = String::from_utf8_lossy(&output.stderr).trim().to_string();
    // mklink /J is usually allowed for the current user; when policy denies it,
    // retry once through the elevated helper (one UAC prompt).
    match crate::elevate::create_junction_elevated(link, target) {
        Ok(()) => Ok(()),
        Err(elevated) => Err(if direct.is_empty() { elevated } else { format!("{direct}; {elevated}") }),
    }
}

fn remove_junction(path: &Path) -> Result<(), String> {
    if fs::remove_dir(path).is_ok() {
        return Ok(());
    }
    let output = Command::new("cmd.exe")
        .args(["/C", "rmdir"])
        .arg(path)
        .creation_flags(CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT)
        .output()
        .map_err(|err| format!("cannot start rmdir: {err}"))?;
    if output.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).trim().to_string())
    }
}

fn timestamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_millis() as u64)
        .unwrap_or_default()
}

fn move_one(source: &Path, target_root: &Path, journal: &mut Journal) -> Result<String, String> {
    if !source.exists() {
        return Err("app_move.fail_missing".to_string());
    }
    if is_reparse(source) {
        return Err("app_move.fail_already_moved".to_string());
    }
    let name = source
        .file_name()
        .ok_or_else(|| "app_move.fail_invalid".to_string())?
        .to_string_lossy()
        .into_owned();
    let parent = source
        .parent()
        .and_then(|path| path.file_name())
        .map(|value| value.to_string_lossy().into_owned())
        .unwrap_or_else(|| "AppData".to_string());
    let target = target_root.join(&parent).join(&name);
    if target.exists() && fs::read_dir(&target).map(|mut dir| dir.next().is_some()).unwrap_or(false) {
        return Err("app_move.fail_target_exists".to_string());
    }
    if normalize(&target.to_string_lossy()).starts_with(&format!("{}/", normalize(&source.to_string_lossy()))) {
        return Err("app_move.fail_invalid".to_string());
    }

    // Journal first: an interrupted copy can always be attributed.
    journal.entries.retain(|entry| normalize(&entry.source) != normalize(&source.to_string_lossy()));
    journal.entries.push(JournalEntry {
        source: source.to_string_lossy().into_owned(),
        target: target.to_string_lossy().into_owned(),
        phase: "copying".to_string(),
        backup: String::new(),
    });
    save_journal(journal);

    let source_size = dir_size(source);
    if let Err(err) = run_robocopy(source, &target) {
        return Err(err);
    }
    let copied_size = dir_size(&target);
    if source_size != copied_size {
        journal
            .entries
            .iter_mut()
            .filter(|entry| normalize(&entry.source) == normalize(&source.to_string_lossy()))
            .for_each(|entry| entry.phase = "copied".to_string());
        save_journal(journal);
        return Err(format!("app_move.fail_verify|{source_size}|{copied_size}"));
    }

    let backup = source.with_file_name(format!("{name}{OLD_SUFFIX}-{}", timestamp()));
    // Record the backup before renaming: a crash here must leave a recoverable
    // trail pointing at the source that was moved aside.
    journal
        .entries
        .iter_mut()
        .filter(|entry| normalize(&entry.source) == normalize(&source.to_string_lossy()))
        .for_each(|entry| {
            entry.phase = "copied".to_string();
            entry.backup = backup.to_string_lossy().into_owned();
        });
    save_journal(journal);
    fs::rename(source, &backup).map_err(|err| format!("app_move.fail_rename|{err}"))?;

    if let Err(err) = create_junction(source, &target) {
        let _ = fs::rename(&backup, source);
        return Err(format!("app_move.fail_link|{err}"));
    }

    journal
        .entries
        .iter_mut()
        .filter(|entry| normalize(&entry.source) == normalize(&source.to_string_lossy()))
        .for_each(|entry| {
            entry.phase = "linked".to_string();
            entry.backup = backup.to_string_lossy().into_owned();
        });
    save_journal(journal);

    // Best effort: lingering locks leave a residue folder the panel can clean.
    if !discard_tree(&backup) {
        return Ok("app_move.done_residue".to_string());
    }
    journal.entries.retain(|entry| normalize(&entry.source) != normalize(&source.to_string_lossy()));
    save_journal(journal);
    Ok("app_move.state_moved".to_string())
}

#[tauri::command]
pub fn move_app_dirs(app: AppHandle, paths: Vec<String>, target_root: String) -> Result<(), String> {
    if paths.is_empty() {
        return Err("no folders selected".to_string());
    }
    let target = PathBuf::from(&target_root);
    if !target.exists() {
        return Err("target folder does not exist".to_string());
    }
    std::thread::spawn(move || {
        let total = paths.len() as u32;
        let mut journal = load_journal();
        let mut results = Vec::new();
        for (index, path) in paths.iter().enumerate() {
            let _ = app.emit(
                "appmove://progress",
                MoveProgress {
                    index: index as u32 + 1,
                    total,
                    path: path.clone(),
                    phase: "copying".to_string(),
                    percent: 0,
                },
            );
            let outcome = move_one(Path::new(path), &target, &mut journal);
            let (ok, message) = match outcome {
                Ok(message) => (true, message),
                Err(err) => (false, err),
            };
            results.push(MoveItemResult {
                path: path.clone(),
                ok,
                message,
            });
        }
        let moved = results.iter().filter(|item| item.ok).count() as u32;
        let failed = results.len() as u32 - moved;
        if moved > 0 {
            let _ = locker::write_dont_delete_marker(&target_root);
        }
        let _ = app.emit(
            "appmove://finished",
            MoveFinished {
                moved,
                failed,
                items: results,
            },
        );
    });
    Ok(())
}

fn move_path_one(source: &Path, target_root: &Path) -> Result<(), String> {
    if !source.is_absolute() || !source.exists() {
        return Err(format!("move.err.no_source|{}", source.to_string_lossy()));
    }
    let lower = source.to_string_lossy().to_lowercase();
    if source.components().count() < 2 {
        return Err("app_move.fail_invalid".to_string());
    }
    if lower == "c:\\windows" || lower.starts_with("c:\\windows\\") {
        return Err("app_move.fail_invalid".to_string());
    }
    if is_reparse(source) {
        return Err("app_move.fail_already_moved".to_string());
    }
    let name = source
        .file_name()
        .ok_or_else(|| "app_move.fail_invalid".to_string())?
        .to_string_lossy()
        .into_owned();
    let target = target_root.join(&name);
    if target.exists() {
        return Err(format!("move.err.dst_exists|{}", target.to_string_lossy()));
    }
    if normalize(&target.to_string_lossy())
        .starts_with(&format!("{}/", normalize(&source.to_string_lossy())))
    {
        return Err("app_move.target_invalid".to_string());
    }

    let metadata = fs::metadata(source).map_err(|err| format!("move.err.read_failed|{err}"))?;
    if metadata.is_dir() {
        let (source_size, source_files) = dir_stats(source);
        if let Err(err) = run_robocopy(source, &target) {
            let _ = fs::remove_dir_all(&target);
            return Err(err);
        }
        let (copied_size, copied_files) = dir_stats(&target);
        if source_size != copied_size {
            let _ = fs::remove_dir_all(&target);
            return Err(format!(
                "move.err.size_mismatch|{source_size}|{copied_size}"
            ));
        }
        if source_files != copied_files {
            let _ = fs::remove_dir_all(&target);
            return Err(format!(
                "move.err.count_mismatch|{source_files}|{copied_files}"
            ));
        }
        locker::write_dont_delete_marker(&target.to_string_lossy());
        if !discard_tree(source) {
            return Err("move.err.remove_failed".to_string());
        }
    } else {
        fs::copy(source, &target).map_err(|err| format!("move.err.copy_failed|{err}"))?;
        let copied = fs::metadata(&target)
            .map_err(|err| format!("move.err.missing_file|{err}"))?
            .len();
        if copied != metadata.len() {
            let _ = fs::remove_file(&target);
            return Err(format!("move.err.size_mismatch|{}|{copied}", metadata.len()));
        }
        locker::write_dont_delete_marker(&target.to_string_lossy());
        fs::remove_file(source).map_err(|err| format!("move.err.remove_failed|{err}"))?;
    }
    Ok(())
}

fn run_move_path(app: AppHandle, paths: Vec<String>, target_root: PathBuf) -> MoveFinished {
    if fs::create_dir_all(&target_root).is_err() {
        let items = paths
            .iter()
            .map(|path| MoveItemResult {
                path: path.clone(),
                ok: false,
                message: format!("move.err.mkdir|{}", target_root.to_string_lossy()),
            })
            .collect::<Vec<_>>();
        return MoveFinished {
            moved: 0,
            failed: items.len() as u32,
            items,
        };
    }
    let total = paths.len() as u32;
    let mut results = Vec::with_capacity(paths.len());
    for (index, path) in paths.iter().enumerate() {
        let _ = app.emit(
            "appmove://progress",
            MoveProgress {
                index: index as u32 + 1,
                total,
                path: path.clone(),
                phase: "moving".to_string(),
                percent: 0,
            },
        );
        let (ok, message) = match move_path_one(Path::new(path), &target_root) {
            Ok(()) => (true, String::new()),
            Err(err) => (false, err),
        };
        results.push(MoveItemResult {
            path: path.clone(),
            ok,
            message,
        });
    }
    let moved = results.iter().filter(|item| item.ok).count() as u32;
    let failed = results.len() as u32 - moved;
    let _ = app.emit(
        "appmove://finished",
        MoveFinished {
            moved,
            failed,
            items: results.clone(),
        },
    );
    MoveFinished {
        moved,
        failed,
        items: results,
    }
}

#[tauri::command]
pub async fn move_path(
    app: AppHandle,
    paths: Vec<String>,
    target_root: String,
) -> Result<MoveFinished, String> {
    if paths.is_empty() {
        return Err("no paths selected".to_string());
    }
    let target = PathBuf::from(&target_root);
    if !target.is_absolute() {
        return Err("app_move.target_invalid".to_string());
    }
    tauri::async_runtime::spawn_blocking(move || run_move_path(app, paths, target))
        .await
        .map_err(|err| err.to_string())
}

/// Restores a junction back into a real folder without touching the junction
/// until a verified copy of the data exists next to it.
fn restore_core(source: &Path) -> Result<String, String> {
    let target = fs::canonicalize(source)
        .map(|path| strip_verbatim(&path.to_string_lossy()))
        .unwrap_or_default();
    if target.is_empty() || !Path::new(&target).exists() {
        return Err("app_move.fail_missing".to_string());
    }
    let target_path = PathBuf::from(&target);
    let real_source = PathBuf::from(strip_verbatim(&source.to_string_lossy()));
    let name = real_source
        .file_name()
        .map(|value| value.to_string_lossy().into_owned())
        .ok_or_else(|| "app_move.fail_invalid".to_string())?;

    // Copy next to the junction first. The junction is only removed once a
    // verified copy exists, so an interrupted restore never loses data.
    let staging = real_source.with_file_name(format!(
        "{name}{OLD_SUFFIX}-restore-{}",
        timestamp()
    ));
    let _ = fs::remove_dir_all(&staging);
    if let Err(err) = run_robocopy(&target_path, &staging) {
        let _ = fs::remove_dir_all(&staging);
        return Err(err);
    }
    let (expected, expected_files) = dir_stats(&target_path);
    let (actual, actual_files) = dir_stats(&staging);
    if expected != actual || expected_files != actual_files {
        let _ = fs::remove_dir_all(&staging);
        return Err(format!("app_move.fail_verify|{expected}|{actual}"));
    }

    if let Err(err) = remove_junction(&real_source) {
        let _ = fs::remove_dir_all(&staging);
        return Err(err);
    }
    if let Err(err) = fs::rename(&staging, &real_source) {
        // Put the junction back so the data stays reachable.
        let _ = create_junction(&real_source, &target_path);
        return Err(format!("app_move.fail_rename|{err}"));
    }
    crate::locker::remove_dont_delete_marker(&real_source.to_string_lossy());
    // Best effort: the restored copy is already in place.
    let _ = discard_tree(&target_path);
    Ok("app_move.state_normal".to_string())
}

#[tauri::command]
pub fn restore_app_dir(app: AppHandle, path: String) -> Result<(), String> {
    let source = PathBuf::from(&path);
    if !source.exists() || !is_reparse(&source) {
        return Err("app_move.fail_not_moved".to_string());
    }
    std::thread::spawn(move || {
        let _ = app.emit(
            "appmove://progress",
            MoveProgress {
                index: 1,
                total: 1,
                path: source.to_string_lossy().into_owned(),
                phase: "restoring".to_string(),
                percent: 0,
            },
        );
        let message = match restore_core(&source) {
            Ok(message) => {
                let mut journal = load_journal();
                journal.entries.retain(|entry| normalize(&entry.source) != normalize(&source.to_string_lossy()));
                save_journal(&journal);
                Ok(message)
            }
            Err(err) => Err(err),
        };
        let (ok, message) = match message {
            Ok(value) => (true, value),
            Err(err) => (false, err),
        };
        let _ = app.emit(
            "appmove://finished",
            MoveFinished {
                moved: if ok { 1 } else { 0 },
                failed: if ok { 0 } else { 1 },
                items: vec![MoveItemResult {
                    path: source.to_string_lossy().into_owned(),
                    ok,
                    message,
                }],
            },
        );
    });
    Ok(())
}

#[tauri::command]
pub fn clean_app_residue(app: AppHandle, paths: Vec<String>) -> Result<(), String> {
    if paths.is_empty() {
        return Err("no folders selected".to_string());
    }
    std::thread::spawn(move || {
        let total = paths.len() as u32;
        let mut results = Vec::new();
        for (index, path) in paths.iter().enumerate() {
            let _ = app.emit(
                "appmove://progress",
                MoveProgress {
                    index: index as u32 + 1,
                    total,
                    path: path.clone(),
                    phase: "deleting".to_string(),
                    percent: 0,
                },
            );
            let lower = path.to_lowercase();
            let valid = lower.contains(OLD_SUFFIX) && Path::new(path).exists();
            let outcome = if valid {
                fs::remove_dir_all(path).map_err(|err| err.to_string())
            } else {
                Err("app_move.fail_invalid".to_string())
            };
            results.push(MoveItemResult {
                path: path.clone(),
                ok: outcome.is_ok(),
                message: outcome.err().unwrap_or_else(|| "app_move.residue_done".to_string()),
            });
        }
        let moved = results.iter().filter(|item| item.ok).count() as u32;
        let failed = results.len() as u32 - moved;
        let _ = app.emit(
            "appmove://finished",
            MoveFinished {
                moved,
                failed,
                items: results,
            },
        );
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    struct TempDir(PathBuf);

    impl TempDir {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "ncduwin-move-test-{}-{}",
                std::process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }

        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn verbatim_prefixes_are_removed() {
        assert_eq!(strip_verbatim(r"\\?\C:\Temp\a"), r"C:\Temp\a");
        assert_eq!(strip_verbatim(r"\??\D:\data"), r"D:\data");
        assert_eq!(strip_verbatim(r"\\?\UNC\server\share"), r"\\server\share");
        assert_eq!(strip_verbatim(r"E:\plain"), r"E:\plain");
    }

    #[test]
    fn restore_roundtrip_keeps_content_and_removes_junction() {
        let temp = TempDir::new();
        let real = temp.path().join("real");
        fs::create_dir_all(real.join("nested")).unwrap();
        fs::write(real.join("a.txt"), b"hello").unwrap();
        fs::write(real.join("nested").join("b.txt"), b"world!!").unwrap();

        let junction = temp.path().join("app");
        create_junction(&junction, &real).unwrap();
        assert!(is_reparse(&junction));
        assert_eq!(link_target(&junction), real.to_string_lossy());

        restore_core(&junction).unwrap();
        assert!(!is_reparse(&junction));
        assert_eq!(fs::read(junction.join("a.txt")).unwrap(), b"hello");
        assert_eq!(fs::read(junction.join("nested").join("b.txt")).unwrap(), b"world!!");
        assert!(!real.exists());
    }

    #[test]
    fn restore_keeps_junction_when_target_is_missing() {
        let temp = TempDir::new();
        let real = temp.path().join("real");
        fs::create_dir_all(&real).unwrap();
        fs::write(real.join("a.txt"), b"x").unwrap();
        let junction = temp.path().join("app");
        create_junction(&junction, &real).unwrap();
        fs::remove_dir_all(&real).unwrap();

        assert!(restore_core(&junction).is_err());
        assert!(is_reparse(&junction));
    }

    #[test]
    fn robocopy_copies_and_dir_stats_match() {
        let temp = TempDir::new();
        let source = temp.path().join("src");
        fs::create_dir_all(source.join("inner")).unwrap();
        fs::write(source.join("one.txt"), vec![7u8; 1024]).unwrap();
        fs::write(source.join("inner").join("two.txt"), vec![9u8; 2048]).unwrap();
        let destination = temp.path().join("dst");
        run_robocopy(&source, &destination).unwrap();

        let (source_size, source_files) = dir_stats(&source);
        let (copied_size, copied_files) = dir_stats(&destination);
        assert_eq!(source_size, copied_size);
        assert_eq!(source_files, copied_files);
        assert_eq!(source_files, 2);
        assert_eq!(fs::read(destination.join("inner").join("two.txt")).unwrap().len(), 2048);
    }
}
