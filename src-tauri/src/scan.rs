use std::collections::HashMap;
use std::ffi::OsStr;
use std::fs;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

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
    #[serde(skip_serializing_if = "Option::is_none")]
    pub elapsed_ms: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub files_per_sec: Option<u64>,
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
    pub total: u32,
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

#[derive(Clone)]
struct NodeData {
    name: Box<str>,
    kind: NodeKind,
    size: u64,
    file_count: u32,
    dir_count: u32,
    is_hidden: bool,
    skipped: bool,
    error: Option<Box<str>>,
    parent: u32,
    children: Vec<u32>,
}

#[derive(Clone, Default)]
pub struct ScanState {
    arena: Vec<NodeData>,
    by_path: HashMap<String, u32>, // 只索引目录/盘符
    root: u32,
    root_path: Box<str>,
}

#[derive(Default)]
pub struct ScanStore {
    inner: std::sync::RwLock<Option<Arc<ScanState>>>,
    cancel: Mutex<Option<Arc<AtomicBool>>>,
    /// Held for the whole build so only one whole-volume scan runs at a time.
    running: Mutex<()>,
    generation: AtomicU64,
    active: AtomicBool,
}

impl ScanStore {
    /// Cheap snapshot of the current result: the lock is released immediately.
    pub(crate) fn snapshot(&self) -> Option<Arc<ScanState>> {
        self.inner.read().ok().and_then(|guard| guard.clone())
    }

    /// Runs `f` against the current scan result, if one exists.
    pub(crate) fn with_state<R>(&self, f: impl FnOnce(&ScanState) -> R) -> Option<R> {
        let state = self.snapshot()?;
        Some(f(state.as_ref()))
    }

    pub(crate) fn is_scanning(&self) -> bool {
        self.active.load(Ordering::Relaxed)
    }
}

/// Clears the "scanning" flag on every exit path, including early returns.
/// Only the thread of the latest generation may clear it.
struct ScanActive {
    app: AppHandle,
    generation: u64,
}

impl Drop for ScanActive {
    fn drop(&mut self) {
        let store = self.app.state::<ScanStore>();
        if store.generation.load(Ordering::SeqCst) == self.generation {
            store.active.store(false, Ordering::SeqCst);
        }
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
    generation: u64,
    files: AtomicU64,
    dirs: AtomicU64,
    bytes: AtomicU64,
    last_emit: AtomicU64,
    low_memory: AtomicBool,
    skip_heavy: bool,
    started: Instant,
    last_mem_check: AtomicU64,
    mem_checks: AtomicU64,
}

impl ScanContext {
    fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }

    fn is_low_memory(&self) -> bool {
        if self.low_memory.load(Ordering::Relaxed) {
            return true;
        }
        let count = self.mem_checks.fetch_add(1, Ordering::Relaxed) + 1;
        let now = self.started.elapsed().as_millis() as u64;
        let last = self.last_mem_check.load(Ordering::Relaxed);
        if count % 2048 != 0 && now.saturating_sub(last) < 500 {
            return false;
        }
        if self
            .last_mem_check
            .compare_exchange(last, now, Ordering::Relaxed, Ordering::Relaxed)
            .is_err()
        {
            return false;
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
        // Superseded scans must stay silent so they cannot flip the UI state.
        if self.app.state::<ScanStore>().generation.load(Ordering::SeqCst) != self.generation {
            return;
        }
        let now = self.started.elapsed().as_millis() as u64;
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
        let elapsed = now;
        let files = self.files.load(Ordering::Relaxed);
        let _ = self.app.emit(
            "scan://progress",
            ScanProgress {
                path: path.to_string_lossy().into_owned(),
                files,
                dirs: self.dirs.load(Ordering::Relaxed),
                bytes: self.bytes.load(Ordering::Relaxed),
                engine: "walk".to_string(),
                phase: "walking".to_string(),
                count: None,
                elapsed_ms: Some(elapsed),
                files_per_sec: if elapsed == 0 {
                    None
                } else {
                    Some(files * 1000 / elapsed.max(1))
                },
            },
        );
    }
}

fn path_key_str(text: &str) -> String {
    let text = text.to_lowercase();
    if text.len() > 3 {
        text.trim_end_matches(['\\', '/']).to_string()
    } else {
        text
    }
}

fn path_key(path: &Path) -> String {
    path_key_str(&path.to_string_lossy())
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

fn should_skip(name: &str, skip_heavy: bool) -> bool {
    name.eq_ignore_ascii_case("winsxs") || (skip_heavy && name.eq_ignore_ascii_case("node_modules"))
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
            // 仅跳过目录型重解析点（junction/符号链接）；文件型重解析点
            // （云占位、AppX 存根等）是真实文件，必须计入大小
            if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0
                && attrs & FILE_ATTRIBUTE_DIRECTORY != 0
            {
                continue;
            }
            if attrs & FILE_ATTRIBUTE_DIRECTORY != 0 {
                dirs += 1;
                let name = entry.file_name().to_string_lossy().into_owned();
                if !name.eq_ignore_ascii_case("winsxs") {
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
        let metadata = match entry.metadata() {
            Ok(metadata) => metadata,
            Err(_) => continue,
        };
        let attrs = metadata.file_attributes();

        // 目录型重解析点作为链接展示，不展开；文件型重解析点按普通文件统计
        if attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0 && attrs & FILE_ATTRIBUTE_DIRECTORY != 0 {
            let size = metadata.len();
            node.children.push(RawNode {
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
            if should_skip(&child_name, ctx.skip_heavy) || depth >= MAX_SCAN_DEPTH {
                let (size, files, dirs) = fast_dir_size(&child_path, ctx);
                node.children.push(RawNode {
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

    let scanned: Vec<RawNode> = if depth < 2 && subdirs.len() > 1 {
        subdirs
            .par_iter()
            .map(|(child_path, child_name)| {
                scan_directory(child_path, child_name.clone(), false, depth + 1, ctx)
            })
            .collect()
    } else {
        subdirs
            .into_iter()
            .map(|(child_path, child_name)| {
                scan_directory(&child_path, child_name, false, depth + 1, ctx)
            })
            .collect()
    };
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

static SCAN_POOL: std::sync::OnceLock<Option<rayon::ThreadPool>> = std::sync::OnceLock::new();

fn scan_pool() -> Option<&'static rayon::ThreadPool> {
    SCAN_POOL
        .get_or_init(|| {
            let workers = std::thread::available_parallelism()
                .map(|n| n.get())
                .unwrap_or(4)
                .clamp(2, 8);
            rayon::ThreadPoolBuilder::new()
                .num_threads(workers)
                .build()
                .ok()
        })
        .as_ref()
}

/// Compares names case-insensitively without allocating.
fn compare_name(left: &str, right: &str) -> std::cmp::Ordering {
    let mut left = left.chars();
    let mut right = right.chars();
    loop {
        match (left.next(), right.next()) {
            (None, None) => return std::cmp::Ordering::Equal,
            (None, Some(_)) => return std::cmp::Ordering::Less,
            (Some(_), None) => return std::cmp::Ordering::Greater,
            (Some(a), Some(b)) => {
                let ordering = a.to_ascii_lowercase().cmp(&b.to_ascii_lowercase());
                if ordering != std::cmp::Ordering::Equal {
                    return ordering;
                }
            }
        }
    }
}

fn flatten(
    node: RawNode,
    arena: &mut Vec<NodeData>,
    by_path: &mut HashMap<String, u32>,
    parent: u32,
    parent_path: &str,
    is_root: bool,
    cancel: &AtomicBool,
) -> Option<u32> {
    if cancel.load(Ordering::Relaxed) {
        return None;
    }
    let RawNode {
        name,
        kind,
        size,
        file_count,
        dir_count,
        is_hidden,
        skipped,
        error,
        children: raw_children,
        ..
    } = node;
    let is_dir = matches!(kind, NodeKind::Directory | NodeKind::Drive);
    let full_path = if is_root {
        parent_path.to_string()
    } else if is_dir {
        let mut text = String::with_capacity(parent_path.len() + 1 + name.len());
        text.push_str(parent_path);
        if !parent_path.ends_with('\\') && !parent_path.ends_with('/') {
            text.push('\\');
        }
        text.push_str(&name);
        text
    } else {
        String::new()
    };

    let index = arena.len() as u32;
    arena.push(NodeData {
        name: name.into_boxed_str(),
        kind,
        size,
        file_count,
        dir_count,
        is_hidden,
        skipped,
        error: error.map(|value| value.into_boxed_str()),
        parent,
        children: Vec::new(),
    });
    if is_dir {
        by_path.insert(path_key_str(&full_path), index);
    }

    let child_parent_path: &str = if is_dir { &full_path } else { parent_path };
    let mut children = Vec::with_capacity(raw_children.len());
    for child in raw_children {
        if cancel.load(Ordering::Relaxed) {
            return None;
        }
        children.push(flatten(
            child,
            arena,
            by_path,
            index,
            child_parent_path,
            false,
            cancel,
        )?);
    }
    children.sort_by(|a, b| {
        let left = &arena[*a as usize];
        let right = &arena[*b as usize];
        right
            .size
            .cmp(&left.size)
            .then_with(|| compare_name(&left.name, &right.name))
    });
    arena[index as usize].children = children;
    Some(index)
}

/// Counts the flat arena size the tree needs so `flatten` can preallocate.
fn raw_stats(node: &RawNode) -> (usize, usize) {
    let mut nodes = 0usize;
    let mut dirs = 0usize;
    let mut stack: Vec<&RawNode> = vec![node];
    while let Some(current) = stack.pop() {
        nodes += 1;
        if matches!(current.kind, NodeKind::Directory | NodeKind::Drive) {
            dirs += 1;
        }
        stack.extend(current.children.iter());
    }
    (nodes, dirs)
}

impl ScanState {
    fn path_of(&self, index: u32) -> String {
        if index == self.root {
            return self.root_path.to_string();
        }
        let mut names: Vec<&str> = Vec::new();
        let mut cursor = index;
        let mut guard = 0;
        while cursor != self.root && cursor != u32::MAX && guard < 1024 {
            let node = &self.arena[cursor as usize];
            names.push(&node.name);
            cursor = node.parent;
            guard += 1;
        }
        let mut text = String::with_capacity(self.root_path.len() + 64);
        text.push_str(&self.root_path);
        for name in names.iter().rev() {
            if !text.ends_with('\\') && !text.ends_with('/') {
                text.push('\\');
            }
            text.push_str(name);
        }
        text
    }

    fn entry(&self, index: u32) -> NodeEntry {
        let node = &self.arena[index as usize];
        NodeEntry {
            path: self.path_of(index),
            name: node.name.to_string(),
            kind: node.kind,
            size: node.size,
            file_count: node.file_count,
            dir_count: node.dir_count,
            is_hidden: node.is_hidden,
            skipped: node.skipped,
            error: node.error.as_ref().map(|value| value.to_string()),
        }
    }

    /// Drops a deleted path (and its subtree) from the scanned tree and
    /// subtracts its contribution from every ancestor.
    pub(crate) fn remove_path(&mut self, path: &Path) -> bool {
        let Some(index) = self.find(path) else {
            return false;
        };
        let node = &self.arena[index as usize];
        let size = node.size;
        let file_count = node.file_count;
        let dir_count = node.dir_count;
        let kind = node.kind;
        let parent = node.parent;

        let mut removed = Vec::new();
        let mut stack = vec![index];
        while let Some(current) = stack.pop() {
            removed.push(current);
            stack.extend(self.arena[current as usize].children.iter().copied());
        }
        if parent != u32::MAX {
            self.arena[parent as usize]
                .children
                .retain(|child| *child != index);
        }
        for current in &removed {
            if matches!(
                self.arena[*current as usize].kind,
                NodeKind::Directory | NodeKind::Drive
            ) {
                self.by_path
                    .remove(&path_key(Path::new(&self.path_of(*current))));
            }
        }

        let is_directory = matches!(kind, NodeKind::Directory | NodeKind::Drive);
        let mut cursor = parent;
        while cursor != u32::MAX {
            let node = &mut self.arena[cursor as usize];
            node.size = node.size.saturating_sub(size);
            if is_directory {
                node.file_count = node.file_count.saturating_sub(file_count);
                node.dir_count = node.dir_count.saturating_sub(1 + dir_count);
            } else if kind == NodeKind::File {
                node.file_count = node.file_count.saturating_sub(1);
            }
            cursor = node.parent;
        }
        true
    }

    pub(crate) fn directory_view(
        &self,
        index: u32,
        offset: Option<usize>,
        limit: Option<usize>,
    ) -> DirectoryView {
        let children = &self.arena[index as usize].children;
        let total = children.len();
        let start = offset.unwrap_or(0).min(total);
        let end = limit.map(|count| (start + count).min(total)).unwrap_or(total);
        let items = children[start..end]
            .iter()
            .map(|child| self.entry(*child))
            .collect();
        DirectoryView {
            node: self.entry(index),
            children: items,
            total: total as u32,
        }
    }

    fn find(&self, path: &Path) -> Option<u32> {
        if let Some(index) = self.by_path.get(&path_key(path)) {
            return Some(*index);
        }
        let parent_index = *self.by_path.get(&path_key(path.parent()?))?;
        let name = path.file_name()?.to_string_lossy();
        self.arena[parent_index as usize]
            .children
            .iter()
            .copied()
            .find(|child| {
                let node = &self.arena[*child as usize];
                node.name.eq_ignore_ascii_case(name.as_ref())
            })
    }

    /// Entry lookup for modules outside the scan commands (cleanup, migrate).
    pub(crate) fn lookup_entry(&self, path: &Path) -> Option<NodeEntry> {
        self.find(path).map(|index| self.entry(index))
    }

    /// Every file in the scanned tree, used by the cleanup and duplicate scans.
    pub(crate) fn file_entries(&self) -> Vec<NodeEntry> {
        (0..self.arena.len() as u32)
            .filter(|index| self.arena[*index as usize].kind == NodeKind::File)
            .map(|index| self.entry(index))
            .collect()
    }

    /// Visits every file without cloning the whole tree (memory friendly).
    pub(crate) fn for_each_file(&self, mut visit: impl FnMut(NodeEntry)) {
        for index in 0..self.arena.len() as u32 {
            if self.arena[index as usize].kind == NodeKind::File {
                visit(self.entry(index));
            }
        }
    }

    fn treemap(&self, index: u32, depth: u32, show_files: bool) -> TreemapNode {
        let node = &self.arena[index as usize];
        let mut children = Vec::new();
        if depth > 0 {
            for child in &node.children {
                if !show_files && self.arena[*child as usize].kind == NodeKind::File {
                    continue;
                }
                children.push(self.treemap(*child, depth - 1, show_files));
            }
        }
        TreemapNode {
            path: self.path_of(index),
            name: node.name.to_string(),
            kind: node.kind,
            size: node.size,
            children,
        }
    }

    fn search(&self, start: u32, query: &str, limit: usize, results: &mut Vec<NodeEntry>) {
        if results.len() >= limit {
            return;
        }
        let node = &self.arena[start as usize];
        if wildcard_match(&node.name, query) {
            results.push(self.entry(start));
        }
        for child in &node.children {
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
    store.active.store(true, Ordering::SeqCst);
    // Release the previous tree now instead of keeping it until the new one is
    // built: a whole-volume rescan otherwise holds two full trees at once.
    {
        let mut guard = store
            .inner
            .write()
            .map_err(|_| "scan state poisoned")?;
        *guard = None;
    }

    std::thread::spawn(move || {
        let store = app.state::<ScanStore>();
        // A thread that was already superseded has no work to do.
        if store.generation.load(Ordering::SeqCst) != generation {
            return;
        }
        // Only one build runs at a time; wait until the previous tree (and its
        // temporary structures) has been dropped before starting a new one.
        let _run = store.running.lock().unwrap_or_else(|err| err.into_inner());
        let _active = ScanActive {
            app: app.clone(),
            generation,
        };
        if store.generation.load(Ordering::SeqCst) != generation {
            return;
        }
        let started = Instant::now();
        let root_display = normalize(&root_path);
        let root_path = root_display.to_string_lossy().into_owned();

        let ctx = ScanContext {
            cancel: cancel.clone(),
            app: app.clone(),
            generation,
            files: AtomicU64::new(0),
            dirs: AtomicU64::new(0),
            bytes: AtomicU64::new(0),
            last_emit: AtomicU64::new(0),
            low_memory: AtomicBool::new(false),
            skip_heavy,
            started,
            last_mem_check: AtomicU64::new(0),
            mem_checks: AtomicU64::new(0),
        };

        let walk = || match scan_pool() {
            Some(pool) => pool.install(|| {
                scan_directory(
                    &root_display,
                    display_name(&root_display, true),
                    true,
                    0,
                    &ctx,
                )
            }),
            None => scan_directory(
                &root_display,
                display_name(&root_display, true),
                true,
                0,
                &ctx,
            ),
        };

        let mut engine = "walk";
        let raw = if should_use_mft(&root_display) && mft::is_supported(&root_display) {
            let low_memory = || ctx.is_low_memory();
            let mut on_progress = |phase: mft::MftPhase, count: u64| {
                if store.generation.load(Ordering::SeqCst) != generation {
                    return;
                }
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
                        elapsed_ms: Some(started.elapsed().as_millis() as u64),
                        files_per_sec: None,
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
                    if store.generation.load(Ordering::SeqCst) == generation {
                        let _ = app.emit(
                            "scan://failed",
                            ScanFailure {
                                code: "cancelled".to_string(),
                                message: "scan cancelled".to_string(),
                            },
                        );
                    }
                    return;
                }
                Err(err @ (mft::MftError::Failed(_) | mft::MftError::JournalReset)) => {
                    log::warn!(
                        "MFT scan unavailable for {}: {err:?}; falling back to directory walk",
                        root_display.display()
                    );
                    walk()
                }
            }
        } else {
            walk()
        };

        if cancel.load(Ordering::Relaxed) {
            if store.generation.load(Ordering::SeqCst) == generation {
                let _ = app.emit(
                    "scan://failed",
                    ScanFailure {
                        code: "cancelled".to_string(),
                        message: "scan cancelled".to_string(),
                    },
                );
            }
            return;
        }
        if store.generation.load(Ordering::SeqCst) != generation {
            return;
        }

        let (node_count, dir_count) = raw_stats(&raw);
        let mut arena = Vec::with_capacity(node_count);
        let mut by_path = HashMap::with_capacity(dir_count);
        let Some(root_index) = flatten(
            raw,
            &mut arena,
            &mut by_path,
            u32::MAX,
            &root_path,
            true,
            &cancel,
        ) else {
            return;
        };
        let state = ScanState {
            arena,
            by_path,
            root: root_index,
            root_path: root_path.into_boxed_str(),
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

        if store.generation.load(Ordering::SeqCst) != generation {
            return;
        }
        if let Ok(mut guard) = store.inner.write() {
            if store.generation.load(Ordering::SeqCst) != generation {
                return;
            }
            *guard = Some(Arc::new(state));
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
    let Ok(mut guard) = store.inner.write() else {
        return 0;
    };
    let Some(current) = guard.as_mut() else {
        return 0;
    };
    let state = Arc::make_mut(current);
    let mut removed = 0;
    for path in paths {
        if state.remove_path(Path::new(path)) {
            removed += 1;
        }
    }
    removed
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
pub fn release_scan(store: State<'_, ScanStore>) -> Result<(), String> {
    let mut guard = store.inner.write().map_err(|_| "scan state poisoned")?;
    *guard = None;
    Ok(())
}

#[tauri::command]
pub fn get_directory(
    store: State<'_, ScanStore>,
    path: String,
    offset: Option<usize>,
    limit: Option<usize>,
) -> Result<DirectoryView, String> {
    let state = store.snapshot().ok_or("no scan result")?;
    let index = state
        .find(Path::new(&path))
        .ok_or("path not in scan result")?;
    Ok(state.directory_view(index, offset, limit))
}

#[tauri::command]
pub fn get_treemap(
    store: State<'_, ScanStore>,
    path: String,
    depth: Option<u32>,
    show_files: Option<bool>,
) -> Result<TreemapNode, String> {
    let state = store.snapshot().ok_or("no scan result")?;
    let index = state
        .find(Path::new(&path))
        .ok_or("path not in scan result")?;
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
    let state = store.snapshot().ok_or("no scan result")?;
    let index = state
        .find(Path::new(&path))
        .ok_or("path not in scan result")?;
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
    Ok(store
        .snapshot()
        .map(|state| state.entry(state.root)))
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

#[cfg(test)]
mod tests {
    use super::*;

    fn file(name: &str, size: u64) -> RawNode {
        RawNode {
            name: name.to_string(),
            kind: NodeKind::File,
            size,
            file_count: 0,
            dir_count: 0,
            is_hidden: false,
            skipped: false,
            error: None,
            children: Vec::new(),
        }
    }

    fn dir(name: &str, size: u64, file_count: u32, dir_count: u32, children: Vec<RawNode>) -> RawNode {
        RawNode {
            name: name.to_string(),
            kind: NodeKind::Directory,
            size,
            file_count,
            dir_count,
            is_hidden: false,
            skipped: false,
            error: None,
            children,
        }
    }

    fn sample_root() -> RawNode {
        RawNode {
            name: "C:".to_string(),
            kind: NodeKind::Drive,
            size: 300,
            file_count: 4,
            dir_count: 2,
            is_hidden: false,
            skipped: false,
            error: None,
            children: vec![
                dir(
                    "Alpha",
                    100,
                    2,
                    0,
                    vec![file("Beta.bin", 60), file("alpha.bin", 40)],
                ),
                dir(
                    "beta",
                    200,
                    2,
                    0,
                    vec![file("Beta.txt", 50), file("alpha.txt", 50)],
                ),
            ],
        }
    }

    fn sample_state() -> ScanState {
        let mut arena = Vec::new();
        let mut by_path = HashMap::new();
        let root = flatten(
            sample_root(),
            &mut arena,
            &mut by_path,
            u32::MAX,
            r"C:\",
            true,
            &AtomicBool::new(false),
        )
        .expect("flatten");
        ScanState {
            arena,
            by_path,
            root,
            root_path: r"C:\".to_string().into_boxed_str(),
        }
    }

    #[test]
    fn flatten_indexes_directories_and_resolves_paths() {
        let state = sample_state();

        let alpha = state.find(Path::new(r"C:\Alpha")).expect("Alpha dir");
        let alpha_entry = state.entry(alpha);
        assert_eq!(alpha_entry.path, r"C:\Alpha");
        assert_eq!(alpha_entry.kind, NodeKind::Directory);

        // Path lookups ignore case.
        assert!(state.find(Path::new(r"c:\alpha")).is_some());

        // Files are resolved through their parent directory.
        let beta_bin = state
            .find(Path::new(r"C:\Alpha\Beta.bin"))
            .expect("file in Alpha");
        let beta_entry = state.entry(beta_bin);
        assert_eq!(beta_entry.name, "Beta.bin");
        assert_eq!(beta_entry.path, r"C:\Alpha\Beta.bin");
        assert_eq!(beta_entry.kind, NodeKind::File);
        assert!(state.find(Path::new(r"c:\alpha\BETA.BIN")).is_some());

        // Root is reachable through its own key.
        let root = state.find(Path::new(r"C:\")).expect("root");
        assert_eq!(root, state.root);
        assert_eq!(state.entry(root).path, r"C:\");
    }

    #[test]
    fn flatten_sorts_children_by_size_then_name_ignoring_case() {
        let state = sample_state();

        let root = state.directory_view(state.root, None, None);
        assert_eq!(root.total, 2);
        let names: Vec<&str> = root.children.iter().map(|child| child.name.as_str()).collect();
        // "beta" (200) before "Alpha" (100).
        assert_eq!(names, vec!["beta", "Alpha"]);

        let beta = state
            .find(Path::new(r"C:\beta"))
            .expect("beta dir");
        let view = state.directory_view(beta, None, None);
        let names: Vec<&str> = view.children.iter().map(|child| child.name.as_str()).collect();
        // Equal sizes fall back to a case-insensitive name order.
        assert_eq!(names, vec!["alpha.txt", "Beta.txt"]);

        let paged = state.directory_view(state.root, Some(1), Some(1));
        assert_eq!(paged.total, 2);
        assert_eq!(paged.children.len(), 1);
        assert_eq!(paged.children[0].name, "Alpha");
    }

    #[test]
    fn remove_path_updates_ancestors_and_drops_lookup() {
        let mut state = sample_state();

        assert!(state.remove_path(Path::new(r"C:\Alpha")));
        assert!(state.find(Path::new(r"C:\Alpha")).is_none());

        let root = state.entry(state.root);
        assert_eq!(root.size, 200);
        assert_eq!(root.file_count, 2);
        assert_eq!(root.dir_count, 1);

        assert!(state.remove_path(Path::new(r"C:\beta\Beta.txt")));
        assert!(state.find(Path::new(r"C:\beta\Beta.txt")).is_none());

        let root = state.entry(state.root);
        assert_eq!(root.size, 150);
        assert_eq!(root.file_count, 1);
        assert_eq!(root.dir_count, 1);

        let beta = state.find(Path::new(r"C:\beta")).expect("beta dir");
        let view = state.directory_view(beta, None, None);
        assert_eq!(view.total, 1);
        assert_eq!(view.children[0].name, "alpha.txt");
    }

    #[test]
    fn search_and_treemap_keep_full_paths() {
        let state = sample_state();
        let mut results = Vec::new();
        state.search(state.root, "*alpha*", 10, &mut results);
        assert!(results.iter().any(|entry| entry.path == r"C:\Alpha\alpha.bin"));
        assert!(results.iter().any(|entry| entry.path == r"C:\beta\alpha.txt"));

        let tree = state.treemap(state.root, 1, true);
        assert_eq!(tree.path, r"C:\");
        assert!(tree.children.iter().any(|child| child.path == r"C:\beta"));
        assert!(tree.children.iter().any(|child| child.path == r"C:\Alpha"));
    }

    #[test]
    fn flatten_returns_none_when_cancelled() {
        let cancel = AtomicBool::new(true);
        let mut arena = Vec::new();
        let mut by_path = HashMap::new();
        let result = flatten(
            sample_root(),
            &mut arena,
            &mut by_path,
            u32::MAX,
            r"C:\",
            true,
            &cancel,
        );
        assert!(result.is_none());
    }

    #[test]
    fn raw_stats_counts_nodes_and_directories() {
        let (nodes, dirs) = raw_stats(&sample_root());
        // Root drive + two directories + four files.
        assert_eq!(nodes, 7);
        assert_eq!(dirs, 3);
    }

    #[test]
    #[ignore = "benchmark: cargo test -- --ignored --nocapture bench_flatten"]
    fn bench_flatten_one_million_nodes() {
        const DIRS: usize = 1_000;
        const FILES_PER_DIR: usize = 1_000;
        let children: Vec<RawNode> = (0..DIRS)
            .map(|dir_index| {
                let files: Vec<RawNode> = (0..FILES_PER_DIR)
                    .map(|file_index| {
                        let size = (file_index as u64 % 4096) + 1;
                        file(&format!("file{file_index}.bin"), size)
                    })
                    .collect();
                dir(
                    &format!("dir{dir_index}"),
                    FILES_PER_DIR as u64 * 2048,
                    FILES_PER_DIR as u32,
                    0,
                    files,
                )
            })
            .collect();
        let root = RawNode {
            name: "C:".to_string(),
            kind: NodeKind::Drive,
            size: DIRS as u64 * FILES_PER_DIR as u64 * 2048,
            file_count: (DIRS * FILES_PER_DIR) as u32,
            dir_count: DIRS as u32,
            is_hidden: false,
            skipped: false,
            error: None,
            children,
        };

        let started = Instant::now();
        let mut arena = Vec::new();
        let mut by_path = HashMap::new();
        let root_index =
            flatten(root, &mut arena, &mut by_path, u32::MAX, r"C:\", true, &AtomicBool::new(false))
                .expect("flatten");
        let elapsed = started.elapsed();
        println!(
            "flatten: {} nodes in {elapsed:?}, arena bytes={}, indexed dirs={}",
            arena.len(),
            std::mem::size_of::<NodeData>() * arena.len(),
            by_path.len()
        );
        let state = ScanState {
            arena,
            by_path,
            root: root_index,
            root_path: r"C:\".to_string().into_boxed_str(),
        };
        let lookup = Instant::now();
        for index in 0..1_000u32 {
            let _ = state.entry(index);
        }
        println!("1000 entry() lookups in {:?}", lookup.elapsed());
        assert_eq!(state.directory_view(state.root, None, None).total, DIRS as u32);
    }
}
