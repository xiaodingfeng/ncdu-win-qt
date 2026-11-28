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

use crate::mft;
use crate::scan::ScanStore;

use super::builder;
use super::persist;
use super::query as search_query;
use super::store::{
    clear_status_override, default_volume, emit_progress, emit_status, lower_thread_priority,
    normalize_volume, volume_root, CachedResponse, IndexStatus, IndexStore, VolumeSlot,
};
use super::watch;

const DRIVE_REMOVABLE: u32 = 2;
const DRIVE_FIXED: u32 = 3;
/// Low-memory abort threshold is independent of the memory budget, which only
/// sizes the index.
const LOW_MEMORY_FLOOR: u64 = 256 * 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BuildMode {
    /// Keep the finished index in memory and start USN tailing.
    Resident,
    /// Write the snapshot, then drop the index (startup maintenance).
    PersistOnly,
}

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

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexDiagnosis {
    pub(crate) volume: String,
    pub(crate) volume_indexed: bool,
    pub(crate) volume_status: String,
    pub(crate) engine: String,
    pub(crate) entries: u64,
    pub(crate) truncated: u64,
    pub(crate) name_found: bool,
    pub(crate) indexed_path: Option<String>,
    pub(crate) matched: u64,
    pub(crate) hit_exclude: Option<String>,
    pub(crate) reparse_target: Option<String>,
    pub(crate) hint: String,
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

/// Cheap drive-letter list used by status polling (no volume probing).
pub(crate) fn local_letters() -> Vec<String> {
    let mask = unsafe { GetLogicalDrives() };
    let mut letters = Vec::new();
    for offset in 0..26u32 {
        if mask & (1 << offset) == 0 {
            continue;
        }
        let letter = (b'A' + offset as u8) as char;
        let wide_root = wide(&format!("{letter}:\\"));
        let kind = unsafe { GetDriveTypeW(wide_root.as_ptr()) };
        // Coverage warnings target fixed local disks; removable media is
        // optional and would otherwise nag every time it is plugged in.
        if kind == DRIVE_FIXED {
            letters.push(letter.to_string());
        }
    }
    letters
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

pub(crate) fn low_memory() -> bool {
    let mut status: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    status.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
    if unsafe { GlobalMemoryStatusEx(&mut status) } == 0 {
        return false;
    }
    status.ullAvailPhys < LOW_MEMORY_FLOOR
}

/// A scan and an index build read the same disk, so the builder yields while a
/// scan is running. Queries are unaffected.
pub(crate) fn pause_for_scan(app: &AppHandle, cancel: &AtomicBool) {
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

pub(crate) fn set_volume_status(app: &AppHandle, volume: &str, status: &str, message: &str) {
    if let Ok(mut state) = app.state::<IndexStore>().state.write() {
        if let Some(slot) = state.volumes.get_mut(volume) {
            slot.status = status.to_string();
            slot.message = message.to_string();
        }
    }
    emit_status(app);
}

pub(crate) fn set_volume_message(app: &AppHandle, volume: &str, message: &str) {
    if let Ok(mut state) = app.state::<IndexStore>().state.write() {
        if let Some(slot) = state.volumes.get_mut(volume) {
            slot.message = message.to_string();
        }
    }
    emit_status(app);
}

/// Starts a resident build for one or more volumes.
pub(crate) fn spawn_build(app: AppHandle, volumes: Option<Vec<String>>) {
    spawn_build_mode(app, volumes, BuildMode::Resident);
}

pub(crate) fn spawn_build_mode(app: AppHandle, volumes: Option<Vec<String>>, mode: BuildMode) {
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
        lower_thread_priority();
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            run_build_all(worker.clone(), targets, generation, cancel, mode);
        }));
        if outcome.is_err() {
            log::error!("[index] build thread panicked");
            super::store::update_status(&worker, "error", "the index build panicked");
        }
    });
}

fn run_build_all(
    app: AppHandle,
    targets: Vec<String>,
    generation: u64,
    cancel: Arc<AtomicBool>,
    mode: BuildMode,
) {
    let settings = crate::settings::load_or_init(&app).unwrap_or_default();
    let budget = settings.index.memory_budget_mb.clamp(64, 8192) as u64 * 1024 * 1024;
    // Budget only caps the entry count; a volume is never skipped wholesale.
    let per_volume_cap = (budget / 56).max(1).min(u32::MAX as u64) as u32;
    let cap = settings.index.max_entries.min(per_volume_cap).max(1);

    for volume in &targets {
        if cancel.load(Ordering::Relaxed) {
            break;
        }
        run_build(
            app.clone(),
            volume.clone(),
            generation,
            cancel.clone(),
            &settings,
            cap,
            mode,
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
    max_entries: u32,
    mode: BuildMode,
) {
    let started = Instant::now();
    let root = PathBuf::from(volume_root(&volume));
    let progress_app = app.clone();
    let pause_app = app.clone();
    let low_memory_check = || low_memory();
    let ntfs = mft::is_ntfs_volume(&root);
    let mft_ready = ntfs && mft::is_supported(&root);
    // Startup maintenance builds must not stream high-frequency progress; the
    // UI only needs the `building`/`stored` status transitions there.
    let emit = mode == BuildMode::Resident;

    if !mft_ready {
        // Non-NTFS volumes, or NTFS without administrator rights: walk the
        // directory tree and keep the same index layout.
        let mut last_emit = Instant::now();
        let progress_volume = volume.clone();
        let mut on_count = |count: u64| {
            pause_for_scan(&pause_app, &cancel);
            if !emit || last_emit.elapsed() < Duration::from_millis(200) {
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
            max_entries,
            &mut on_count,
        ) {
            Ok(outcome) => {
                let message = if ntfs {
                    "cannot read the raw volume; only accessible folders are indexed".to_string()
                } else {
                    "realtime updates unavailable (non-NTFS volume)".to_string()
                };
                install(&app, &volume, generation, started, outcome, mode, "walk", &message);
            }
            Err(builder::WalkError::Cancelled) => {
                set_volume_status(&app, &volume, "idle", "index build cancelled")
            }
            Err(builder::WalkError::LowMemory) => {
                set_volume_status(&app, &volume, "error", "low memory")
            }
        }
        return;
    }

    let mut last_emit = Instant::now();
    let progress_volume = volume.clone();
    let mut on_progress = |phase: mft::MftPhase, count: u64| {
        pause_for_scan(&pause_app, &cancel);
        if !emit || last_emit.elapsed() < Duration::from_millis(200) {
            return;
        }
        last_emit = Instant::now();
        emit_progress(&progress_app, &progress_volume, phase_name(phase), count);
    };
    match mft::collect_volume_entries(&root, &cancel, &low_memory_check, &mut on_progress) {
        Ok(data) => {
            let truncated_records = data.truncated_records;
            let outcome = builder::build(
                &volume,
                data,
                &settings.index.excludes,
                max_entries,
            );
            let message = if truncated_records {
                "the USN journal exceeded the record cap; some files were not indexed".to_string()
            } else {
                String::new()
            };
            install(
                &app,
                &volume,
                generation,
                started,
                outcome,
                mode,
                "mft",
                &message,
            );
        }
        Err(mft::MftError::Cancelled) => {
            set_volume_status(&app, &volume, "idle", "index build cancelled")
        }
        Err(mft::MftError::Failed(message)) => {
            log::warn!("index build failed for {volume}: {message}");
            set_volume_status(&app, &volume, "error", &message);
        }
        Err(mft::MftError::JournalReset) => {
            set_volume_status(&app, &volume, "error", "USN journal was reset while reading")
        }
    }
}

/// Publishes a finished volume index. `PersistOnly` writes the snapshot and
/// drops the in-memory copy; `Resident` keeps it and starts USN tailing.
fn install(
    app: &AppHandle,
    volume: &str,
    generation: u64,
    started: Instant,
    outcome: builder::BuildOutcome,
    mode: BuildMode,
    engine: &str,
    message: &str,
) {
    let store = app.state::<IndexStore>();
    if store.generation.load(Ordering::SeqCst) != generation {
        return;
    }
    let mut index = outcome.index;
    index.version = generation;
    index.truncated_entries = index.truncated_entries.max(outcome.truncated);
    let count = index.live_count();
    let memory = index.memory_bytes();
    let built_at = index.built_at;
    let journal_id = index.journal_id;
    let truncated = index.truncated_entries;
    let elapsed = started.elapsed().as_millis() as u64;
    let snapshot = persist::save(app, &index).is_ok();
    // A completed build supersedes any pending delta log.
    persist::reset_delta(app, volume);
    store.clear_query_cache();
    let realtime = crate::settings::load_or_init(app)
        .map(|settings| settings.index.realtime)
        .unwrap_or(true);
    let mut final_message = message.to_string();
    if truncated > 0 {
        let note = format!("reached the entry cap; {truncated} files were not indexed");
        final_message = if final_message.is_empty() {
            note
        } else {
            format!("{final_message}; {note}")
        };
    }
    if !outcome.excluded.is_empty() {
        let detail = outcome
            .excluded
            .iter()
            .map(|(rule, hits)| format!("{rule}({hits})"))
            .collect::<Vec<_>>()
            .join(" ");
        let note = format!("excludes: {detail}");
        final_message = if final_message.is_empty() {
            note
        } else {
            format!("{final_message}; {note}")
        };
    }
    let mut resident = mode == BuildMode::Resident;
    {
        let Ok(mut state) = store.state.write() else {
            return;
        };
        let slot = state
            .volumes
            .entry(volume.to_string())
            .or_insert_with(|| VolumeSlot::new(volume));
        if slot.load_requested {
            resident = true;
        }
        slot.truncated = truncated;
        slot.built_at = built_at;
        slot.elapsed_ms = elapsed;
        slot.snapshot = snapshot;
        slot.generation = generation;
        slot.engine = engine.to_string();
        slot.stored_count = count;
        if resident {
            slot.index = Some(Arc::new(RwLock::new(index)));
            slot.status = "ready".to_string();
            slot.loaded = true;
            slot.message = final_message.clone();
        } else {
            slot.index = None;
            slot.status = "stored".to_string();
            slot.loaded = false;
            slot.message = final_message.clone();
        }
    }
    log::info!(
        "index ready: volume {volume}, engine={engine}, entries={count}, truncated={truncated}, excludesHit={}, elapsed={elapsed} ms",
        outcome.excluded.iter().map(|(_, hits)| *hits).sum::<u64>()
    );
    emit_status(app);
    if resident {
        if realtime && journal_id != 0 {
            watch::start(app.clone(), volume.to_string(), true);
        } else if realtime && journal_id == 0 {
            set_volume_message(app, volume, "realtime updates unavailable");
        }
        log::info!(
            "index resident: volume {volume}, {count} entries, {memory} bytes, {elapsed} ms"
        );
    }
}

/// Loads snapshots the first time the search dialog opens and keeps them in
/// memory for the rest of the session.
pub(crate) fn ensure_loaded(app: AppHandle) {
    let settings = crate::settings::load_or_init(&app).unwrap_or_default();
    if !settings.index.enabled {
        super::store::update_status(&app, "disabled", "index is disabled in settings");
        return;
    }
    let targets = target_volumes(&app, None);
    let store = app.state::<IndexStore>();
    let generation = store.generation.load(Ordering::SeqCst);
    let mut pending: Vec<String> = Vec::new();
    {
        let Ok(mut state) = store.state.write() else {
            return;
        };
        state.realtime = settings.index.realtime;
        for volume in &targets {
            let slot = state
                .volumes
                .entry(volume.clone())
                .or_insert_with(|| VolumeSlot::new(volume));
            if slot.index.is_some() {
                continue;
            }
            // A startup build/sync should become resident once it finishes.
            slot.load_requested = true;
            if slot.status == "building" || slot.status == "syncing" || slot.status == "loading" {
                continue;
            }
            slot.status = "loading".to_string();
            slot.message.clear();
            pending.push(volume.clone());
        }
    };
    if pending.is_empty() {
        emit_status(&app);
        return;
    }
    clear_status_override(&app);
    emit_status(&app);

    let worker = app.clone();
    std::thread::spawn(move || {
        lower_thread_priority();
        let mut missing: Vec<String> = Vec::new();
        let mut stale: Vec<String> = Vec::new();
        for volume in &pending {
            if worker.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
                return;
            }
            match persist::load(&worker, volume) {
                Ok(mut index) => {
                    let built_at = index.built_at;
                    let journal_id = index.journal_id;
                    let volume_serial = index.volume_serial;
                    // The delta log may already cover more than the snapshot file
                    // itself; expire only when even the delta tail is unreachable.
                    let effective = persist::effective_next_usn(
                        &worker,
                        volume,
                        volume_serial,
                        journal_id,
                        index.version,
                        index.next_usn,
                    );
                    let expired = is_snapshot_expired(volume, journal_id, effective);
                    // A stale snapshot must not receive a delta from a different
                    // journal generation; it is rebuilt instead.
                    if !expired {
                        replay_delta(&worker, volume, &mut index);
                    }
                    let delta_size = if expired {
                        0
                    } else {
                        persist::read_delta_state(&worker, volume)
                            .map(|state| state.delta_len)
                            .unwrap_or(0)
                    };
                    let count = index.live_count();
                    let truncated = index.truncated_entries;
                    let engine = if journal_id != 0 { "mft" } else { "walk" };
                    // A build started while decoding owns the slot now.
                    if worker.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
                        return;
                    }
                    {
                        let store = worker.state::<IndexStore>();
                        let Ok(mut state) = store.state.write() else {
                            return;
                        };
                        let Some(slot) = state.volumes.get_mut(volume) else {
                            continue;
                        };
                        slot.index = Some(Arc::new(RwLock::new(index)));
                        slot.status = if expired { "building" } else { "ready" }.to_string();
                        slot.message.clear();
                        slot.built_at = built_at;
                        slot.elapsed_ms = 0;
                        slot.snapshot = true;
                        slot.loaded = true;
                        slot.stored_count = count;
                        slot.truncated = truncated;
                        slot.engine = engine.to_string();
                        slot.generation = generation;
                    }
                    log::info!("index snapshot loaded for volume {volume}");
                    worker.state::<IndexStore>().clear_query_cache();
                    if expired {
                        stale.push(volume.clone());
                        continue;
                    }
                    if settings.index.realtime
                        && journal_id != 0
                        && worker.state::<IndexStore>().writable()
                    {
                        watch::start(worker.clone(), volume.clone(), true);
                    }
                    schedule_delta_merge(&worker, volume, volume_serial, delta_size);
                }
                Err(err) => {
                    log::info!("no usable index snapshot for volume {volume}: {err}");
                    missing.push(volume.clone());
                }
            }
        }
        if worker.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
            return;
        }
        if !stale.is_empty() && worker.state::<IndexStore>().writable() {
            log::warn!("rebuilding expired index snapshots: {stale:?}");
            spawn_build_mode(worker.clone(), Some(stale), BuildMode::Resident);
            return;
        }
        if missing.is_empty() {
            return;
        }
        if worker.state::<IndexStore>().writable() {
            spawn_build(worker.clone(), Some(missing));
        } else {
            for volume in &missing {
                set_volume_status(
                    &worker,
                    volume,
                    "error",
                    "another instance owns the index; this window is read-only",
                );
            }
        }
    });
}

/// True when the snapshot can no longer be caught up from the USN journal.
fn is_snapshot_expired(volume: &str, journal_id: u64, next_usn: i64) -> bool {
    if journal_id == 0 {
        // Walk indexes have no journal; they are rebuilt explicitly.
        return false;
    }
    match mft::journal_for_volume(volume) {
        Ok(journal) => {
            let lower = journal.first_usn.max(journal.lowest_valid_usn);
            journal.journal_id != journal_id
                || next_usn < lower
                || next_usn > journal.next_usn
        }
        // Non-elevated processes cannot query the journal; leave it to the
        // watcher, which retries once it has raw access.
        Err(_) => false,
    }
}

/// Replays the committed delta log into a freshly loaded snapshot.
fn replay_delta(app: &AppHandle, volume: &str, index: &mut super::model::Index) {
    let Some(state) = persist::read_delta_state(app, volume) else {
        return;
    };
    if state.volume_serial != index.volume_serial
        || state.journal_id != index.journal_id
        || state.base_index_version != index.version
    {
        persist::reset_delta(app, volume);
        return;
    }
    let records = persist::read_delta(app, volume, &state);
    if records.is_empty() {
        index.next_usn = state.tail_usn;
        return;
    }
    let record_count = apply_delta_records(index, records, state.tail_usn);
    log::info!("index delta replayed for volume {volume}: {record_count} records");
}

/// Applies a delta range to a loaded index with the same semantics as the live
/// watcher: a create after a delete wins, deletions are propagated once.
fn apply_delta_records(
    index: &mut super::model::Index,
    records: Vec<persist::DeltaRecord>,
    tail_usn: i64,
) -> usize {
    let count = records.len();
    let mut deleted: std::collections::HashMap<u64, u32> = std::collections::HashMap::new();
    for record in records {
        if record.kind == persist::DeltaKind::Delete {
            if let Some(idx) = index.lookup_ref(record.file_ref) {
                deleted.insert(record.file_ref, idx);
            }
            continue;
        }
        // A create after a delete in the same range must win.
        deleted.remove(&record.file_ref);
        let parent = if record.parent_ref == super::model::VOLUME_ROOT_REF {
            0
        } else {
            index.lookup_ref(record.parent_ref).unwrap_or(0)
        };
        let attributes = (if record.is_dir { 0x10u32 } else { 0 })
            | (if record.hidden { 0x2u32 } else { 0 });
        index.upsert(
            record.file_ref,
            parent,
            &record.name,
            attributes,
            record.size,
            record.mtime,
        );
    }
    if !deleted.is_empty() {
        let roots: Vec<u32> = deleted.into_values().collect();
        index.mark_many_deleted(&roots);
    }
    index.next_usn = tail_usn;
    count
}

/// Threshold above which the delta log is folded into the snapshot.
fn delta_merge_threshold(app: &AppHandle, volume: &str) -> u64 {
    let base_size = persist::read_header(app, volume)
        .map(|header| header.count as u64 * 33)
        .unwrap_or(0);
    (base_size / 10).max(8 * 1024 * 1024)
}

/// Folds the committed delta log into the snapshot when it grows past the merge
/// threshold. Startup maintenance calls this so the snapshot's `next_usn` does
/// not stay frozen and the delta never grows without bound.
pub(crate) fn maybe_compact_delta(app: &AppHandle, volume: &str, generation: u64) {
    if app.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
        return;
    }
    let Some(before) = persist::read_delta_state(app, volume) else {
        return;
    };
    if before.delta_len <= delta_merge_threshold(app, volume) {
        return;
    }
    let Ok(mut index) = persist::load(app, volume) else {
        return;
    };
    replay_delta(app, volume, &mut index);
    // Another writer may have reset or replaced the delta while we decoded;
    // never overwrite a snapshot that moved on.
    match persist::read_delta_state(app, volume) {
        Some(after)
            if after.tail_usn == before.tail_usn && after.delta_len == before.delta_len => {}
        _ => return,
    }
    if app.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
        return;
    }
    if persist::save(app, &index).is_ok() {
        persist::reset_delta(app, volume);
        log::info!("index startup: compacted delta for volume {volume}");
    }
}

/// A delta larger than 10% of the base (or 8MB) is folded into a new snapshot
/// in the background, never on the first-open critical path.
fn schedule_delta_merge(app: &AppHandle, volume: &str, volume_serial: u64, delta_size: u64) {
    if delta_size == 0 || delta_size <= delta_merge_threshold(app, volume) {
        return;
    }
    let worker = app.clone();
    let volume = volume.to_string();
    std::thread::spawn(move || {
        lower_thread_priority();
        std::thread::sleep(Duration::from_secs(2));
        let Some(handle) = worker.state::<IndexStore>().index_for(&volume) else {
            return;
        };
        let Ok(index) = handle.read() else {
            return;
        };
        if index.volume_serial != volume_serial {
            return;
        }
        if persist::save(&worker, &index).is_ok() {
            persist::reset_delta(&worker, &volume);
            log::info!("index delta merged for volume {volume}");
        }
    });
}

#[tauri::command]
pub fn index_ensure_loaded(app: AppHandle) -> Result<(), String> {
    ensure_loaded(app);
    Ok(())
}

#[tauri::command]
pub async fn index_status(app: AppHandle) -> IndexStatus {
    app.state::<IndexStore>().status()
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

fn empty_response(version: u64, started: Instant) -> search_query::SearchResponse {
    search_query::SearchResponse {
        items: Vec::new(),
        total: 0,
        took_ms: started.elapsed().as_millis() as u64,
        version,
        has_more: false,
        error: None,
    }
}

fn run_search(
    app: AppHandle,
    request: search_query::SearchRequest,
) -> Result<search_query::SearchResponse, String> {
    let started = Instant::now();
    let store = app.state::<IndexStore>();
    let (sequence, token) = store.begin_search();
    if !store.is_current_search(sequence) {
        return Ok(empty_response(0, started));
    }
    // Wait for the previous scan to notice its cancellation and exit.
    let _gate = store
        .search_gate
        .lock()
        .unwrap_or_else(|err| err.into_inner());
    if !store.is_current_search(sequence) || token.load(Ordering::Relaxed) {
        return Ok(empty_response(0, started));
    }

    let handles = store.indexes();
    let letter = scope_letter(request.scope.as_ref());
    let selected: Vec<(String, Arc<RwLock<super::model::Index>>)> = handles
        .into_iter()
        .filter(|(volume, _)| match &letter {
            Some(letter) => volume.eq_ignore_ascii_case(letter),
            None => true,
        })
        .collect();
    if selected.is_empty() {
        return Err("the index is not ready".to_string());
    }

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
        let response = search_query::search_with_cancel(&index, &volume_request, Some(&token));
        if token.load(Ordering::Relaxed) || !store.is_current_search(sequence) {
            return Ok(empty_response(0, started));
        }
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
        has_more: offset + page.len() < total,
        items: page,
        total,
        took_ms: started.elapsed().as_millis() as u64,
        version: version_hash,
        error: None,
    };

    if !token.load(Ordering::Relaxed) {
        if let Ok(mut cache) = store.query_cache.lock() {
            *cache = Some(CachedResponse {
                key,
                version,
                response: response.clone(),
            });
        }
    }
    Ok(response)
}

#[tauri::command]
pub async fn index_search(
    app: AppHandle,
    query: String,
    scope: Option<String>,
    sort: Option<String>,
    asc: Option<bool>,
    offset: Option<u32>,
    limit: Option<u32>,
    case_sensitive: Option<bool>,
) -> Result<search_query::SearchResponse, String> {
    let request = search_query::SearchRequest {
        query,
        scope: scope.filter(|value| !value.trim().is_empty()),
        sort: sort.unwrap_or_else(|| "size".to_string()),
        asc: asc.unwrap_or(false),
        offset: offset.unwrap_or(0) as usize,
        limit: limit.unwrap_or(200) as usize,
        case_sensitive: case_sensitive.unwrap_or(false),
    };
    tauri::async_runtime::spawn_blocking(move || run_search(app, request))
        .await
        .map_err(|err| format!("search task failed: {err}"))?
}

#[tauri::command]
pub fn index_diagnose(app: AppHandle, path: String) -> Result<IndexDiagnosis, String> {
    let target = PathBuf::from(&path);
    let name = target
        .file_name()
        .map(|value| value.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.clone());
    let drive_supported = path
        .chars()
        .next()
        .map(|value| value.is_ascii_alphabetic())
        .unwrap_or(false)
        && path.as_bytes().get(1) == Some(&b':');
    let letter = scope_letter(Some(&path)).unwrap_or_else(default_volume);
    let settings = crate::settings::load_or_init(&app).unwrap_or_default();
    let store = app.state::<IndexStore>();
    let state = store
        .state
        .read()
        .map_err(|_| "index state is unavailable".to_string())?;
    let slot = state.volumes.get(&letter);
    let volume_indexed = slot.is_some();
    let volume_status = slot
        .map(|slot| slot.status.clone())
        .unwrap_or_else(|| "missing".to_string());
    let stored_unloaded = slot
        .map(|slot| !slot.loaded && slot.status == "stored")
        .unwrap_or(false);
    let mut engine = slot.map(|slot| slot.engine.clone()).unwrap_or_default();
    let mut entries = 0u64;
    let mut truncated = 0u64;
    let mut name_found = false;
    let mut indexed_path = None;
    let mut matched = 0u64;
    if let Some(handle) = slot.and_then(|slot| slot.index.clone()) {
        if let Ok(index) = handle.read() {
            engine = if index.journal_id != 0 { "mft" } else { "walk" }.to_string();
            entries = index.live_count();
            truncated = index.truncated_entries;
            for idx in 1..index.len() as u32 {
                if !index.is_live(idx) {
                    continue;
                }
                if index.name(idx).eq_ignore_ascii_case(&name) {
                    matched += 1;
                    if indexed_path.is_none() {
                        let built = index.path_of(idx);
                        if built.eq_ignore_ascii_case(&path) {
                            indexed_path = Some(built);
                            name_found = true;
                        } else if indexed_path.is_none() {
                            indexed_path = Some(built);
                            name_found = true;
                        }
                    }
                }
            }
        }
    }
    drop(state);

    let hit_exclude = settings
        .index
        .excludes
        .iter()
        .find(|rule| {
            let rule = rule.trim().to_ascii_lowercase();
            !rule.is_empty() && name.to_lowercase().contains(&rule)
        })
        .cloned();
    let reparse_target = std::fs::symlink_metadata(&target)
        .ok()
        .filter(|metadata| {
            use std::os::windows::fs::MetadataExt;
            metadata.file_attributes() & 0x400 != 0
        })
        .map(|_| path.clone());

    let hint = diagnose_hint(
        drive_supported,
        volume_indexed,
        stored_unloaded,
        name_found,
        hit_exclude.is_some(),
        truncated,
        reparse_target.is_some(),
        &letter,
    );

    Ok(IndexDiagnosis {
        volume: letter,
        volume_indexed,
        volume_status,
        engine,
        entries,
        truncated,
        name_found,
        indexed_path,
        matched,
        hit_exclude,
        reparse_target,
        hint,
    })
}

/// Pure decision table behind `index_diagnose`, kept separate so it can be
/// unit-tested without an app handle or a real volume.
#[allow(clippy::too_many_arguments)]
fn diagnose_hint(
    drive_supported: bool,
    volume_indexed: bool,
    stored_unloaded: bool,
    name_found: bool,
    hit_exclude: bool,
    truncated: u64,
    reparse: bool,
    letter: &str,
) -> String {
    if !drive_supported {
        "network, mounted and UNC paths are not indexed; only local drives are supported".to_string()
    } else if !volume_indexed {
        format!("volume {letter}: is not indexed; add it in the search settings")
    } else if !name_found && stored_unloaded {
        "the index is built but not loaded; open the search panel to load it".to_string()
    } else if hit_exclude {
        "the file name matches an exclude rule".to_string()
    } else if truncated > 0 {
        format!("the index hit its entry cap; {truncated} files were not indexed")
    } else if reparse {
        "the path is a reparse point (junction/symlink) that is not indexed".to_string()
    } else if name_found {
        "the file is present in the index".to_string()
    } else {
        "the file is missing from the index; rebuild or raise the entry cap".to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::index::model::Index;
    use crate::index::persist::{DeltaKind, DeltaRecord};

    fn record(kind: DeltaKind, file_ref: u64, name: &str) -> DeltaRecord {
        DeltaRecord {
            kind,
            hidden: false,
            is_dir: false,
            file_ref,
            parent_ref: crate::index::model::VOLUME_ROOT_REF,
            size: 10,
            mtime: 5,
            name: name.to_string(),
        }
    }

    #[test]
    fn delta_replay_applies_creates_and_deletes() {
        let mut index = Index::new("C");
        index.version = 3;
        index.volume_serial = 9;
        index.journal_id = 7;
        index.push_entry(11, 0, "gone.txt", 0, 2, 0);
        index.rebuild_refs();

        let records = vec![
            record(DeltaKind::Upsert, 12, "new.txt"),
            record(DeltaKind::Delete, 11, "gone.txt"),
        ];
        let count = apply_delta_records(&mut index, records, 42);

        assert_eq!(count, 2);
        assert_eq!(index.next_usn, 42);
        let created = index.lookup_ref(12).expect("created");
        assert_eq!(index.path_of(created), "C:\\new.txt");
        let removed = index.lookup_ref(11).expect("slot kept");
        assert!(!index.is_live(removed));
    }

    #[test]
    fn delta_replay_keeps_a_recreated_entry_live() {
        let mut index = Index::new("C");
        index.push_entry(11, 0, "gone.txt", 0, 2, 0);
        index.rebuild_refs();
        let records = vec![
            record(DeltaKind::Delete, 11, "gone.txt"),
            record(DeltaKind::Upsert, 11, "back.txt"),
        ];
        apply_delta_records(&mut index, records, 50);
        let idx = index.lookup_ref(11).expect("slot");
        assert!(index.is_live(idx));
        assert_eq!(index.name(idx), "back.txt");
    }

    #[test]
    fn diagnose_reports_missing_volume_exclude_truncation_and_hit() {
        // Missing volume.
        assert!(diagnose_hint(true, false, false, false, false, 0, false, "E")
            .contains("not indexed"));
        // Excluded by a rule.
        assert!(diagnose_hint(true, true, false, false, true, 0, false, "C")
            .contains("exclude"));
        // Entry cap reached.
        assert!(diagnose_hint(true, true, false, false, false, 1234, false, "C")
            .contains("1234"));
        // Present in the index.
        assert!(diagnose_hint(true, true, false, true, false, 0, false, "C")
            .contains("present"));
        // Unsupported network/UNC path.
        assert!(diagnose_hint(false, false, false, false, false, 0, false, "C")
            .contains("network"));
        // Built but not loaded.
        assert!(diagnose_hint(true, true, true, false, false, 0, false, "C")
            .contains("not loaded"));
    }
}
