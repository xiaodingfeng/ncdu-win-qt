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

use std::sync::atomic::Ordering;
use std::sync::{Arc, RwLock};

use tauri::{AppHandle, Manager};

use store::{IndexStore, VolumeSlot};

/// Loads snapshots for every configured volume in the background. Failures
/// only leave those volumes idle so the user can build them manually.
pub(crate) fn startup(app: AppHandle) {
    std::thread::spawn(move || {
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

        let writable = app.state::<IndexStore>().writable();
        let mut broken: Vec<String> = Vec::new();
        for volume in &targets {
            match persist::load(&app, &volume) {
                Ok(index) => {
                    let built_at = index.built_at;
                    let journal_id = index.journal_id;
                    let generation = app.state::<IndexStore>().generation.load(Ordering::SeqCst);
                    {
                        let store = app.state::<IndexStore>();
                        if let Ok(mut state) = store.state.write() {
                            if let Some(slot) = state.volumes.get_mut(volume) {
                                slot.index = Some(Arc::new(RwLock::new(index)));
                                slot.status = "ready".to_string();
                                slot.message.clear();
                                slot.built_at = built_at;
                                slot.elapsed_ms = 0;
                                slot.snapshot = true;
                                slot.generation = generation;
                            }
                        };
                    }
                    log::info!("index snapshot loaded for volume {volume}");
                    if settings.index.realtime && journal_id != 0 && writable {
                        watch::start(app.clone(), volume.clone(), true);
                    }
                }
                Err(err) => {
                    log::info!("no usable index snapshot for volume {volume}: {err}");
                    // A missing snapshot is just as much a reason to build as a
                    // corrupt one: the configured volumes are expected to be
                    // searchable right after startup.
                    if writable {
                        broken.push(volume.clone());
                    }
                }
            }
        }
        store::emit_status(&app);
        if !broken.is_empty() {
            log::info!("building indexes for volumes without a usable snapshot: {broken:?}");
            commands::spawn_build(app.clone(), Some(broken));
        }
    });
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
        let index = builder::build("C", data, &[], 5_000_000);
        let build = build_started.elapsed();
        println!(
            "entries={count} collect={collect:?} build={build:?} memory={} bytes",
            index.memory_bytes()
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
