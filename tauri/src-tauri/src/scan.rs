use std::collections::HashMap;
use std::ffi::OsStr;
use std::fs;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};
use windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};

use crate::mft;

const MAX_SCAN_DEPTH: u32 = 64;
const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x10;
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const LOW_MEMORY_BYTES: u64 = 256 * 1024 * 1024;

const SKIP_ALWAYS: &[&str] = &["winsxs"];
const SKIP_HEAVY: &[&str] = &["node_modules"];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NodeKind {
    File,
    Directory,
    Drive,
    Symlink,
    Other,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NodeEntry {
    pub path: String,
    pub name: String,
    pub kind: NodeKind,
    pub size: u64,
    pub file_count: u32,
    pub dir_count: u32,
    pub is_hidden: bool,
    pub skipped: bool,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanProgress {
    pub path: String,
    pub files: u64,
    pub dirs: u64,
    pub bytes: u64,
    pub engine: String,
    pub phase: String,
    pub count: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanFinished {
    pub root: NodeEntry,
    pub elapsed_ms: u64,
    pub engine: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanFailure {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DirectoryView {
    pub node: NodeEntry,
    pub children: Vec<NodeEntry>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TreemapNode {
    pub path: String,
    pub name: String,
    pub kind: NodeKind,
    pub size: u64,
    pub children: Vec<TreemapNode>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiskInfo {
    pub free: u64,
    pub total: u64,
}

pub(crate) struct RawNode {
    pub(crate) path: PathBuf,
    pub(crate) name: String,
    pub(crate) kind: NodeKind,
    pub(crate) size: u64,
    pub(crate) file_count: u32,
    pub(crate) dir_count: u32,
    pub(crate) is_hidden: bool,
    pub(crate) skipped: bool,
    pub(crate) error: Option<String>,
    pub(crate) children: Vec<RawNode>,
}

struct NodeData {
    entry: NodeEntry,
    children: Vec<usize>,
    parent: usize,
}

#[derive(Default)]
pub struct ScanState {
    arena: Vec<NodeData>,
    by_path: HashMap<String, usize>,
    root: usize,
}

#[derive(Default)]
pub struct ScanStore {
    inner: Mutex<Option<ScanState>>,
    cancel: Mutex<Option<Arc<AtomicBool>>>,
    generation: AtomicU64,
}

impl ScanStore {
    /// Runs `f` against the current scan result, if one exists.
    pub(crate) fn with_state<R>(&self, f: impl FnOnce(&ScanState) -> R) -> Option<R> {
        let guard = self.inner.lock().ok()?;
        guard.as_ref().map(f)
    }
}

/// Free/total bytes of the volume that holds `path`.
pub(crate) fn disk_space(path: &Path) -> (u64, u64) {
    let wide: Vec<u16> = OsStr::new(path)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let mut free = 0u64;
    let mut total = 0u64;
    let ok = unsafe {
        GetDiskFreeSpaceExW(
            wide.as_ptr(),
            &mut free,
            &mut total,
            std::ptr::null_mut(),
        )
    };
    if ok == 0 {
        (0, 0)
    } else {
        (free, total)
    }
}

struct ScanContext {
    cancel: Arc<AtomicBool>,
    app: AppHandle,
    files: AtomicU64,
    dirs: AtomicU64,
    bytes: AtomicU64,
    last_emit: AtomicU64,
    low_memory: AtomicBool,
    skip_heavy: bool,
}

impl ScanContext {
    fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }

    fn is_low_memory(&self) -> bool {
        if self.low_memory.load(Ordering::Relaxed) {
            return true;
        }
        let mut status: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
        status.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
        let ok = unsafe { GlobalMemoryStatusEx(&mut status) };
        if ok == 0 {
            return false;
        }
        if status.ullAvailPhys < LOW_MEMORY_BYTES {
            self.low_memory.store(true, Ordering::Relaxed);
            return true;
        }
        false
    }

    fn emit_progress(&self, path: &Path) {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as u64;
        let last = self.last_emit.load(Ordering::Relaxed);
        if now.saturating_sub(last) < 100 {
            return;
        }
        if self
            .last_emit
            .compare_exchange(last, now, Ordering::Relaxed, Ordering::Relaxed)
            .is_err()
        {
            return;
        }
        let _ = self.app.emit(
            "scan://progress",
            ScanProgress {
                path: path.to_string_lossy().into_owned(),
                files: self.files.load(Ordering::Relaxed),
                dirs: self.dirs.load(Ordering::Relaxed),
                bytes: self.bytes.load(Ordering::Relaxed),
                engine: "walk".to_string(),
                phase: "walking".to_string(),
                count: None,
            },
        );
    }
}

fn path_key(path: &Path) -> String {
    let text = path.to_string_lossy().to_lowercase();
    if text.len() > 3 {
        text.trim_end_matches(['\\', '/']).to_string()
    } else {
        text
    }
}

fn normalize(path: &Path) -> PathBuf {
    let canonical = fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    let text = canonical.to_string_lossy();
    // canonicalize() returns verbatim paths (\\?\C:\...); keep UI paths clean.
    if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
        PathBuf::from(format!(r"\\{rest}"))
    } else if let Some(rest) = text.strip_prefix(r"\\?\") {
        PathBuf::from(rest)
    } else {
        canonical
    }
}
fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.to_string_lossy().into_owned())
}

fn display_name(path: &Path, top: bool) -> String {
    if top {
        if let Some(text) = path.to_str() {
            let trimmed = text.trim_end_matches(['\\', '/']);
            if trimmed.len() == 2 && trimmed.as_bytes()[1] == b':' {
                return trimmed.to_string();
            }
        }
    }
    file_name(path)
}

fn is_hidden_name(name: &str) -> bool {
    name.starts_with('.') || name.starts_with('$')
}

fn skip_names(skip_heavy: bool) -> Vec<String> {
    let mut names: Vec<String> = SKIP_ALWAYS.iter().map(|s| s.to_string()).collect();
    if skip_heavy {
        names.extend(SKIP_HEAVY.iter().map(|s| s.to_string()));
    }
    names
}

fn should_skip(name: &str, skip_list: &[String]) -> bool {
    skip_list.iter().any(|skip| skip.eq_ignore_ascii_case(name))
}

fn fast_dir_size(path: &Path, ctx: &ScanContext) -> (u64, u32, u32) {
    let mut size = 0u64;
    let mut files = 0u32;
    let mut dirs = 0u32;
    let mut pending = vec![path.to_path_buf()];
    while let Some(current) = pending.pop() {
        if ctx.cancelled() || ctx.is_low_memory() {
            break;
        }
        let Ok(entries) = fs::read_dir(&current) else {
            continue;
        };
        for entry in entries.flatten() {
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            let attrs = metadata.file_attributes();
            if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
                continue;
            }
            if attrs & FILE_ATTRIBUTE_DIRECTORY != 0 {
                dirs += 1;
                let name = entry.file_name().to_string_lossy().into_owned();
                if !SKIP_ALWAYS.iter().any(|skip| skip.eq_ignore_ascii_case(&name)) {
                    pending.push(entry.path());
                }
            } else {
                size += metadata.len();
                files += 1;
            }
        }
    }
    (size, files, dirs)
}

fn scan_directory(path: &Path, name: String, top: bool, depth: u32, ctx: &ScanContext) -> RawNode {
    let mut node = RawNode {
        path: path.to_path_buf(),
        is_hidden: is_hidden_name(&name),
        name,
        kind: if top && is_drive_root(path) {
            NodeKind::Drive
        } else {
            NodeKind::Directory
        },
        size: 0,
        file_count: 0,
        dir_count: 0,
        skipped: false,
        error: None,
        children: Vec::new(),
    };

    ctx.dirs.fetch_add(1, Ordering::Relaxed);
    ctx.emit_progress(path);

    let skip_list = skip_names(ctx.skip_heavy);
    let entries = match fs::read_dir(path) {
        Ok(entries) => entries,
        Err(err) => {
            node.error = Some(if err.kind() == std::io::ErrorKind::PermissionDenied {
                "Access denied".to_string()
            } else {
                err.to_string()
            });
            return node;
        }
    };

    for entry in entries.flatten() {
        if ctx.cancelled() || ctx.is_low_memory() {
            break;
        }
        let child_path = entry.path();
        let child_name = entry.file_name().to_string_lossy().into_owned();
        let metadata = match entry.metadata() {
            Ok(metadata) => metadata,
            Err(_) => continue,
        };
        let attrs = metadata.file_attributes();

        if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            let size = metadata.len();
            node.children.push(RawNode {
                path: child_path,
                name: child_name,
                kind: NodeKind::Symlink,
                size,
                file_count: 0,
                dir_count: 0,
                is_hidden: attrs & FILE_ATTRIBUTE_HIDDEN != 0,
                skipped: false,
                error: None,
                children: Vec::new(),
            });
            continue;
        }

        if attrs & FILE_ATTRIBUTE_DIRECTORY != 0 {
            let hidden = is_hidden_name(&child_name);
            if should_skip(&child_name, &skip_list) || depth >= MAX_SCAN_DEPTH {
                let (size, files, dirs) = fast_dir_size(&child_path, ctx);
                node.children.push(RawNode {
                    path: child_path,
                    name: child_name,
                    kind: NodeKind::Directory,
                    size,
                    file_count: files,
                    dir_count: dirs,
                    is_hidden: hidden,
                    skipped: true,
                    error: None,
                    children: Vec::new(),
                });
            } else {
                node.children.push(scan_directory(
                    &child_path,
                    child_name,
                    false,
                    depth + 1,
                    ctx,
                ));
            }
        } else {
            let hidden = attrs & FILE_ATTRIBUTE_HIDDEN != 0 || is_hidden_name(&child_name);
            node.children.push(RawNode {
                path: child_path,
                name: child_name,
                kind: NodeKind::File,
                size: metadata.len(),
                file_count: 0,
                dir_count: 0,
                is_hidden: hidden,
                skipped: false,
                error: None,
                children: Vec::new(),
            });
            ctx.files.fetch_add(1, Ordering::Relaxed);
            ctx.bytes.fetch_add(metadata.len(), Ordering::Relaxed);
        }
    }

    aggregate(&mut node);
    node
}

/// Root pass: immediate subdirectories are scanned in parallel, matching the
/// Qt implementation's thread-pool behaviour.
fn scan_root(path: &Path, ctx: &ScanContext) -> RawNode {
    let mut node = RawNode {
        path: path.to_path_buf(),
        name: display_name(path, true),
        kind: if is_drive_root(path) {
            NodeKind::Drive
        } else {
            NodeKind::Directory
        },
        size: 0,
        file_count: 0,
        dir_count: 0,
        is_hidden: false,
        skipped: false,
        error: None,
        children: Vec::new(),
    };

    ctx.dirs.fetch_add(1, Ordering::Relaxed);
    ctx.emit_progress(path);

    let skip_list = skip_names(ctx.skip_heavy);
    let entries = match fs::read_dir(path) {
        Ok(entries) => entries,
        Err(err) => {
            node.error = Some(if err.kind() == std::io::ErrorKind::PermissionDenied {
                "Access denied".to_string()
            } else {
                err.to_string()
            });
            return node;
        }
    };

    let mut subdirs: Vec<(PathBuf, String)> = Vec::new();
    for entry in entries.flatten() {
        if ctx.cancelled() || ctx.is_low_memory() {
            break;
        }
        let child_path = entry.path();
        let child_name = entry.file_name().to_string_lossy().into_owned();
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        let attrs = metadata.file_attributes();

        if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            node.children.push(RawNode {
                path: child_path,
                name: child_name,
                kind: NodeKind::Symlink,
                size: metadata.len(),
                file_count: 0,
                dir_count: 0,
                is_hidden: attrs & FILE_ATTRIBUTE_HIDDEN != 0,
                skipped: false,
                error: None,
                children: Vec::new(),
            });
        } else if attrs & FILE_ATTRIBUTE_DIRECTORY != 0 {
            if should_skip(&child_name, &skip_list) {
                let hidden = is_hidden_name(&child_name);
                let (size, files, dirs) = fast_dir_size(&child_path, ctx);
                node.children.push(RawNode {
                    path: child_path,
                    name: child_name,
                    kind: NodeKind::Directory,
                    size,
                    file_count: files,
                    dir_count: dirs,
                    is_hidden: hidden,
                    skipped: true,
                    error: None,
                    children: Vec::new(),
                });
            } else {
                subdirs.push((child_path, child_name));
            }
        } else {
            let hidden = attrs & FILE_ATTRIBUTE_HIDDEN != 0 || is_hidden_name(&child_name);
            node.children.push(RawNode {
                path: child_path,
                name: child_name,
                kind: NodeKind::File,
                size: metadata.len(),
                file_count: 0,
                dir_count: 0,
                is_hidden: hidden,
                skipped: false,
                error: None,
                children: Vec::new(),
            });
            ctx.files.fetch_add(1, Ordering::Relaxed);
            ctx.bytes.fetch_add(metadata.len(), Ordering::Relaxed);
        }
    }

    let scanned: Vec<RawNode> = subdirs
        .par_iter()
        .map(|(child_path, child_name)| {
            scan_directory(child_path, child_name.clone(), false, 1, ctx)
        })
        .collect();
    node.children.extend(scanned);

    aggregate(&mut node);
    node
}

fn aggregate(node: &mut RawNode) {
    if node.skipped {
        return;
    }
    let mut size = 0u64;
    let mut files = 0u32;
    let mut dirs = 0u32;
    for child in &node.children {
        size += child.size;
        match child.kind {
            NodeKind::Directory | NodeKind::Drive => {
                dirs += 1 + child.dir_count;
                files += child.file_count;
            }
            NodeKind::File => files += 1,
            _ => {}
        }
    }
    node.size = size;
    node.file_count = files;
    node.dir_count = dirs;
}

fn is_drive_root(path: &Path) -> bool {
    let text = path.to_string_lossy();
    let trimmed = text.trim_end_matches(['\\', '/']);
    trimmed.len() == 2 && trimmed.as_bytes()[1] == b':'
}

/// The MFT scanner reads the whole volume, so it pays off only for a drive root
/// and the handful of well-known huge folders directly below one.
fn should_use_mft(path: &Path) -> bool {
    if is_drive_root(path) {
        return true;
    }
    let parent_is_root = path.parent().map(is_drive_root).unwrap_or(false);
    if !parent_is_root {
        return false;
    }
    let name = path
        .file_name()
        .map(|value| value.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    matches!(
        name.as_str(),
        "users" | "windows" | "program files" | "program files (x86)" | "programdata"
    )
}

fn flatten(
    node: RawNode,
    arena: &mut Vec<NodeData>,
    by_path: &mut HashMap<String, usize>,
    parent: usize,
) -> usize {
    let index = arena.len();
    arena.push(NodeData {
        entry: NodeEntry {
            path: node.path.to_string_lossy().into_owned(),
            name: node.name,
            kind: node.kind,
            size: node.size,
            file_count: node.file_count,
            dir_count: node.dir_count,
            is_hidden: node.is_hidden,
            skipped: node.skipped,
            error: node.error,
        },
        children: Vec::new(),
        parent,
    });
    by_path.insert(path_key(&node.path), index);

    let mut children = Vec::with_capacity(node.children.len());
    for child in node.children {
        children.push(flatten(child, arena, by_path, index));
    }
    children.sort_by(|a, b| {
        let left = &arena[*a].entry;
        let right = &arena[*b].entry;
        right
            .size
            .cmp(&left.size)
            .then_with(|| left.name.to_lowercase().cmp(&right.name.to_lowercase()))
    });
    arena[index].children = children;
    index
}

impl ScanState {
    fn entry(&self, index: usize) -> NodeEntry {
        self.arena[index].entry.clone()
    }

    /// Drops a deleted path (and its subtree) from the scanned tree and
    /// subtracts its contribution from every ancestor.
    pub(crate) fn remove_path(&mut self, path: &Path) -> bool {
        let Some(index) = self.find(path) else {
            return false;
        };
        let entry = self.arena[index].entry.clone();
        let parent = self.arena[index].parent;

        let mut removed = Vec::new();
        let mut stack = vec![index];
        while let Some(current) = stack.pop() {
            removed.push(current);
            stack.extend(self.arena[current].children.iter().copied());
        }
        if parent != usize::MAX {
            self.arena[parent].children.retain(|child| *child != index);
        }
        for current in &removed {
            self.by_path
                .remove(&path_key(Path::new(&self.arena[*current].entry.path)));
        }

        let is_directory = matches!(entry.kind, NodeKind::Directory | NodeKind::Drive);
        let mut cursor = parent;
        while cursor != usize::MAX {
            let node = &mut self.arena[cursor];
            node.entry.size = node.entry.size.saturating_sub(entry.size);
            if is_directory {
                node.entry.file_count = node.entry.file_count.saturating_sub(entry.file_count);
                node.entry.dir_count = node
                    .entry
                    .dir_count
                    .saturating_sub(1 + entry.dir_count);
            } else if entry.kind == NodeKind::File {
                node.entry.file_count = node.entry.file_count.saturating_sub(1);
            }
            cursor = node.parent;
        }
        true
    }

    fn child_entries(&self, index: usize) -> Vec<NodeEntry> {
        self.arena[index]
            .children
            .iter()
            .map(|child| self.entry(*child))
            .collect()
    }

    fn find(&self, path: &Path) -> Option<usize> {
        self.by_path.get(&path_key(path)).copied()
    }

    /// Entry lookup for modules outside the scan commands (cleanup, migrate).
    pub(crate) fn lookup_entry(&self, path: &Path) -> Option<NodeEntry> {
        self.find(path).map(|index| self.entry(index))
    }

    /// Every file in the scanned tree, used by the cleanup and duplicate scans.
    pub(crate) fn file_entries(&self) -> Vec<NodeEntry> {
        self.arena
            .iter()
            .filter(|node| node.entry.kind == NodeKind::File)
            .map(|node| node.entry.clone())
            .collect()
    }

    /// Visits every file without cloning the whole tree (memory friendly).
    pub(crate) fn for_each_file(&self, mut visit: impl FnMut(&NodeEntry)) {
        for node in self.arena.iter() {
            if node.entry.kind == NodeKind::File {
                visit(&node.entry);
            }
        }
    }
}

impl ScanState {

    fn treemap(&self, index: usize, depth: u32, show_files: bool) -> TreemapNode {
        let entry = &self.arena[index].entry;
        let mut children = Vec::new();
        if depth > 0 {
            for child in &self.arena[index].children {
                let child_entry = &self.arena[*child].entry;
                if !show_files && child_entry.kind == NodeKind::File {
                    continue;
                }
                children.push(self.treemap(*child, depth - 1, show_files));
            }
        }
        TreemapNode {
            path: entry.path.clone(),
            name: entry.name.clone(),
            kind: entry.kind,
            size: entry.size,
            children,
        }
    }

    fn search(&self, start: usize, query: &str, limit: usize, results: &mut Vec<NodeEntry>) {
        if results.len() >= limit {
            return;
        }
        let entry = &self.arena[start].entry;
        if wildcard_match(&entry.name, query) {
            results.push(entry.clone());
        }
        for child in &self.arena[start].children {
            if results.len() >= limit {
                break;
            }
            self.search(*child, query, limit, results);
        }
    }
}

fn wildcard_match(text: &str, pattern: &str) -> bool {
    let text: Vec<char> = text.to_lowercase().chars().collect();
    let pattern: Vec<char> = pattern.to_lowercase().chars().collect();
    let (mut ti, mut pi) = (0usize, 0usize);
    let (mut star, mut matched) = (None, 0usize);
    while ti < text.len() {
        if pi < pattern.len() && (pattern[pi] == '?' || pattern[pi] == text[ti]) {
            ti += 1;
            pi += 1;
        } else if pi < pattern.len() && pattern[pi] == '*' {
            star = Some(pi);
            matched = ti;
            pi += 1;
        } else if let Some(star_index) = star {
            pi = star_index + 1;
            matched += 1;
            ti = matched;
        } else {
            return false;
        }
    }
    while pi < pattern.len() && pattern[pi] == '*' {
        pi += 1;
    }
    pi == pattern.len()
}

#[tauri::command]
pub fn start_scan(
    app: AppHandle,
    store: State<'_, ScanStore>,
    root: String,
    skip_heavy: bool,
) -> Result<(), String> {
    let root_path = PathBuf::from(&root);
    if !root_path.exists() {
        return Err(format!("path not found: {root}"));
    }

    let cancel = {
        let mut current = store.cancel.lock().map_err(|_| "scan state poisoned")?;
        if let Some(old) = current.take() {
            old.store(true, Ordering::Relaxed);
        }
        let flag = Arc::new(AtomicBool::new(false));
        *current = Some(flag.clone());
        flag
    };
    let generation = store.generation.fetch_add(1, Ordering::SeqCst) + 1;

    std::thread::spawn(move || {
        let store = app.state::<ScanStore>();
        let started = Instant::now();
        let root_display = normalize(&root_path);

        let workers = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(4)
            .clamp(2, 8);
        let pool = match rayon::ThreadPoolBuilder::new().num_threads(workers).build() {
            Ok(pool) => pool,
            Err(err) => {
                let _ = app.emit(
                    "scan://failed",
                    ScanFailure {
                        code: "failed".to_string(),
                        message: err.to_string(),
                    },
                );
                return;
            }
        };

        let ctx = ScanContext {
            cancel: cancel.clone(),
            app: app.clone(),
            files: AtomicU64::new(0),
            dirs: AtomicU64::new(0),
            bytes: AtomicU64::new(0),
            last_emit: AtomicU64::new(0),
            low_memory: AtomicBool::new(false),
            skip_heavy,
        };

        let mut engine = "walk";
        let raw = if should_use_mft(&root_display) && mft::is_supported(&root_display) {
            let low_memory = || ctx.is_low_memory();
            let mut on_progress = |phase: mft::MftPhase, count: u64| {
                let phase_name = match phase {
                    mft::MftPhase::Enumerating => "enumerating",
                    mft::MftPhase::ReadingMft => "readingMft",
                    mft::MftPhase::BuildingTree => "buildingTree",
                };
                let _ = app.emit(
                    "scan://progress",
                    ScanProgress {
                        path: String::new(),
                        files: 0,
                        dirs: 0,
                        bytes: 0,
                        engine: "mft".to_string(),
                        phase: phase_name.to_string(),
                        count: Some(count),
                    },
                );
            };
            match mft::scan_volume(
                &root_display,
                &cancel,
                skip_heavy,
                &low_memory,
                &mut on_progress,
            ) {
                Ok(node) => {
                    engine = "mft";
                    node
                }
                Err(mft::MftError::Cancelled) => {
                    let _ = app.emit(
                        "scan://failed",
                        ScanFailure {
                            code: "cancelled".to_string(),
                            message: "scan cancelled".to_string(),
                        },
                    );
                    return;
                }
                Err(mft::MftError::Failed(err)) => {
                    log::warn!(
                        "MFT scan unavailable for {}: {err}; falling back to directory walk",
                        root_display.display()
                    );
                    pool.install(|| scan_root(&root_display, &ctx))
                }
            }
        } else {
            pool.install(|| scan_root(&root_display, &ctx))
        };

        if cancel.load(Ordering::Relaxed) {
            let _ = app.emit(
                "scan://failed",
                ScanFailure {
                    code: "cancelled".to_string(),
                    message: "scan cancelled".to_string(),
                },
            );
            return;
        }

        let mut arena = Vec::new();
        let mut by_path = HashMap::new();
        let root_index = flatten(raw, &mut arena, &mut by_path, usize::MAX);
        let state = ScanState {
            arena,
            by_path,
            root: root_index,
        };
        let summary = state.entry(root_index);
        let low_memory = ctx.low_memory.load(Ordering::Relaxed);
        log::info!(
            "scan finished: {} ({} files, {} dirs, {} bytes) in {} ms [engine={}]{}",
            summary.path,
            summary.file_count,
            summary.dir_count,
            summary.size,
            started.elapsed().as_millis(),
            engine,
            if low_memory { " [low memory]" } else { "" }
        );

        if let Ok(mut guard) = store_guard(&store) {
            *guard = Some(state);
        }
        if store.generation.load(Ordering::SeqCst) != generation {
            return;
        }

        if low_memory {
            let _ = app.emit(
                "scan://low-memory",
                ScanFailure {
                    code: "lowMemory".to_string(),
                    message: "low memory".to_string(),
                },
            );
        }
        let _ = app.emit(
            "scan://finished",
            ScanFinished {
                root: summary,
                elapsed_ms: started.elapsed().as_millis() as u64,
                engine: engine.to_string(),
            },
        );
    });

    Ok(())
}

pub(crate) fn remove_paths(store: &ScanStore, paths: &[String]) -> usize {
    let Ok(mut guard) = store.inner.lock() else {
        return 0;
    };
    let Some(state) = guard.as_mut() else {
        return 0;
    };
    let mut removed = 0;
    for path in paths {
        if state.remove_path(Path::new(path)) {
            removed += 1;
        }
    }
    removed
}

fn store_guard<'a>(
    store: &'a State<'_, ScanStore>,
) -> Result<std::sync::MutexGuard<'a, Option<ScanState>>, String> {
    store.inner.lock().map_err(|_| "scan state poisoned".to_string())
}

#[tauri::command]
pub fn cancel_scan(store: State<'_, ScanStore>) -> Result<(), String> {
    let current = store.cancel.lock().map_err(|_| "scan state poisoned")?;
    if let Some(flag) = current.as_ref() {
        flag.store(true, Ordering::Relaxed);
    }
    Ok(())
}

#[tauri::command]
pub fn get_directory(
    store: State<'_, ScanStore>,
    path: String,
) -> Result<DirectoryView, String> {
    let guard = store_guard(&store)?;
    let state = guard.as_ref().ok_or("no scan result")?;
    let index = state.find(Path::new(&path)).ok_or("path not in scan result")?;
    Ok(DirectoryView {
        node: state.entry(index),
        children: state.child_entries(index),
    })
}

#[tauri::command]
pub fn get_treemap(
    store: State<'_, ScanStore>,
    path: String,
    depth: Option<u32>,
    show_files: Option<bool>,
) -> Result<TreemapNode, String> {
    let guard = store_guard(&store)?;
    let state = guard.as_ref().ok_or("no scan result")?;
    let index = state.find(Path::new(&path)).ok_or("path not in scan result")?;
    Ok(state.treemap(index, depth.unwrap_or(1), show_files.unwrap_or(true)))
}

#[tauri::command]
pub fn search_tree(
    store: State<'_, ScanStore>,
    path: String,
    query: String,
    limit: Option<usize>,
) -> Result<Vec<NodeEntry>, String> {
    let trimmed = query.trim();
    if trimmed.is_empty() {
        return Ok(Vec::new());
    }
    let guard = store_guard(&store)?;
    let state = guard.as_ref().ok_or("no scan result")?;
    let index = state.find(Path::new(&path)).ok_or("path not in scan result")?;
    let mut results = Vec::new();
    let pattern = if trimmed.contains('*') || trimmed.contains('?') {
        trimmed.to_string()
    } else {
        format!("*{trimmed}*")
    };
    state.search(index, &pattern, limit.unwrap_or(500), &mut results);
    Ok(results)
}

#[tauri::command]
pub fn get_scan_root(store: State<'_, ScanStore>) -> Result<Option<NodeEntry>, String> {
    let guard = store_guard(&store)?;
    Ok(guard.as_ref().map(|state| state.entry(state.root)))
}

#[tauri::command]
pub fn default_path(_app: AppHandle) -> Result<String, String> {
    Ok("C:\\".to_string())
}

#[tauri::command]
pub fn disk_info(path: String) -> Result<DiskInfo, String> {
    let wide: Vec<u16> = OsStr::new(&path)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let mut free = 0u64;
    let mut total = 0u64;
    let ok = unsafe {
        GetDiskFreeSpaceExW(
            wide.as_ptr(),
            &mut free,
            &mut total,
            std::ptr::null_mut(),
        )
    };
    if ok == 0 {
        return Err("cannot read disk space".to_string());
    }
    Ok(DiskInfo { free, total })
}
