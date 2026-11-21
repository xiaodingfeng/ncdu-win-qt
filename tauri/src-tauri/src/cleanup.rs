use std::collections::HashMap;
use std::fs;
use std::io::Read;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};

use crate::ops;
use crate::scan::{NodeEntry, NodeKind, ScanStore};

const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const MIN_LARGE_FILE_SIZE: u64 = 50 * 1024 * 1024;
const LARGE_ARCHIVE_MIN_SIZE: u64 = 100 * 1024 * 1024;
const MAX_LARGE_FILES: usize = 200;
const MIN_DUP_SIZE: u64 = 1024 * 1024;
const PARTIAL_HASH_BYTES: usize = 4 * 1024;
const FULL_HASH_BUFFER: usize = 4 * 1024 * 1024;

const TMP_SUFFIXES: &[&str] = &[".tmp", ".log", ".bak", ".old", ".cache"];
const ARCHIVE_SUFFIXES: &[&str] = &[".zip", ".rar", ".7z", ".tar", ".iso", ".msi", ".dmg", ".tgz", ".xz"];
const PYC_SUFFIXES: &[&str] = &[".pyc", ".pyo"];
const BUILD_CACHE_NAMES: &[&str] = &[
    "obj", "build", "dist", ".cache", ".mypy_cache", ".ruff_cache", ".pytest_cache", ".tox", ".eggs",
];
const EXCLUDED_EXTENSIONS: &[&str] = &[
    ".dll", ".sys", ".drv", ".ocx", ".ax", ".cpl", ".scr", ".msi", ".msp", ".mst", ".lib", ".a",
    ".obj", ".cat", ".inf", ".cab", ".ttf", ".otf", ".fon", ".fnt",
];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DangerLevel {
    S,
    A,
    B,
    C,
    D,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanupTarget {
    pub key: String,
    pub path: String,
    pub size: u64,
    pub file_count: u32,
    pub danger: DangerLevel,
    pub is_dir: bool,
    pub remark: String,
    pub checked: bool,
    pub enabled: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LargeFile {
    pub path: String,
    pub name: String,
    pub size: u64,
    pub danger: DangerLevel,
    pub remark: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DuplicateFile {
    pub path: String,
    pub name: String,
    pub size: u64,
    pub danger: DangerLevel,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DuplicateGroup {
    pub size: u64,
    pub wasted: u64,
    pub files: Vec<DuplicateFile>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanupFinished {
    pub targets: Vec<CleanupTarget>,
    pub large_files: Vec<LargeFile>,
    pub total_size: u64,
    pub total_count: u32,
    pub large_total: u64,
    pub free: u64,
    pub total: u64,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DupProgress {
    pub phase: u32,
    pub processed: u32,
    pub total: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DupFinished {
    pub groups: Vec<DuplicateGroup>,
    pub total_wasted: u64,
    pub total_files: u32,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanItem {
    #[serde(rename = "type")]
    pub kind: String,
    pub key: String,
    pub path: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanItemOutcome {
    pub key: String,
    pub path: String,
    pub deleted: u32,
    pub skipped: u32,
    pub freed: u64,
    pub failed: bool,
    pub message: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanProgress {
    pub index: u32,
    pub total: u32,
    pub label: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanFinished {
    pub total_deleted: u32,
    pub total_skipped: u32,
    pub total_freed: u64,
    pub items: Vec<CleanItemOutcome>,
    pub failed: Vec<String>,
    /// Set when failures look permission-related and elevation may help.
    pub needs_elevation: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CleanupFailure {
    pub code: String,
    pub message: String,
}

#[derive(Default)]
struct CleanupData {
    targets: Vec<CleanupTarget>,
    large_files: Vec<LargeFile>,
    root: String,
}

#[derive(Default)]
pub struct CleanupStore {
    data: Mutex<CleanupData>,
    cancel: Mutex<Option<Arc<AtomicBool>>>,
}

impl CleanupStore {
    fn begin(&self) -> Result<(Arc<AtomicBool>, String), String> {
        let mut current = self.cancel.lock().map_err(|_| "cleanup state poisoned")?;
        if let Some(old) = current.take() {
            old.store(true, Ordering::Relaxed);
        }
        let flag = Arc::new(AtomicBool::new(false));
        *current = Some(flag.clone());
        drop(current);
        let root = self
            .data
            .lock()
            .map(|guard| guard.root.clone())
            .unwrap_or_default();
        Ok((flag, root))
    }
}

// ---------------------------------------------------------------------------
// Path and classification helpers
// ---------------------------------------------------------------------------

fn normalize(path: &Path) -> String {
    let mut text = path.to_string_lossy().replace('\\', "/").to_lowercase();
    // Keep drive roots as "c:" so prefix checks use "c:/...", not "c://...".
    while text.ends_with('/') && text.len() > 1 {
        text.pop();
    }
    text
}

fn path_under(path: &Path, root_norm: &str) -> bool {
    let pn = normalize(path);
    pn == root_norm || pn.starts_with(&format!("{root_norm}/"))
}

fn is_excluded_extension(name: &str) -> bool {
    let lower = name.to_lowercase();
    let Some(dot) = lower.rfind('.') else {
        return true;
    };
    EXCLUDED_EXTENSIONS.contains(&&lower[dot..])
}

fn env_path(var: &str) -> PathBuf {
    std::env::var_os(var).map(PathBuf::from).unwrap_or_default()
}

fn classify_file_danger(path: &str, name: &str) -> DangerLevel {
    let name_l = name.to_lowercase();
    let pn = normalize(Path::new(path));
    let win = normalize(&env_path("WINDIR"));
    let win = if win.is_empty() { "c:/windows".to_string() } else { win };

    for segment in ["documents", "desktop", "pictures", "videos", "music", "onedrive", "downloads"] {
        if pn.contains(&format!("/{segment}/")) {
            return DangerLevel::D;
        }
    }
    if pn == win || pn.starts_with(&format!("{win}/")) {
        return DangerLevel::C;
    }
    let prog = normalize(&env_path("ProgramFiles"));
    let prog_x86 = normalize(&env_path("ProgramFiles(x86)"));
    let program_data = normalize(&env_path("ProgramData"));
    for base in [prog, prog_x86, program_data] {
        if !base.is_empty() && (pn == base || pn.starts_with(&format!("{base}/"))) {
            return DangerLevel::C;
        }
    }
    if matches!(name_l.as_str(), "pagefile.sys" | "hiberfil.sys" | "swapfile.sys") {
        return DangerLevel::C;
    }
    if name_l.ends_with(".tmp")
        || name_l.ends_with(".cache")
        || name_l.ends_with(".log")
        || name_l.ends_with(".pyc")
    {
        return DangerLevel::A;
    }
    for ext in [".zip", ".rar", ".7z", ".tar", ".tgz", ".iso", ".dmg", ".vhd", ".vhdx", ".vmdk", ".ova"] {
        if name_l.ends_with(ext) {
            return DangerLevel::B;
        }
    }
    DangerLevel::B
}

fn is_windows_root(path: &str) -> bool {
    let win = env_path("WINDIR");
    !win.as_os_str().is_empty() && normalize(Path::new(path)) == normalize(&win)
}

fn compute_path_size(path: &Path, cancel: &AtomicBool) -> (u64, u32) {
    let Ok(meta) = fs::symlink_metadata(path) else {
        return (0, 0);
    };
    if !meta.is_dir() || meta.file_type().is_symlink() {
        return (meta.len(), 1);
    }
    let mut size = 0u64;
    let mut count = 0u32;
    let mut pending = vec![path.to_path_buf()];
    while let Some(current) = pending.pop() {
        if cancel.load(Ordering::Relaxed) {
            break;
        }
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
                size += meta.len();
                count += 1;
            }
        }
    }
    (size, count)
}

// ---------------------------------------------------------------------------
// Target discovery
// ---------------------------------------------------------------------------

struct Discover<'a> {
    root: PathBuf,
    root_norm: String,
    targets: &'a mut Vec<CleanupTarget>,
    cancel: &'a AtomicBool,
}

impl<'a> Discover<'a> {
    fn add(
        &mut self,
        key: &str,
        path: PathBuf,
        danger: DangerLevel,
        enabled: bool,
        checked: bool,
        remark: &str,
        under_root: bool,
    ) {
        if path.as_os_str().is_empty() || !path.exists() {
            return;
        }
        if under_root && !path_under(&path, &self.root_norm) {
            return;
        }
        let is_dir = fs::symlink_metadata(&path).map(|m| m.is_dir()).unwrap_or(false);
        self.targets.push(CleanupTarget {
            key: key.to_string(),
            path: path.to_string_lossy().into_owned(),
            size: 0,
            file_count: 0,
            danger,
            is_dir,
            remark: remark.to_string(),
            checked,
            enabled,
        });
    }

    fn discover(&mut self) {
        let user = env_path("USERPROFILE");
        let local = env_path("LOCALAPPDATA");
        let roaming = env_path("APPDATA");
        let win = env_path("WINDIR");

        let user_dir = if user.as_os_str().is_empty() { env_path("USERPROFILE") } else { user };
        let local_dir = if local.as_os_str().is_empty() {
            user_dir.join("AppData/Local")
        } else {
            local
        };
        let roaming_dir = if roaming.as_os_str().is_empty() {
            user_dir.join("AppData/Roaming")
        } else {
            roaming
        };
        let win_dir = if win.as_os_str().is_empty() { PathBuf::from("C:/Windows") } else { win };

        // S-level: temp/cache produced by the system.
        self.add("cleanup.s_temp", self.root.join("temp"), DangerLevel::S, true, true, "cleanup.remark_s_temp", true);
        self.add("cleanup.s_cache", self.root.join("cache"), DangerLevel::S, true, true, "cleanup.remark_s_cache", true);
        let sys_temp = env_path("TEMP");
        let sys_temp = if sys_temp.as_os_str().is_empty() { env_path("TMP") } else { sys_temp };
        if !sys_temp.as_os_str().is_empty() {
            self.add("cleanup.s_user_temp", sys_temp, DangerLevel::S, true, true, "cleanup.remark_s_temp", true);
        }
        self.add(
            "cleanup.s_user_temp",
            user_dir.join("AppData/Local/Temp"),
            DangerLevel::S,
            true,
            true,
            "cleanup.remark_s_temp",
            true,
        );
        self.discover_pycache(&self.root.clone(), 4);
        self.discover_tmp_files();

        // A-level: user caches, all regeneratable.
        self.add("cleanup.a_temp", self.root.join("tmp"), DangerLevel::A, true, true, "cleanup.remark_a_temp", true);
        for name in BUILD_CACHE_NAMES {
            self.add(
                "cleanup.a_build_cache",
                self.root.join(name),
                DangerLevel::A,
                true,
                true,
                "cleanup.remark_a_build",
                true,
            );
        }
        self.discover_node_modules(&self.root.clone(), 3);
        self.discover_compiled_py();

        let browser = [
            local_dir.join("Google/Chrome/User Data/Default/Cache"),
            local_dir.join("Google/Chrome/User Data/Default/Code Cache"),
            local_dir.join("Microsoft/Edge/User Data/Default/Cache"),
            local_dir.join("Microsoft/Edge/User Data/Default/Code Cache"),
        ];
        for path in browser {
            self.add(
                "cleanup.a_browser_cache",
                path,
                DangerLevel::A,
                true,
                true,
                "cleanup.remark_a_browser_cache",
                true,
            );
        }
        let ff_profiles = user_dir.join("AppData/Roaming/Mozilla/Firefox/Profiles");
        if ff_profiles.exists() && path_under(&ff_profiles, &self.root_norm) {
            if let Ok(entries) = fs::read_dir(&ff_profiles) {
                for entry in entries.flatten() {
                    let cache = entry.path().join("cache2");
                    if cache.exists() {
                        let is_dir = fs::symlink_metadata(&cache).map(|m| m.is_dir()).unwrap_or(false);
                        self.targets.push(CleanupTarget {
                            key: "cleanup.a_browser_cache".to_string(),
                            path: cache.to_string_lossy().into_owned(),
                            size: 0,
                            file_count: 0,
                            danger: DangerLevel::A,
                            is_dir,
                            remark: "cleanup.remark_a_browser_cache".to_string(),
                            checked: true,
                            enabled: true,
                        });
                    }
                }
            }
        }

        let package_caches = [
            ("cleanup.a_pip_cache", local_dir.join("pip/cache")),
            ("cleanup.a_npm_cache", user_dir.join("AppData/Local/npm-cache")),
            ("cleanup.a_npm_cache", user_dir.join("AppData/Roaming/npm-cache")),
            ("cleanup.a_nuget_cache", user_dir.join(".nuget/packages")),
            ("cleanup.a_gradle_cache", user_dir.join(".gradle/caches")),
            ("cleanup.a_maven_cache", user_dir.join(".m2/repository")),
            ("cleanup.a_cargo_cache", user_dir.join(".cargo/registry/cache")),
            ("cleanup.a_yarn_cache", local_dir.join("Yarn/cache")),
            ("cleanup.a_go_cache", user_dir.join("go/pkg/mod/cache")),
            ("cleanup.a_composer_cache", local_dir.join("Composer/files")),
            ("cleanup.a_docker_cache", local_dir.join("Docker/wsl/data")),
            ("cleanup.a_conda_cache", user_dir.join("miniconda3/pkgs")),
            ("cleanup.a_conda_cache", user_dir.join("anaconda3/pkgs")),
            ("cleanup.a_conda_cache", local_dir.join("conda/conda/pkgs")),
        ];
        for (key, path) in package_caches {
            self.add(key, path, DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache", true);
        }

        // Windows' own cache locations.
        self.add("cleanup.c_win_temp", win_dir.join("Temp"), DangerLevel::S, true, true, "cleanup.remark_s_temp", true);
        self.add("cleanup.a_prefetch", win_dir.join("Prefetch"), DangerLevel::A, true, true, "cleanup.remark_a_prefetch", true);
        self.add(
            "cleanup.b_error_reports",
            win_dir.join("LiveKernelReports"),
            DangerLevel::B,
            true,
            false,
            "cleanup.remark_b_error_reports",
            true,
        );
        self.add(
            "cleanup.b_error_reports",
            local_dir.join("Microsoft/Windows/WER"),
            DangerLevel::B,
            true,
            false,
            "cleanup.remark_b_error_reports",
            true,
        );
        self.add(
            "cleanup.a_font_cache",
            local_dir.join("Microsoft/Windows/Fonts"),
            DangerLevel::A,
            true,
            true,
            "cleanup.remark_a_font_cache",
            true,
        );
        self.add(
            "cleanup.a_thumb_cache",
            local_dir.join("Microsoft/Windows/Explorer"),
            DangerLevel::A,
            true,
            true,
            "cleanup.remark_a_thumb_cache",
            true,
        );
        self.add("cleanup.b_win_logs", win_dir.join("Logs"), DangerLevel::B, true, false, "cleanup.remark_b_win_logs", true);
        self.add(
            "cleanup.b_recent_files",
            roaming_dir.join("Microsoft/Windows/Recent"),
            DangerLevel::B,
            true,
            false,
            "cleanup.remark_b_recent",
            true,
        );
        self.add(
            "cleanup.a_ie_cache",
            local_dir.join("Microsoft/Windows/INetCache"),
            DangerLevel::A,
            true,
            true,
            "cleanup.remark_a_ie_cache",
            true,
        );
        self.add(
            "cleanup.a_ie_cache",
            local_dir.join("Microsoft/Windows/Temporary Internet Files"),
            DangerLevel::A,
            true,
            true,
            "cleanup.remark_a_ie_cache",
            true,
        );
        self.add(
            "cleanup.b_event_logs",
            win_dir.join("System32/winevt/Logs"),
            DangerLevel::B,
            true,
            false,
            "cleanup.remark_b_event_logs",
            true,
        );
        self.add(
            "cleanup.a_delivery_opt",
            win_dir.join("DeliveryOptimization"),
            DangerLevel::A,
            true,
            true,
            "cleanup.remark_a_delivery_opt",
            true,
        );
        for path in [
            local_dir.join("D3DSCache"),
            user_dir.join("AppData/LocalLow/NVIDIA/PerDriverVersion/DXCache"),
            local_dir.join("AMD/DxCache"),
        ] {
            self.add(
                "cleanup.a_shader_cache",
                path,
                DangerLevel::A,
                true,
                true,
                "cleanup.remark_a_shader_cache",
                true,
            );
        }

        // B-level: large but potentially useful.
        self.add("cleanup.b_logs", self.root.join("logs"), DangerLevel::B, true, false, "cleanup.remark_b_logs", true);
        self.add("cleanup.b_old_backups", self.root.join("backup"), DangerLevel::B, true, false, "cleanup.remark_b_backup", true);
        self.add("cleanup.b_old_backups", self.root.join("old"), DangerLevel::B, true, false, "cleanup.remark_b_backup", true);

        // Downloads is a virtual group: files go, the folder stays.
        let downloads = user_dir.join("Downloads");
        if downloads.exists() && path_under(&downloads, &self.root_norm) {
            let (size, count) = compute_path_size(&downloads, self.cancel);
            if count > 0 {
                self.targets.push(CleanupTarget {
                    key: "cleanup.b_downloads".to_string(),
                    path: downloads.to_string_lossy().into_owned(),
                    size,
                    file_count: count,
                    danger: DangerLevel::B,
                    is_dir: false,
                    remark: "cleanup.remark_b_downloads".to_string(),
                    checked: false,
                    enabled: true,
                });
            }
        }
        self.discover_large_archives();

        // Windows Update caches are shown but disabled unless the Windows
        // directory itself is the scan root (C-level below).
        self.add(
            "cleanup.c_win_update",
            win_dir.join("SoftwareDistribution/Download"),
            DangerLevel::B,
            false,
            false,
            "cleanup.remark_c_win_update",
            true,
        );
        self.add(
            "cleanup.c_win_update",
            win_dir.join("SoftwareDistribution/Delivery Optimization"),
            DangerLevel::B,
            false,
            false,
            "cleanup.remark_c_win_update",
            true,
        );

        let win_norm = normalize(&win_dir);
        if path_under(&self.root, &win_norm) {
            for (key, path, remark) in [
                ("cleanup.c_win_update", win_dir.join("SoftwareDistribution/Download"), "cleanup.remark_c_win_update"),
                (
                    "cleanup.c_win_update",
                    win_dir.join("SoftwareDistribution/Delivery Optimization"),
                    "cleanup.remark_c_win_update",
                ),
                ("cleanup.c_win_temp", win_dir.join("Temp"), "cleanup.remark_c_win_temp"),
                ("cleanup.c_system32", win_dir.join("System32"), "cleanup.remark_c_system"),
                ("cleanup.c_syswow64", win_dir.join("SysWOW64"), "cleanup.remark_c_system"),
            ] {
                self.add(key, path, DangerLevel::C, false, false, remark, true);
            }
        }

        // Recycle bin lives at the drive root, not under the scanned folder.
        let drive_root = if self.root.to_string_lossy().len() >= 2 {
            PathBuf::from(format!("{}:/", &self.root.to_string_lossy()[..2]))
        } else {
            self.root.clone()
        };
        let bin = drive_root.join("$Recycle.Bin");
        if bin.exists() {
            let root_text = drive_root.to_string_lossy().into_owned();
            let (size, count) = ops::query_recycle_bin(&root_text);
            self.targets.push(CleanupTarget {
                key: "cleanup.b_recycle".to_string(),
                path: bin.to_string_lossy().into_owned(),
                size,
                file_count: count,
                danger: DangerLevel::B,
                is_dir: true,
                remark: "cleanup.remark_b_recycle".to_string(),
                checked: false,
                enabled: true,
            });
        }
    }

    fn discover_pycache(&mut self, root: &Path, depth: u32) {
        if depth == 0 || self.cancel.load(Ordering::Relaxed) {
            return;
        }
        let Ok(entries) = fs::read_dir(root) else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let path = entry.path();
            if name == "__pycache__" {
                let is_dir = fs::symlink_metadata(&path).map(|m| m.is_dir()).unwrap_or(false);
                self.targets.push(CleanupTarget {
                    key: "cleanup.s_pycache".to_string(),
                    path: path.to_string_lossy().into_owned(),
                    size: 0,
                    file_count: 0,
                    danger: DangerLevel::S,
                    is_dir,
                    remark: "cleanup.remark_s_pycache".to_string(),
                    checked: true,
                    enabled: true,
                });
            } else if !name.starts_with('.') {
                let lower = name.to_lowercase();
                if matches!(lower.as_str(), "node_modules" | ".git" | "venv" | ".venv") {
                    continue;
                }
                self.discover_pycache(&path, depth - 1);
            }
        }
    }

    fn discover_node_modules(&mut self, root: &Path, depth: u32) {
        if depth == 0 || self.cancel.load(Ordering::Relaxed) {
            return;
        }
        let Ok(entries) = fs::read_dir(root) else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let path = entry.path();
            if name == "node_modules" {
                let is_dir = fs::symlink_metadata(&path).map(|m| m.is_dir()).unwrap_or(false);
                self.targets.push(CleanupTarget {
                    key: "cleanup.a_node_modules".to_string(),
                    path: path.to_string_lossy().into_owned(),
                    size: 0,
                    file_count: 0,
                    danger: DangerLevel::A,
                    is_dir,
                    remark: "cleanup.remark_a_node_modules".to_string(),
                    checked: true,
                    enabled: true,
                });
            } else if !name.starts_with('.') {
                let lower = name.to_lowercase();
                if matches!(lower.as_str(), ".git" | "venv" | ".venv") {
                    continue;
                }
                self.discover_node_modules(&path, depth - 1);
            }
        }
    }

    fn discover_tmp_files(&mut self) {
        let Ok(entries) = fs::read_dir(&self.root) else {
            return;
        };
        let mut total = 0u64;
        let mut count = 0u32;
        for entry in entries.flatten() {
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            if meta.is_dir() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_lowercase();
            if TMP_SUFFIXES.iter().any(|suffix| name.ends_with(suffix)) {
                total += meta.len();
                count += 1;
            }
        }
        if count > 0 {
            self.targets.push(CleanupTarget {
                key: "cleanup.s_tmp_files".to_string(),
                path: self.root.to_string_lossy().into_owned(),
                size: total,
                file_count: count,
                danger: DangerLevel::S,
                is_dir: false,
                remark: "cleanup.remark_s_tmp_files".to_string(),
                checked: true,
                enabled: true,
            });
        }
    }

    fn discover_compiled_py(&mut self) {
        let Ok(entries) = fs::read_dir(&self.root) else {
            return;
        };
        let mut total = 0u64;
        let mut count = 0u32;
        for entry in entries.flatten() {
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            if meta.is_dir() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_lowercase();
            if PYC_SUFFIXES.iter().any(|suffix| name.ends_with(suffix)) {
                total += meta.len();
                count += 1;
            }
        }
        if count > 0 {
            self.targets.push(CleanupTarget {
                key: "cleanup.a_pyc".to_string(),
                path: self.root.to_string_lossy().into_owned(),
                size: total,
                file_count: count,
                danger: DangerLevel::A,
                is_dir: false,
                remark: "cleanup.remark_a_pyc".to_string(),
                checked: true,
                enabled: true,
            });
        }
    }

    fn discover_large_archives(&mut self) {
        let Ok(entries) = fs::read_dir(&self.root) else {
            return;
        };
        let mut total = 0u64;
        let mut count = 0u32;
        for entry in entries.flatten() {
            let Ok(meta) = entry.metadata() else {
                continue;
            };
            if meta.is_dir() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().to_lowercase();
            let archive = name.ends_with(".tar.gz") || ARCHIVE_SUFFIXES.iter().any(|suffix| name.ends_with(suffix));
            if archive && meta.len() > LARGE_ARCHIVE_MIN_SIZE {
                total += meta.len();
                count += 1;
            }
        }
        if count > 0 {
            self.targets.push(CleanupTarget {
                key: "cleanup.b_large_archives".to_string(),
                path: self.root.to_string_lossy().into_owned(),
                size: total,
                file_count: count,
                danger: DangerLevel::B,
                is_dir: false,
                remark: "cleanup.remark_b_large_archives".to_string(),
                checked: false,
                enabled: true,
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Scanning commands
// ---------------------------------------------------------------------------

fn apply_tree_sizes(state: &crate::scan::ScanState, targets: &mut [CleanupTarget]) {
    for target in targets.iter_mut() {
        if target.size > 0 && target.file_count > 0 {
            continue;
        }
        if let Some(entry) = state.lookup_entry(Path::new(&target.path)) {
            target.size = entry.size;
            target.file_count = if matches!(entry.kind, crate::scan::NodeKind::File) {
                1
            } else {
                entry.file_count
            };
        }
    }
}

fn build_large_files(entries: Vec<NodeEntry>) -> Vec<LargeFile> {
    let mut files: Vec<LargeFile> = entries
        .into_iter()
        .filter(|entry| entry.size >= MIN_LARGE_FILE_SIZE && !is_excluded_extension(&entry.name))
        .filter_map(|entry| {
            let danger = classify_file_danger(&entry.path, &entry.name);
            if danger == DangerLevel::C {
                return None;
            }
            Some(LargeFile {
                path: entry.path,
                name: entry.name,
                size: entry.size,
                danger,
                remark: String::new(),
            })
        })
        .collect();
    files.sort_by(|a, b| b.size.cmp(&a.size));
    files.truncate(MAX_LARGE_FILES);
    files
}

fn collect_large_files(state: &crate::scan::ScanState) -> Vec<LargeFile> {
    build_large_files(state.file_entries())
}

/// Walks the scan root directly when the main window has no scan result to
/// reuse (auto-scan is disabled, or the user opened the cleanup tab first).
fn collect_files_from_disk(root: &Path, cancel: &AtomicBool, min_size: u64) -> Vec<NodeEntry> {
    let mut files = Vec::new();
    let mut pending = vec![root.to_path_buf()];
    while let Some(current) = pending.pop() {
        if cancel.load(Ordering::Relaxed) {
            break;
        }
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
                continue;
            }
            if meta.len() < min_size {
                continue;
            }
            files.push(NodeEntry {
                path: entry.path().to_string_lossy().into_owned(),
                name: entry.file_name().to_string_lossy().into_owned(),
                kind: NodeKind::File,
                size: meta.len(),
                file_count: 1,
                dir_count: 0,
                is_hidden: false,
                skipped: false,
                error: None,
            });
        }
    }
    files
}

fn emit_cancelled(app: &AppHandle, scope: &str) {
    let _ = app.emit(
        "cleanup://failed",
        CleanupFailure {
            code: "cancelled".to_string(),
            message: scope.to_string(),
        },
    );
}

#[tauri::command]
pub fn scan_cleanup(
    app: AppHandle,
    store: State<'_, CleanupStore>,
    root: String,
    scan_categories: bool,
    scan_large_files: bool,
) -> Result<(), String> {
    let root_path = PathBuf::from(&root);
    if !root_path.exists() {
        return Err(format!("path not found: {root}"));
    }
    log::info!(
        "cleanup scan start: root={root} categories={scan_categories} large={scan_large_files}"
    );
    let (cancel, _) = store.begin()?;

    {
        let mut data = store.data.lock().map_err(|_| "cleanup state poisoned")?;
        data.targets.clear();
        data.large_files.clear();
        data.root = root.clone();
    }

    std::thread::spawn(move || {
        let store = app.state::<CleanupStore>();
        let root_path = root_path.clone();

        let mut targets = Vec::new();
        if scan_categories {
            let root_norm = normalize(&root_path);
            let mut discover = Discover {
                root: root_path.clone(),
                root_norm,
                targets: &mut targets,
                cancel: &cancel,
            };
            discover.discover();
            log::info!("cleanup discovery found {} candidates", targets.len());

            app.state::<ScanStore>()
                .with_state(|state| apply_tree_sizes(state, &mut targets));
            targets.par_iter_mut().for_each(|target| {
                if cancel.load(Ordering::Relaxed) {
                    return;
                }
                if target.size == 0 && target.file_count == 0 && Path::new(&target.path).exists() {
                    let (size, count) = compute_path_size(Path::new(&target.path), &cancel);
                    target.size = size;
                    target.file_count = count;
                }
            });
            if cancel.load(Ordering::Relaxed) {
                emit_cancelled(&app, "categories");
                return;
            }
            for target in &targets {
                let _ = app.emit("cleanup://target", target.clone());
            }
        }

        let mut large_files = Vec::new();
        if scan_large_files {
            let from_tree = app
                .state::<ScanStore>()
                .with_state(|state| collect_large_files(state));
            large_files = match from_tree {
                Some(files) => files,
                None => build_large_files(collect_files_from_disk(
                    &root_path,
                    &cancel,
                    MIN_LARGE_FILE_SIZE,
                )),
            };
        }
        if cancel.load(Ordering::Relaxed) {
            emit_cancelled(&app, "large");
            return;
        }

        let total_size: u64 = targets.iter().map(|t| t.size).sum();
        let total_count: u32 = targets.iter().map(|t| t.file_count).sum();
        let large_total: u64 = large_files.iter().map(|f| f.size).sum();
        let (free, total) = crate::scan::disk_space(Path::new(&root));
        log::info!(
            "cleanup scan finished: {} targets ({} bytes), {} large files",
            targets.len(),
            total_size,
            large_files.len()
        );

        if let Ok(mut data) = store.data.lock() {
            data.targets = targets.clone();
            data.large_files = large_files.clone();
        }
        if cancel.load(Ordering::Relaxed) {
            return;
        }
        let _ = app.emit(
            "cleanup://finished",
            CleanupFinished {
                targets,
                large_files,
                total_size,
                total_count,
                large_total,
                free,
                total,
            },
        );
    });

    Ok(())
}

fn hash_partial(path: &str) -> Vec<u8> {
    use md5::Digest;
    let Ok(mut file) = fs::File::open(path) else {
        return Vec::new();
    };
    let mut buffer = vec![0u8; PARTIAL_HASH_BYTES];
    let Ok(read) = file.read(&mut buffer) else {
        return Vec::new();
    };
    if read == 0 {
        return Vec::new();
    }
    let mut hasher = md5::Md5::new();
    hasher.update(&buffer[..read]);
    hasher.finalize().to_vec()
}

fn hash_full(path: &str, cancel: &AtomicBool) -> Vec<u8> {
    use sha2::Digest;
    let Ok(mut file) = fs::File::open(path) else {
        return Vec::new();
    };
    let mut hasher = sha2::Sha256::new();
    let mut buffer = vec![0u8; FULL_HASH_BUFFER];
    loop {
        if cancel.load(Ordering::Relaxed) {
            return Vec::new();
        }
        let Ok(read) = file.read(&mut buffer) else {
            return Vec::new();
        };
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    hasher.finalize().to_vec()
}

fn emit_dup_progress(app: &AppHandle, phase: u32, processed: u32, total: u32) {
    let _ = app.emit(
        "cleanup://dup-progress",
        DupProgress {
            phase,
            processed,
            total,
        },
    );
}

#[tauri::command]
pub fn scan_duplicates(app: AppHandle, store: State<'_, CleanupStore>) -> Result<(), String> {
    let (cancel, root) = store.begin()?;
    if root.is_empty() {
        return Err("no scan result".to_string());
    }

    std::thread::spawn(move || {
        let mut buckets: HashMap<u64, Vec<NodeEntry>> = HashMap::new();
        let from_tree = app.state::<ScanStore>().with_state(|state| {
            state.for_each_file(|entry| {
                if entry.size < MIN_DUP_SIZE || is_excluded_extension(&entry.name) {
                    return;
                }
                if classify_file_danger(&entry.path, &entry.name) == DangerLevel::C {
                    return;
                }
                buckets.entry(entry.size).or_default().push(entry.clone());
            });
        });
        if from_tree.is_none() {
            for entry in collect_files_from_disk(Path::new(&root), &cancel, MIN_DUP_SIZE) {
                if cancel.load(Ordering::Relaxed) {
                    emit_cancelled(&app, "duplicates");
                    return;
                }
                if entry.size < MIN_DUP_SIZE || is_excluded_extension(&entry.name) {
                    continue;
                }
                if classify_file_danger(&entry.path, &entry.name) == DangerLevel::C {
                    continue;
                }
                buckets.entry(entry.size).or_default().push(entry);
            }
        }

        emit_dup_progress(&app, 1, 0, 0);
        let size_groups: Vec<Vec<NodeEntry>> = buckets
            .into_values()
            .filter(|group| group.len() >= 2)
            .collect();
        let stage2_total: usize = size_groups.iter().map(|group| group.len()).sum();

        emit_dup_progress(&app, 2, 0, stage2_total as u32);
        let mut partial_groups: Vec<Vec<NodeEntry>> = Vec::new();
        let mut stage2_done = 0usize;
        for group in size_groups {
            if cancel.load(Ordering::Relaxed) {
                emit_cancelled(&app, "duplicates");
                return;
            }
            let group_len = group.len();
            let hashes: Vec<Vec<u8>> = group
                .par_iter()
                .map(|entry| hash_partial(&entry.path))
                .collect();
            let mut by_hash: HashMap<Vec<u8>, Vec<NodeEntry>> = HashMap::new();
            for (entry, hash) in group.into_iter().zip(hashes) {
                if !hash.is_empty() {
                    by_hash.entry(hash).or_default().push(entry);
                }
            }
            for (_, vec) in by_hash {
                if vec.len() >= 2 {
                    partial_groups.push(vec);
                }
            }
            stage2_done += group_len;
            let percent = (stage2_done as f32 / stage2_total.max(1) as f32 * 100.0) as u32;
            emit_dup_progress(&app, 2, percent, 100);
        }
        emit_dup_progress(&app, 2, 100, 100);

        let stage3_total: usize = partial_groups.iter().map(|group| group.len()).sum();
        emit_dup_progress(&app, 3, 0, stage3_total as u32);
        let mut groups = Vec::new();
        let mut total_wasted = 0u64;
        let mut total_files = 0u32;
        let mut stage3_done = 0usize;
        for group in partial_groups {
            if cancel.load(Ordering::Relaxed) {
                emit_cancelled(&app, "duplicates");
                return;
            }
            let group_len = group.len();
            let hashes: Vec<Vec<u8>> = group
                .par_iter()
                .map(|entry| hash_full(&entry.path, &cancel))
                .collect();
            let mut by_hash: HashMap<Vec<u8>, Vec<NodeEntry>> = HashMap::new();
            for (entry, hash) in group.into_iter().zip(hashes) {
                if !hash.is_empty() {
                    by_hash.entry(hash).or_default().push(entry);
                }
            }
            for (_, vec) in by_hash {
                if vec.len() < 2 {
                    continue;
                }
                let size = vec[0].size;
                let mut files: Vec<DuplicateFile> = vec
                    .into_iter()
                    .map(|entry| DuplicateFile {
                        danger: classify_file_danger(&entry.path, &entry.name),
                        path: entry.path,
                        name: entry.name,
                        size: entry.size,
                    })
                    .collect();
                files.sort_by_key(|file| file.path.len());
                let wasted = size * (files.len() as u64 - 1);
                total_wasted += wasted;
                total_files += files.len() as u32;
                let group = DuplicateGroup { size, wasted, files };
                // Only the final `dup-finished` payload is sent: emitting one
                // event per group floods the webview and copies every group.
                groups.push(group);
            }
            stage3_done += group_len;
            let percent = (stage3_done as f32 / stage3_total.max(1) as f32 * 100.0) as u32;
            emit_dup_progress(&app, 3, percent, 100);
        }
        groups.sort_by(|a, b| b.wasted.cmp(&a.wasted));
        emit_dup_progress(&app, 3, 100, 100);

        let _ = app.emit(
            "cleanup://dup-finished",
            DupFinished {
                groups,
                total_wasted,
                total_files,
            },
        );
    });

    Ok(())
}

#[tauri::command]
pub fn cancel_cleanup(store: State<'_, CleanupStore>) -> Result<(), String> {
    let current = store.cancel.lock().map_err(|_| "cleanup state poisoned")?;
    if let Some(flag) = current.as_ref() {
        flag.store(true, Ordering::Relaxed);
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Cleanup execution
// ---------------------------------------------------------------------------

fn app_dir() -> String {
    std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|parent| normalize(parent)))
        .unwrap_or_default()
}

fn is_application_path(path: &str) -> bool {
    let dir = app_dir();
    if dir.is_empty() {
        return false;
    }
    let pn = normalize(Path::new(path));
    pn == dir || pn.starts_with(&format!("{dir}/"))
}

fn remove_one(path: &str, permanent: bool) -> bool {
    if ops::is_protected(path) || is_application_path(path) {
        return false;
    }
    if permanent {
        ops::permanent_delete(path).is_ok()
    } else {
        ops::move_to_recycle_bin(&[path.to_string()]).is_ok()
    }
}

fn matching_files_in_root(root: &Path, suffixes: &[&str]) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(root) else {
        return Vec::new();
    };
    entries
        .flatten()
        .filter(|entry| {
            entry
                .metadata()
                .map(|meta| !meta.is_dir())
                .unwrap_or(false)
        })
        .filter(|entry| {
            let name = entry.file_name().to_string_lossy().to_lowercase();
            suffixes.iter().any(|suffix| name.ends_with(suffix))
        })
        .map(|entry| entry.path())
        .collect()
}

fn clean_virtual_group(target: &CleanupTarget, permanent: bool, out: &mut CleanItemOutcome) {
    let paths: Vec<PathBuf> = match target.key.as_str() {
        "cleanup.s_tmp_files" => matching_files_in_root(Path::new(&target.path), TMP_SUFFIXES),
        "cleanup.a_pyc" => matching_files_in_root(Path::new(&target.path), PYC_SUFFIXES),
        "cleanup.b_large_archives" => matching_files_in_root(Path::new(&target.path), ARCHIVE_SUFFIXES)
            .into_iter()
            .filter(|path| {
                fs::metadata(path)
                    .map(|meta| meta.len() > LARGE_ARCHIVE_MIN_SIZE)
                    .unwrap_or(false)
            })
            .collect(),
        "cleanup.b_downloads" => {
            let mut collected = Vec::new();
            let mut pending = vec![PathBuf::from(&target.path)];
            while let Some(current) = pending.pop() {
                let Ok(entries) = fs::read_dir(&current) else {
                    continue;
                };
                for entry in entries.flatten() {
                    let Ok(meta) = entry.metadata() else {
                        continue;
                    };
                    if meta.is_dir() {
                        pending.push(entry.path());
                    } else {
                        collected.push(entry.path());
                    }
                }
            }
            collected
        }
        _ => Vec::new(),
    };

    if target.key == "cleanup.b_downloads" {
        for path in &paths {
            let text = path.to_string_lossy().into_owned();
            if remove_one(&text, permanent) {
                out.deleted += 1;
            } else {
                out.skipped += 1;
            }
        }
        // Drop the subdirectories the deletion emptied, never Downloads itself.
        let mut dirs = Vec::new();
        let mut pending = vec![PathBuf::from(&target.path)];
        while let Some(current) = pending.pop() {
            let Ok(entries) = fs::read_dir(&current) else {
                continue;
            };
            for entry in entries.flatten() {
                if entry.metadata().map(|meta| meta.is_dir()).unwrap_or(false) {
                    dirs.push(entry.path());
                    pending.push(entry.path());
                }
            }
        }
        dirs.sort_by_key(|path| std::cmp::Reverse(path.as_os_str().len()));
        for dir in dirs {
            let _ = fs::remove_dir(&dir);
        }
    } else if !paths.is_empty() {
        let strings: Vec<String> = paths.iter().map(|p| p.to_string_lossy().into_owned()).collect();
        if permanent {
            for path in &strings {
                if remove_one(path, true) {
                    out.deleted += 1;
                } else {
                    out.skipped += 1;
                }
            }
        } else if ops::move_to_recycle_bin(&strings).is_ok() {
            out.deleted += strings.len() as u32;
        } else {
            out.skipped += strings.len() as u32;
        }
    }
    out.freed = if out.deleted > 0 { target.size } else { 0 };
}

fn clean_target(target: &CleanupTarget, permanent: bool) -> CleanItemOutcome {
    let mut out = CleanItemOutcome {
        key: target.key.clone(),
        path: target.path.clone(),
        deleted: 0,
        skipped: 0,
        freed: 0,
        failed: false,
        message: String::new(),
    };

    if !target.enabled || matches!(target.danger, DangerLevel::C | DangerLevel::D) {
        out.skipped = 1;
        out.failed = true;
        out.message = "protected target".to_string();
        return out;
    }
    if is_application_path(&target.path) || is_windows_root(&target.path) {
        out.skipped = 1;
        out.failed = true;
        out.message = "protected path".to_string();
        return out;
    }

    if target.key == "cleanup.b_recycle" {
        let drive = if target.path.len() >= 2 {
            format!("{}/", &target.path[..2])
        } else {
            target.path.clone()
        };
        if ops::empty_recycle_bin(&drive) {
            out.deleted = 1;
            out.freed = target.size;
        } else {
            out.skipped = 1;
            out.failed = true;
            out.message = "cannot empty recycle bin".to_string();
        }
        return out;
    }

    if matches!(
        target.key.as_str(),
        "cleanup.s_tmp_files" | "cleanup.a_pyc" | "cleanup.b_large_archives" | "cleanup.b_downloads"
    ) {
        clean_virtual_group(target, permanent, &mut out);
    } else if !Path::new(&target.path).exists() {
        out.skipped = 1;
        out.failed = true;
        out.message = "path no longer exists".to_string();
    } else if target.is_dir {
        let Ok(entries) = fs::read_dir(&target.path) else {
            out.skipped = 1;
            out.failed = true;
            out.message = "cannot read folder".to_string();
            return out;
        };
        let children: Vec<String> = entries
            .flatten()
            .map(|entry| entry.path().to_string_lossy().into_owned())
            .collect();
        if children.is_empty() {
            out.skipped = 1;
            return out;
        }
        if permanent {
            for child in &children {
                if remove_one(child, true) {
                    out.deleted += 1;
                } else {
                    out.skipped += 1;
                }
            }
        } else if ops::move_to_recycle_bin(&children).is_ok() {
            out.deleted = children.len() as u32;
        } else {
            out.skipped = children.len() as u32;
        }
        out.freed = if out.deleted > 0 { target.size } else { 0 };
    } else {
        if remove_one(&target.path, permanent) {
            out.deleted = 1;
            out.freed = target.size;
        } else {
            out.skipped = 1;
            out.failed = true;
            out.message = "cannot delete file".to_string();
        }
    }

    out.failed = out.deleted == 0 && target.file_count > 0;
    out
}

/// Deletes a file that came from the duplicate scan (or any stale large-file
/// path) without requiring it to exist in the current large-file store.
fn clean_direct_file(path: &str, permanent: bool) -> CleanItemOutcome {
    let mut out = CleanItemOutcome {
        key: "large".to_string(),
        path: path.to_string(),
        deleted: 0,
        skipped: 0,
        freed: 0,
        failed: false,
        message: String::new(),
    };
    if is_application_path(path) || is_windows_root(path) {
        out.skipped = 1;
        out.failed = true;
        out.message = "protected path".to_string();
        return out;
    }
    let Ok(metadata) = fs::metadata(path) else {
        out.skipped = 1;
        out.failed = true;
        out.message = "path no longer exists".to_string();
        return out;
    };
    let size = if metadata.is_dir() { 0 } else { metadata.len() };
    if remove_one(path, permanent) {
        out.deleted = 1;
        out.freed = size;
    } else {
        out.skipped = 1;
        out.failed = true;
        out.message = "cannot delete file".to_string();
    }
    out
}

fn clean_large_file(file: &LargeFile, permanent: bool) -> CleanItemOutcome {
    let mut out = CleanItemOutcome {
        key: "large".to_string(),
        path: file.path.clone(),
        deleted: 0,
        skipped: 0,
        freed: 0,
        failed: false,
        message: String::new(),
    };
    if is_application_path(&file.path) || is_windows_root(&file.path) {
        out.skipped = 1;
        out.failed = true;
        out.message = "protected path".to_string();
        return out;
    }
    if !Path::new(&file.path).exists() {
        out.skipped = 1;
        out.failed = true;
        out.message = "path no longer exists".to_string();
        return out;
    }
    if remove_one(&file.path, permanent) {
        out.deleted = 1;
        out.freed = file.size;
    } else {
        out.skipped = 1;
        out.failed = true;
        out.message = "cannot delete file".to_string();
    }
    out
}

#[tauri::command]
pub fn clean_items(app: AppHandle, items: Vec<CleanItem>, permanent: bool) -> Result<(), String> {
    if items.is_empty() {
        return Err("no items selected".to_string());
    }
    let store = app.state::<CleanupStore>();
    let (targets, large_files) = {
        let data = store.data.lock().map_err(|_| "cleanup state poisoned")?;
        (data.targets.clone(), data.large_files.clone())
    };

    std::thread::spawn(move || {
        let total = items.len() as u32;
        let mut outcomes = Vec::new();
        let mut failed = Vec::new();
        let mut total_deleted = 0u32;
        let mut total_skipped = 0u32;
        let mut total_freed = 0u64;

        for (index, item) in items.iter().enumerate() {
            let label = if item.kind == "target" {
                item.key.clone()
            } else {
                Path::new(&item.path)
                    .file_name()
                    .map(|name| name.to_string_lossy().into_owned())
                    .unwrap_or_else(|| item.path.clone())
            };
            let _ = app.emit(
                "cleanup://clean-progress",
                CleanProgress {
                    index: index as u32 + 1,
                    total,
                    label,
                },
            );

            let outcome = if item.kind == "target" {
                targets
                    .iter()
                    .find(|target| target.key == item.key && target.path == item.path)
                    .map(|target| clean_target(target, permanent))
                    .unwrap_or_else(|| CleanItemOutcome {
                        key: item.key.clone(),
                        path: item.path.clone(),
                        deleted: 0,
                        skipped: 1,
                        freed: 0,
                        failed: true,
                        message: "target not found in last scan".to_string(),
                    })
            } else {
                large_files
                    .iter()
                    .find(|file| file.path == item.path)
                    .map(|file| clean_large_file(file, permanent))
                    .unwrap_or_else(|| clean_direct_file(&item.path, permanent))
            };

            total_deleted += outcome.deleted;
            total_skipped += outcome.skipped;
            total_freed += outcome.freed;
            if outcome.failed {
                failed.push(if outcome.message.is_empty() {
                    outcome.path.clone()
                } else {
                    format!("{}: {}", outcome.path, outcome.message)
                });
            }
            outcomes.push(outcome);
        }

        // Drop deleted paths from the scanned tree so the list and totals stay
        // in sync immediately after a cleanup.
        let deleted_paths: Vec<String> = outcomes
            .iter()
            .filter(|outcome| {
                !outcome.failed
                    && !matches!(
                        outcome.key.as_str(),
                        "cleanup.s_tmp_files" | "cleanup.a_pyc" | "cleanup.b_large_archives" | "cleanup.b_downloads" | "cleanup.b_recycle"
                    )
            })
            .map(|outcome| outcome.path.clone())
            .collect();
        if !deleted_paths.is_empty() {
            let scan = app.state::<ScanStore>();
            crate::scan::remove_paths(&scan, &deleted_paths);
        }

        let needs_elevation = outcomes.iter().any(|outcome| {
            outcome.failed
                && outcome.key != "cleanup.b_recycle"
                && !outcome.message.contains("no longer exists")
                && !outcome.message.contains("not found")
                && !outcome.message.contains("protected")
                && outcome.path.to_lowercase().len() > 2
        });

        log::info!(
            "cleanup finished: {} deleted, {} skipped, {} bytes freed",
            total_deleted,
            total_skipped,
            total_freed
        );
        let _ = app.emit(
            "cleanup://clean-finished",
            CleanFinished {
                total_deleted,
                total_skipped,
                total_freed,
                items: outcomes,
                failed,
                needs_elevation,
            },
        );
    });

    Ok(())
}

/// Retries failed deletes through the elevated helper (one UAC prompt).
#[tauri::command]
pub fn clean_items_elevated(
    app: AppHandle,
    items: Vec<CleanItem>,
    permanent: bool,
) -> Result<(), String> {
    if items.is_empty() {
        return Err("no items selected".to_string());
    }
    std::thread::spawn(move || {
        let mut paths = Vec::new();
        let mut sizes = Vec::new();
        for item in items.iter().filter(|item| item.kind != "target") {
            let size = fs::metadata(&item.path)
                .map(|meta| if meta.is_dir() { 0 } else { meta.len() })
                .unwrap_or(0);
            sizes.push((item.path.clone(), size));
            paths.push(item.path.clone());
        }
        let outcome = crate::elevate::delete_paths_elevated(&paths, permanent);
        let ok = matches!(&outcome, Ok(value) if value.ok);
        let message = match outcome {
            Ok(value) => value.message,
            Err(err) => err,
        };

        let mut deleted_paths = Vec::new();
        let mut total_freed = 0u64;
        let outcomes: Vec<CleanItemOutcome> = sizes
            .iter()
            .map(|(path, size)| CleanItemOutcome {
                key: "large".to_string(),
                path: path.clone(),
                deleted: if ok { 1 } else { 0 },
                skipped: if ok { 0 } else { 1 },
                freed: if ok { *size } else { 0 },
                failed: !ok,
                message: if ok || message.is_empty() {
                    String::new()
                } else {
                    message.clone()
                },
            })
            .collect();
        if ok {
            total_freed = sizes.iter().map(|(_, size)| *size).sum();
            deleted_paths = paths.clone();
        }
        if !deleted_paths.is_empty() {
            let scan = app.state::<ScanStore>();
            crate::scan::remove_paths(&scan, &deleted_paths);
        }
        let _ = app.emit(
            "cleanup://clean-finished",
            CleanFinished {
                total_deleted: if ok { paths.len() as u32 } else { 0 },
                total_skipped: if ok { 0 } else { paths.len() as u32 },
                total_freed,
                items: outcomes,
                failed: if ok { Vec::new() } else { vec![message] },
                needs_elevation: false,
            },
        );
    });
    Ok(())
}

#[tauri::command]
pub fn cleanup_disk_space(root: String) -> Result<(u64, u64), String> {
    Ok(crate::scan::disk_space(&PathBuf::from(root)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static COUNTER: AtomicU32 = AtomicU32::new(0);

    fn temp_path(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "ncduwin-cleanup-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir.join(name)
    }

    #[test]
    fn direct_cleanup_deletes_a_file_and_reports_freed_bytes() {
        let path = temp_path("duplicate.bin");
        fs::write(&path, vec![3u8; 4096]).unwrap();
        let outcome = clean_direct_file(&path.to_string_lossy(), true);
        assert!(outcome.failed == false, "unexpected failure: {}", outcome.message);
        assert_eq!(outcome.deleted, 1);
        assert_eq!(outcome.freed, 4096);
        assert!(!path.exists());
        let _ = fs::remove_dir_all(path.parent().unwrap());
    }

    #[test]
    fn direct_cleanup_reports_a_missing_file_instead_of_failing_hard() {
        let path = temp_path("gone.bin");
        let outcome = clean_direct_file(&path.to_string_lossy(), true);
        assert!(outcome.failed);
        assert_eq!(outcome.deleted, 0);
        assert!(outcome.message.contains("no longer exists"));
        let _ = fs::remove_dir_all(path.parent().unwrap());
    }
}
