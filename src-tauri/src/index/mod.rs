//! Global file-name index (Everything-style): a compact in-memory snapshot,
//! optional persistence, and USN-based incremental updates. It is fully
//! decoupled from the scan subsystem; deleting or disabling the index never
//! affects scanning, cleanup, migration or AI features.

pub(crate) mod builder;
pub(crate) mod commands;
pub(crate) mod model;
pub(crate) mod persist;
pub(crate) mod query;
pub(crate) mod store;
pub(crate) mod watch;

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use tauri::{AppHandle, Manager};

use store::{IndexStore, VolumeSlot};

const STARTUP_DELAY: Duration = Duration::from_secs(10);
const MAX_SYNC_BATCHES: usize = 8;
const SYNC_METADATA_BUDGET: usize = 512;

/// Startup maintenance: every configured volume is checked with a 256-byte
/// header read. Missing snapshots are built in the background and written to
/// disk only (`PersistOnly`); valid snapshots have their USN delta appended to
/// the delta log and committed, also without decoding the base index. Nothing
/// stays resident until the search panel is opened.
pub(crate) fn startup(app: AppHandle) {
    std::thread::spawn(move || {
        store::lower_thread_priority();
        std::thread::sleep(STARTUP_DELAY);
        let settings = match crate::settings::load_or_init(&app) {
            Ok(settings) => settings,
            Err(err) => {
                log::warn!("index startup skipped: {err}");
                return;
            }
        };
        if !settings.index.enabled {
            store::update_status(&app, "disabled", "index is disabled in settings");
            return;
        }
        if !app.state::<IndexStore>().writable() {
            store::update_status(
                &app,
                "error",
                "another instance owns the index; this window is read-only",
            );
            return;
        }
        let targets = commands::target_volumes(&app, None);
        {
            let store = app.state::<IndexStore>();
            if let Ok(mut state) = store.state.write() {
                state.realtime = settings.index.realtime;
                for volume in &targets {
                    state
                        .volumes
                        .entry(volume.clone())
                        .or_insert_with(|| VolumeSlot::new(volume));
                }
            };
        }
        store::emit_status(&app);
        log::info!("index startup maintenance checking {targets:?}");

        let generation = app.state::<IndexStore>().generation.load(Ordering::SeqCst);
        let cancel = AtomicBool::new(false);
        let mut to_build: Vec<String> = Vec::new();
        for volume in &targets {
            if app.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
                log::info!("index startup: superseded by a newer build; stopping maintenance");
                return;
            }
            commands::pause_for_scan(&app, &cancel);
            if volume_already_active(&app, volume) {
                continue;
            }
            let Some(header) = persist::read_header(&app, volume) else {
                log::info!("index startup: volume {volume} has no usable snapshot");
                to_build.push(volume.clone());
                continue;
            };
            if !header.volume.eq_ignore_ascii_case(volume) {
                log::info!(
                    "index startup: snapshot {} does not belong to volume {volume}; rebuilding",
                    header.volume
                );
                to_build.push(volume.clone());
                continue;
            }
            if mark_stored(&app, volume, &header) {
                // The user already opened search: let the loader take over
                // (its watcher catches up from the saved USN position).
                commands::ensure_loaded(app.clone());
                continue;
            }
            let journal = match crate::mft::journal_for_volume(volume) {
                Ok(journal) => journal,
                Err(crate::mft::MftError::JournalReset) => {
                    log::info!("index startup: volume {volume} journal was reset; rebuilding");
                    to_build.push(volume.clone());
                    continue;
                }
                Err(err) => {
                    log::info!(
                        "index startup: cannot query the journal of {volume} ({err:?}); leaving it stored"
                    );
                    continue;
                }
            };
            match maintenance_plan(&header, &journal) {
                Maintenance::Rebuild => {
                    log::info!("index startup: volume {volume} snapshot is stale; rebuilding");
                    to_build.push(volume.clone());
                    continue;
                }
                Maintenance::UpToDate => continue,
                Maintenance::Sync => {}
            }
            if commands::low_memory() {
                // Leave the tail for the watcher so a low-memory machine is
                // never pushed into a large sync during startup.
                log::info!(
                    "index startup: low memory; deferring delta sync for {volume} to the watcher"
                );
                continue;
            }
            if let Err(err) = sync_delta(
                &app,
                volume,
                &header,
                &journal,
                &cancel,
                generation,
            ) {
                log::warn!("index startup: delta sync failed for {volume}: {err:?}");
                to_build.push(volume.clone());
            }
        }

        if !to_build.is_empty()
            && app.state::<IndexStore>().generation.load(Ordering::SeqCst) == generation
        {
            log::info!("index startup: building {to_build:?} in the background");
            commands::spawn_build_mode(app.clone(), Some(to_build), commands::BuildMode::PersistOnly);
        }
    });
}

#[derive(Debug, PartialEq, Eq)]
enum Maintenance {
    Rebuild,
    UpToDate,
    Sync,
}

/// Decides what startup maintenance must do for one volume. `UpToDate` means
/// "no disk writes at all" (the tail already equals the journal position).
fn maintenance_plan(
    header: &persist::IndexHeader,
    journal: &crate::mft::JournalSnapshot,
) -> Maintenance {
    let lower = journal.first_usn.max(journal.lowest_valid_usn);
    if journal.journal_id != header.journal_id
        || header.next_usn < lower
        || header.next_usn > journal.next_usn
    {
        Maintenance::Rebuild
    } else if header.next_usn == journal.next_usn {
        Maintenance::UpToDate
    } else {
        Maintenance::Sync
    }
}

fn volume_already_active(app: &AppHandle, volume: &str) -> bool {
    let store = app.state::<IndexStore>();
    let Ok(state) = store.state.read() else {
        return true;
    };
    match state.volumes.get(volume) {
        Some(slot) => {
            slot.index.is_some()
                || matches!(
                    slot.status.as_str(),
                    "building" | "syncing" | "loading" | "ready"
                )
        }
        None => false,
    }
}

/// Records snapshot metadata without loading it. Returns true when the search
/// panel asked for this volume while maintenance was running.
fn mark_stored(app: &AppHandle, volume: &str, header: &persist::IndexHeader) -> bool {
    let requested = {
        let store = app.state::<IndexStore>();
        let Ok(mut state) = store.state.write() else {
            return false;
        };
        let slot = state
            .volumes
            .entry(volume.to_string())
            .or_insert_with(|| VolumeSlot::new(volume));
        slot.status = "stored".to_string();
        slot.message.clear();
        slot.built_at = header.built_at;
        slot.stored_count = header.live_count as u64;
        slot.truncated = header.truncated;
        slot.snapshot = true;
        slot.loaded = false;
        slot.engine = header.engine().to_string();
        slot.load_requested
    };
    store::emit_status(app);
    requested
}

/// Appends the USN range `header.next_usn .. journal.next_usn` to the delta log
/// without decoding the base index. Metadata is read for root-level entries
/// only (bounded by a budget); everything else is recorded as size 0.
fn sync_delta(
    app: &AppHandle,
    volume: &str,
    header: &persist::IndexHeader,
    journal: &crate::mft::JournalSnapshot,
    cancel: &AtomicBool,
    generation: u64,
) -> Result<(), crate::mft::MftError> {
    set_syncing(app, volume);
    let letter = volume.chars().next().unwrap_or('C');
    let handle = crate::mft::open_volume(letter)?;
    let mut cursor = header.next_usn;
    let mut records: Vec<persist::DeltaRecord> = Vec::new();
    let mut metadata_budget = SYNC_METADATA_BUDGET;
    let root_prefix = format!("{letter}:\\");
    for _ in 0..MAX_SYNC_BATCHES {
        if cancel.load(Ordering::Relaxed) {
            return Ok(());
        }
        let (changes, next) =
            crate::mft::read_usn_journal(&handle, journal.journal_id, cursor)?;
        for change in changes {
            let delete = change.reason & 0x0000_0200 != 0;
            let is_dir = change.attributes & 0x10 != 0;
            let hidden = change.attributes & 0x2 != 0
                || change.name.starts_with('.')
                || change.name.starts_with('$');
            let (size, mtime) = if !delete
                && !is_dir
                && change.parent_ref == model::VOLUME_ROOT_REF
                && metadata_budget > 0
            {
                metadata_budget -= 1;
                std::fs::metadata(format!("{root_prefix}{}", change.name))
                    .map(|metadata| {
                        let mtime = metadata
                            .modified()
                            .ok()
                            .and_then(|value| value.duration_since(std::time::UNIX_EPOCH).ok())
                            .map(|value| value.as_secs() as i64)
                            .unwrap_or(0);
                        (metadata.len(), mtime)
                    })
                    .unwrap_or((0, 0))
            } else {
                (0, 0)
            };
            records.push(persist::DeltaRecord {
                kind: if delete {
                    persist::DeltaKind::Delete
                } else {
                    persist::DeltaKind::Upsert
                },
                hidden,
                is_dir,
                file_ref: change.file_ref,
                parent_ref: change.parent_ref,
                size,
                mtime,
                name: change.name,
            });
        }
        if next == cursor {
            break;
        }
        cursor = next;
        if cursor >= journal.next_usn {
            break;
        }
    }
    if app.state::<IndexStore>().generation.load(Ordering::SeqCst) != generation {
        // A user build started while syncing; its snapshot supersedes this log.
        return Ok(());
    }
    let count = records.len();
    persist::append_delta(
        app,
        volume,
        header.volume_serial,
        header.journal_id,
        header.index_version,
        &records,
        cursor,
    )
    .map_err(crate::mft::MftError::Failed)?;
    if count > 0 {
        log::info!("index startup: synced {count} changes for volume {volume}");
    }
    if mark_stored(app, volume, header) {
        commands::ensure_loaded(app.clone());
    }
    Ok(())
}

fn set_syncing(app: &AppHandle, volume: &str) {
    {
        let store = app.state::<IndexStore>();
        if let Ok(mut state) = store.state.write() {
            if let Some(slot) = state.volumes.get_mut(volume) {
                slot.status = "syncing".to_string();
                slot.message.clear();
            }
        };
    }
    store::emit_status(app);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn header(journal_id: u64, next_usn: i64) -> persist::IndexHeader {
        persist::IndexHeader {
            volume: "C".to_string(),
            count: 10,
            live_count: 10,
            truncated: 0,
            volume_serial: 1,
            journal_id,
            next_usn,
            built_at: 0,
            index_version: 1,
        }
    }

    fn journal(first_usn: i64, next_usn: i64, journal_id: u64) -> crate::mft::JournalSnapshot {
        crate::mft::JournalSnapshot {
            journal_id,
            first_usn,
            next_usn,
            lowest_valid_usn: 0,
        }
    }

    #[test]
    fn no_usn_progress_means_no_write() {
        assert_eq!(
            maintenance_plan(&header(7, 200), &journal(100, 200, 7)),
            Maintenance::UpToDate
        );
        assert_eq!(
            maintenance_plan(&header(7, 150), &journal(100, 200, 7)),
            Maintenance::Sync
        );
    }

    #[test]
    fn wrapped_or_replaced_journal_means_rebuild() {
        assert_eq!(
            maintenance_plan(&header(7, 50), &journal(100, 200, 7)),
            Maintenance::Rebuild
        );
        assert_eq!(
            maintenance_plan(&header(7, 150), &journal(100, 200, 8)),
            Maintenance::Rebuild
        );
        assert_eq!(
            maintenance_plan(&header(7, 250), &journal(100, 200, 7)),
            Maintenance::Rebuild
        );
    }
}

#[cfg(test)]
mod benchmark {
    use std::sync::atomic::AtomicBool;
    use std::time::Instant;

    use super::{builder, persist, query};

    /// Real-volume measurement (ignored by default):
    /// `cargo test --lib measure_real_volume -- --ignored --nocapture`
    #[test]
    #[ignore = "requires an NTFS volume with administrator rights"]
    fn measure_real_volume() {
        let root = std::path::Path::new("C:\\");
        if !crate::mft::is_supported(root) {
            println!("skipped: no raw volume access (run as administrator)");
            return;
        }
        let cancel = AtomicBool::new(false);
        let low = || false;

        let started = Instant::now();
        let data = crate::mft::collect_volume_entries(root, &cancel, &low, &mut |_, _| {})
            .expect("collect volume");
        let collect = started.elapsed();
        let count = data.entries.len();

        let build_started = Instant::now();
        let outcome = builder::build("C", data, &[], 5_000_000);
        let build = build_started.elapsed();
        let index = outcome.index;
        println!(
            "entries={count} collect={collect:?} build={build:?} memory={} bytes truncated={}",
            index.memory_bytes(),
            outcome.truncated
        );

        let snapshot_started = Instant::now();
        let bytes = persist::encode(&index);
        let decoded = persist::decode(&bytes);
        println!(
            "snapshot={} bytes encode+decode={:?} ok={}",
            bytes.len(),
            snapshot_started.elapsed(),
            decoded.is_ok()
        );

        for term in ["config", "ext:log", "报告"] {
            let query_started = Instant::now();
            let response = query::search(
                &index,
                &query::SearchRequest {
                    query: term.to_string(),
                    sort: "size".to_string(),
                    limit: 200,
                    ..Default::default()
                },
            );
            println!(
                "query={term:?} total={} server_ms={} wall={:?}",
                response.total,
                response.took_ms,
                query_started.elapsed()
            );
        }
    }
}
