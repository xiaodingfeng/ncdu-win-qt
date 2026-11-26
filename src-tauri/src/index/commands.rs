//! Tauri commands for the global index.

use std::collections::BTreeSet;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};

use serde::Serialize;
use tauri::{AppHandle, Manager, State};
use windows_sys::Win32::Storage::FileSystem::{
    GetDiskFreeSpaceExW, GetDriveTypeW, GetLogicalDrives, GetVolumeInformationW,
};
use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
use windows_sys::Win32::UI::Shell::ShellExecuteW;

use crate::mft;
use crate::scan::ScanStore;

use super::builder;
use super::persist;
use super::query as search_query;
use super::store::{
    clear_status_override, default_volume, emit_progress, emit_status, normalize_volume,
    volume_root, CachedResponse, IndexStatus, IndexStore, VolumeSlot,
};
use super::watch;

const DRIVE_REMOVABLE: u32 = 2;
const DRIVE_FIXED: u32 = 3;

/// One local drive shown in the volume picker.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VolumeInfo {
    pub(crate) letter: String,
    pub(crate) label: String,
    pub(crate) ntfs: bool,
    pub(crate) fixed: bool,
    pub(crate) removable: bool,
    pub(crate) total_bytes: u64,
    pub(crate) free_bytes: u64,
    pub(crate) can_build: bool,
}

fn wide(value: &str) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    std::ffi::OsStr::new(value)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn volume_label(root: &[u16]) -> String {
    let mut label = [0u16; 64];
    let mut file_system = [0u16; 32];
    let ok = unsafe {
        GetVolumeInformationW(
            root.as_ptr(),
            label.as_mut_ptr(),
            label.len() as u32,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            file_system.as_mut_ptr(),
            file_system.len() as u32,
        )
    };
    if ok == 0 {
        return String::new();
    }
    let length = label.iter().position(|value| *value == 0).unwrap_or(0);
    String::from_utf16_lossy(&label[..length])
}

fn volume_space(root: &[u16]) -> (u64, u64) {
    let mut total = 0u64;
    let mut free = 0u64;
    let ok = unsafe {
        GetDiskFreeSpaceExW(root.as_ptr(), std::ptr::null_mut(), &mut total, &mut free)
    };
    if ok == 0 {
        (0, 0)
    } else {
        (total, free)
    }
}

/// Every local fixed or removable drive. Network and optical drives are
/// skipped: USN is unavailable there and indexing them is slow and unreliable.
fn local_volumes() -> Vec<VolumeInfo> {
    let mask = unsafe { GetLogicalDrives() };
    let mut volumes = Vec::new();
    for offset in 0..26u32 {
        if mask & (1 << offset) == 0 {
            continue;
        }
        let letter = (b'A' + offset as u8) as char;
        let root = format!("{letter}:\\");
        let wide_root = wide(&root);
        let kind = unsafe { GetDriveTypeW(wide_root.as_ptr()) };
        if kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE {
            continue;
        }
        let path = Path::new(&root);
        let ntfs = mft::is_ntfs_volume(path);
        let (total_bytes, free_bytes) = volume_space(&wide_root);
        volumes.push(VolumeInfo {
            letter: letter.to_string(),
            label: volume_label(&wide_root),
            ntfs,
            fixed: kind == DRIVE_FIXED,
            removable: kind == DRIVE_REMOVABLE,
            total_bytes,
            free_bytes,
            can_build: ntfs && mft::is_supported(path),
        });
    }
    volumes
}

/// Resolves the volumes a build applies to: an explicit list, then the
/// configured list, then every local volume.
pub(crate) fn target_volumes(app: &AppHandle, requested: Option<Vec<String>>) -> Vec<String> {
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut result: Vec<String> = Vec::new();
    let mut push = |value: &str, result: &mut Vec<String>| {
        if value.trim().is_empty() {
            return;
        }
        let letter = normalize_volume(value);
        if seen.insert(letter.clone()) {
            result.push(letter);
        }
    };

    if let Some(explicit) = requested {
        for value in &explicit {
            push(value, &mut result);
        }
        if !result.is_empty() {
            return result;
        }
    }

    let settings = crate::settings::load_or_init(app).unwrap_or_default();
    for value in &settings.index.volumes {
        push(value, &mut result);
    }
    if result.is_empty() {
        for info in local_volumes() {
            push(&info.letter, &mut result);
        }
    }
    if result.is_empty() {
        result.push(default_volume());
    }
    result
}

fn phase_name(phase: mft::MftPhase) -> &'static str {
    match phase {
        mft::MftPhase::Enumerating => "enumerating",
        mft::MftPhase::ReadingMft => "readingMft",
        mft::MftPhase::BuildingTree => "buildingTree",
    }
}

fn low_memory(budget: u64) -> bool {
    let mut status: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    status.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
    if unsafe { GlobalMemoryStatusEx(&mut status) } == 0 {
        return false;
    }
    status.ullAvailPhys < budget
}

/// A scan and an index build read the same disk, so the builder yields while a
/// scan is running. Queries are unaffected.
fn pause_for_scan(app: &AppHandle, cancel: &AtomicBool) {
    loop {
        if cancel.load(Ordering::Relaxed) {
            return;
        }
        if !app.state::<ScanStore>().is_scanning() {
            return;
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

/// Called once at startup: whoever opens the lock file with exclusive sharing
/// owns index writes; other windows become read-only.
pub(crate) fn prepare(app: &AppHandle) {
    let writable = persist::acquire_writer_lock(app);
    let store = app.state::<IndexStore>();
    if let Ok(mut state) = store.state.write() {
        state.writable = writable;
    };
}

fn set_volume_status(app: &AppHandle, volume: &str, status: &str, message: &str) {
    if let Ok(mut state) = app.state::<IndexStore>().state.write() {
        if let Some(slot) = state.volumes.get_mut(volume) {
            slot.status = status.to_string();
            slot.message = message.to_string();
        }
    }
    emit_status(app);
}

fn set_volume_message(app: &AppHandle, volume: &str, message: &str) {
    if let Ok(mut state) = app.state::<IndexStore>().state.write() {
        if let Some(slot) = state.volumes.get_mut(volume) {
            slot.message = message.to_string();
        }
    }
    emit_status(app);
}

/// Starts a build for one or more volumes. The previous index of each volume
/// stays queryable until the new one is installed.
pub(crate) fn spawn_build(app: AppHandle, volumes: Option<Vec<String>>) {
    let store = app.state::<IndexStore>();
    if !store.writable() {
        super::store::update_status(
            &app,
            "error",
            "another instance owns the index; this window is read-only",
        );
        return;
    }
    let targets = target_volumes(&app, volumes);
    if targets.is_empty() {
        super::store::update_status(&app, "error", "no local volumes to index");
        return;
    }
    store.stop_build();
    for volume in &targets {
        store.stop_watcher(volume);
    }
    store.clear_query_cache();

    let cancel = Arc::new(AtomicBool::new(false));
    if let Ok(mut guard) = store.cancel.lock() {
        *guard = Some(cancel.clone());
    }
    let generation = store.generation.fetch_add(1, Ordering::SeqCst) + 1;
    clear_status_override(&app);
    let realtime = crate::settings::load_or_init(&app)
        .map(|settings| settings.index.realtime)
        .unwrap_or(true);
    {
        let Ok(mut state) = store.state.write() else {
            return;
        };
        state.realtime = realtime;
        for volume in &targets {
            let slot = state
                .volumes
                .entry(volume.clone())
                .or_insert_with(|| VolumeSlot::new(volume));
            slot.status = "building".to_string();
            slot.message.clear();
            slot.elapsed_ms = 0;
            slot.generation = generation;
        }
    }
    emit_status(&app);

    let worker = app.clone();
    std::thread::spawn(move || {
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            run_build_all(worker.clone(), targets, generation, cancel);
        }));
        if outcome.is_err() {
            log::error!("[index] build thread panicked");
            super::store::update_status(&worker, "error", "the index build panicked");
        }
    });
}

fn run_build_all(app: AppHandle, targets: Vec<String>, generation: u64, cancel: Arc<AtomicBool>) {
    let settings = crate::settings::load_or_init(&app).unwrap_or_default();
    let budget = settings.index.memory_budget_mb.clamp(64, 8192) as u64 * 1024 * 1024;

    for volume in &targets {
        if cancel.load(Ordering::Relaxed) {
            break;
        }
        let used = app.state::<IndexStore>().status().memory_bytes;
        if used >= budget {
            super::store::update_status(
                &app,
                "error",
                "the index reached its memory budget; lower the entry cap or raise the budget to index the remaining volumes",
            );
            break;
        }
        run_build(
            app.clone(),
            volume.clone(),
            generation,
            cancel.clone(),
            &settings,
        );
    }

    let store = app.state::<IndexStore>();
    if store.generation.load(Ordering::SeqCst) == generation {
        if let Ok(mut state) = store.state.write() {
            for volume in &targets {
                if let Some(slot) = state.volumes.get_mut(volume) {
                    if slot.status == "building" {
                        slot.status = "idle".to_string();
                    }
                }
            }
        }
        if let Ok(mut guard) = store.cancel.lock() {
            guard.take();
        }
        emit_status(&app);
    }
}

fn run_build(
    app: AppHandle,
    volume: String,
    generation: u64,
    cancel: Arc<AtomicBool>,
    settings: &crate::settings::AppSettings,
) {
    let started = Instant::now();
    let root = PathBuf::from(volume_root(&volume));
    let progress_app = app.clone();
    let pause_app = app.clone();
    let budget = settings.index.memory_budget_mb.clamp(64, 8192) as u64 * 1024 * 1024;
    let low_memory_check = || low_memory(budget);
    let ntfs = mft::is_ntfs_volume(&root);
    let mft_ready = ntfs && mft::is_supported(&root);

    if !mft_ready {
        // Non-NTFS volumes, or NTFS without administrator rights: walk the
        // directory tree and keep the same index layout.
        let mut last_emit = Instant::now();
        let progress_volume = volume.clone();
        let mut on_count = |count: u64| {
            pause_for_scan(&pause_app, &cancel);
            if last_emit.elapsed() < Duration::from_millis(200) {
                return;
            }
            last_emit = Instant::now();
            emit_progress(&progress_app, &progress_volume, "walking", count);
        };
        match builder::walk_volume(
            &volume,
            &root,
            &cancel,
            &low_memory_check,
            &settings.index.excludes,
            settings.index.max_entries,
            &mut on_count,
        ) {
            Ok(index) => {
                install(&app, &volume, generation, started, index, false);
                if ntfs {
                    set_volume_message(
                        &app,
                        &volume,
                        "administrator rights were denied; only accessible folders are indexed",
                    );
                }
            }
            Err(builder::WalkError::Cancelled) => {
                set_volume_status(&app, &volume, "idle", "index build cancelled")
            }
            Err(builder::WalkError::LowMemory) => set_volume_status(&app, &volume, "error", "low memory"),
        }
        return;
    }

    let mut last_emit = Instant::now();
    let progress_volume = volume.clone();
    let mut on_progress = |phase: mft::MftPhase, count: u64| {
        pause_for_scan(&pause_app, &cancel);
        if last_emit.elapsed() < Duration::from_millis(200) {
            return;
        }
        last_emit = Instant::now();
        emit_progress(&progress_app, &progress_volume, phase_name(phase), count);
    };
    match mft::collect_volume_entries(&root, &cancel, &low_memory_check, &mut on_progress) {
        Ok(data) => {
            let index = builder::build(
                &volume,
                data,
                &settings.index.excludes,
                settings.index.max_entries,
            );
            install(
                &app,
                &volume,
                generation,
                started,
                index,
                settings.index.realtime,
            );
        }
        Err(mft::MftError::Cancelled) => {
            set_volume_status(&app, &volume, "idle", "index build cancelled")
        }
        Err(mft::MftError::Failed(message)) => {
            log::warn!("index build failed for {volume}: {message}");
            set_volume_status(&app, &volume, "error", &message);
        }
    }
}

/// Publishes a finished volume index, writes its snapshot and starts USN
/// tailing when the index came from a real journal.
fn install(
    app: &AppHandle,
    volume: &str,
    generation: u64,
    started: Instant,
    mut index: super::model::Index,
    realtime: bool,
) {
    let store = app.state::<IndexStore>();
    if store.generation.load(Ordering::SeqCst) != generation {
        return;
    }
    index.version = generation;
    let count = index.live_count();
    let memory = index.memory_bytes();
    let built_at = index.built_at;
    let journal_id = index.journal_id;
    let elapsed = started.elapsed().as_millis() as u64;
    let snapshot = persist::save(app, &index).is_ok();
    store.clear_query_cache();
    {
        let Ok(mut state) = store.state.write() else {
            return;
        };
        let slot = state
            .volumes
            .entry(volume.to_string())
            .or_insert_with(|| VolumeSlot::new(volume));
        slot.index = Some(Arc::new(RwLock::new(index)));
        slot.status = "ready".to_string();
        slot.message.clear();
        slot.built_at = built_at;
        slot.elapsed_ms = elapsed;
        slot.snapshot = snapshot;
        slot.generation = generation;
    }
    log::info!("index ready: volume {volume}, {count} entries, {memory} bytes, {elapsed} ms");
    emit_status(app);
    watch::start(app.clone(), volume.to_string(), realtime && journal_id != 0);
}

#[tauri::command]
pub fn index_status(store: State<'_, IndexStore>) -> IndexStatus {
    store.status()
}

#[tauri::command]
pub fn index_volumes() -> Vec<VolumeInfo> {
    local_volumes()
}

#[tauri::command]
pub fn index_build(app: AppHandle, volumes: Option<Vec<String>>) -> Result<(), String> {
    spawn_build(app, volumes);
    Ok(())
}

#[tauri::command]
pub fn index_cancel(store: State<'_, IndexStore>) -> Result<(), String> {
    store.stop_build();
    Ok(())
}

#[tauri::command]
pub fn index_clear(app: AppHandle) -> Result<(), String> {
    let store = app.state::<IndexStore>();
    if !store.writable() {
        return Err("another instance owns the index; this window is read-only".to_string());
    }
    store.stop_build();
    store.stop_watchers();
    store.clear_query_cache();
    let volumes: Vec<String> = {
        let state = store
            .state
            .read()
            .map_err(|_| "index state is unavailable".to_string())?;
        state.volumes.keys().cloned().collect()
    };
    for volume in &volumes {
        persist::remove(&app, volume);
    }
    {
        let mut state = store
            .state
            .write()
            .map_err(|_| "index state is unavailable".to_string())?;
        state.volumes.clear();
        state.status.clear();
        state.message.clear();
    }
    store.generation.fetch_add(1, Ordering::SeqCst);
    emit_status(&app);
    Ok(())
}

fn scope_letter(scope: Option<&String>) -> Option<String> {
    let text = scope?.trim();
    let letter = text.chars().next()?;
    if letter.is_ascii_alphabetic() {
        Some(letter.to_ascii_uppercase().to_string())
    } else {
        None
    }
}

#[tauri::command]
pub fn index_search(
    store: State<'_, IndexStore>,
    query: String,
    scope: Option<String>,
    sort: Option<String>,
    asc: Option<bool>,
    offset: Option<u32>,
    limit: Option<u32>,
    case_sensitive: Option<bool>,
) -> Result<search_query::SearchResponse, String> {
    let started = Instant::now();
    let request = search_query::SearchRequest {
        query,
        scope: scope.filter(|value| !value.trim().is_empty()),
        sort: sort.unwrap_or_else(|| "size".to_string()),
        asc: asc.unwrap_or(false),
        offset: offset.unwrap_or(0) as usize,
        limit: limit.unwrap_or(200) as usize,
        case_sensitive: case_sensitive.unwrap_or(false),
    };
    let key = format!(
        "{}\u{1}{:?}\u{1}{}\u{1}{}\u{1}{}\u{1}{}\u{1}{}",
        request.query,
        request.scope,
        request.sort,
        request.asc,
        request.offset,
        request.limit,
        request.case_sensitive
    );

    let handles = store.indexes();
    if handles.is_empty() {
        return Err("the index is not ready".to_string());
    }
    let letter = scope_letter(request.scope.as_ref());
    let selected: Vec<(String, Arc<RwLock<super::model::Index>>)> = handles
        .into_iter()
        .filter(|(volume, _)| match &letter {
            Some(letter) => volume.eq_ignore_ascii_case(letter),
            None => true,
        })
        .collect();

    let mut version = String::new();
    for (volume, handle) in &selected {
        if let Ok(index) = handle.read() {
            version.push_str(volume);
            version.push(':');
            version.push_str(&index.version.to_string());
            version.push(';');
        }
    }
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    version.hash(&mut hasher);
    let version_hash = hasher.finish();

    if let Ok(cache) = store.query_cache.lock() {
        if let Some(entry) = cache.as_ref() {
            if entry.key == key && entry.version == version {
                return Ok(entry.response.clone());
            }
        }
    }

    let per_volume_limit = (request.offset + request.limit).clamp(1, 1000);
    let mut items: Vec<search_query::SearchItem> = Vec::new();
    let mut total = 0usize;
    let mut error: Option<String> = None;
    for (_, handle) in &selected {
        let Ok(index) = handle.read() else {
            continue;
        };
        let mut volume_request = request.clone();
        volume_request.offset = 0;
        volume_request.limit = per_volume_limit;
        let response = search_query::search(&index, &volume_request);
        if let Some(message) = response.error {
            if error.is_none() {
                error = Some(message);
            }
        }
        total += response.total;
        items.extend(response.items);
    }

    if let Some(message) = error {
        return Ok(search_query::SearchResponse {
            items: Vec::new(),
            total: 0,
            took_ms: started.elapsed().as_millis() as u64,
            version: version_hash,
            has_more: false,
            error: Some(message),
        });
    }

    search_query::sort_items(&mut items, &request.sort, request.asc);
    let offset = request.offset.min(items.len());
    let end = (offset + request.limit).min(items.len());
    let page: Vec<search_query::SearchItem> = items.drain(offset..end).collect();
    let response = search_query::SearchResponse {
        has_more: end < total,
        items: page,
        total,
        took_ms: started.elapsed().as_millis() as u64,
        version: version_hash,
        error: None,
    };

    if let Ok(mut cache) = store.query_cache.lock() {
        *cache = Some(CachedResponse {
            key,
            version,
            response: response.clone(),
        });
    }
    Ok(response)
}

#[tauri::command]
pub fn restart_as_admin(app: AppHandle) -> Result<(), String> {
    let exe =
        std::env::current_exe().map_err(|err| format!("cannot locate the executable: {err}"))?;
    let operation = wide("runas");
    let file = wide(&exe.to_string_lossy());
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            operation.as_ptr(),
            file.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
        )
    };
    if result as isize <= 32 {
        return Err("elevation was cancelled".to_string());
    }
    app.exit(0);
    Ok(())
}
