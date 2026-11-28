//! USN journal tailing: applies create / delete / rename / data changes to the
//! live index and persists it periodically. Runs at the lowest thread priority
//! and limits metadata work per batch so a bulk write cannot stall the UI.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};

use tauri::{AppHandle, Manager};

use crate::mft;

use super::model::{Index, VOLUME_ROOT_REF};
use super::persist;
use super::store::{self, lower_thread_priority, IndexStore};

const REASON_DATA_OVERWRITE: u32 = 0x0000_0001;
const REASON_DATA_EXTEND: u32 = 0x0000_0002;
const REASON_DATA_TRUNCATION: u32 = 0x0000_0004;
const REASON_FILE_CREATE: u32 = 0x0000_0100;
const REASON_FILE_DELETE: u32 = 0x0000_0200;
const REASON_RENAME_NEW_NAME: u32 = 0x0000_2000;
const REASON_CLOSE: u32 = 0x8000_0000;

const SAVE_PERIOD: Duration = Duration::from_secs(600);
const MAX_BATCHES: usize = 8;
const METADATA_BUDGET: usize = 512;
const ERROR_LOG_PERIOD: Duration = Duration::from_secs(300);

pub(crate) fn start(app: AppHandle, volume: String, realtime: bool) {
    let store = app.state::<IndexStore>();
    store.stop_watcher(&volume);
    if !realtime {
        store::emit_status(&app);
        return;
    }

    let stop = Arc::new(AtomicBool::new(false));
    {
        let store = app.state::<IndexStore>();
        store.register_watcher(volume.clone(), stop.clone());
    }
    store::emit_status(&app);
    std::thread::spawn(move || {
        lower_thread_priority();
        run(app, volume, stop);
    });
}

/// True when the saved USN position can no longer be replayed.
fn journal_reset(journal: &mft::JournalSnapshot, saved_id: u64, next_usn: i64) -> bool {
    // `lowest_valid_usn` is often 0, so `first_usn` is the reliable bound.
    let lower = journal.first_usn.max(journal.lowest_valid_usn);
    journal.journal_id != saved_id || next_usn < lower || next_usn > journal.next_usn
}

struct RateLimit {
    last: Option<Instant>,
}

impl RateLimit {
    fn new() -> Self {
        Self { last: None }
    }

    fn allow(&mut self) -> bool {
        if self.last.map(|time| time.elapsed() < ERROR_LOG_PERIOD).unwrap_or(false) {
            return false;
        }
        self.last = Some(Instant::now());
        true
    }
}

fn run(app: AppHandle, volume: String, stop: Arc<AtomicBool>) {
    let letter = volume.chars().next().unwrap_or('C');
    let volume_handle = match mft::open_volume(letter) {
        Ok(handle) => handle,
        Err(err) => {
            log::warn!("[index] watcher cannot open volume {volume}: {err:?}");
            super::commands::set_volume_message(
                &app,
                &volume,
                "realtime updates unavailable (administrator rights required)",
            );
            return;
        }
    };
    let mut last_save = Instant::now();
    let mut dirty = false;
    let mut query_errors = RateLimit::new();
    let mut read_errors = RateLimit::new();
    while !stop.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_secs(2));
        if stop.load(Ordering::Relaxed) {
            break;
        }
        let store = app.state::<IndexStore>();
        let Some(handle) = store.index_for(&volume) else {
            break;
        };
        let (journal_id, next_usn) = match handle.read() {
            Ok(index) => (index.journal_id, index.next_usn),
            Err(_) => break,
        };
        let journal = match mft::query_journal(&volume_handle) {
            Ok(journal) => journal,
            Err(err) => {
                if query_errors.allow() {
                    log::warn!("[index] USN journal query failed: {err:?}");
                }
                continue;
            }
        };
        if journal_reset(&journal, journal_id, next_usn) {
            log::warn!("[index] USN journal was reset; rebuilding volume {volume}");
            drop(store);
            super::commands::spawn_build(app.clone(), Some(vec![volume.clone()]));
            return;
        }
        if journal.next_usn == next_usn {
            // Nothing happened: one journal query per 2s, no read ioctl.
            continue;
        }

        let mut cursor = next_usn;
        let mut changes: Vec<mft::UsnChange> = Vec::new();
        let mut reset = false;
        for _ in 0..MAX_BATCHES {
            match mft::read_usn_journal(&volume_handle, journal.journal_id, cursor) {
                Ok((batch, next)) => {
                    if batch.is_empty() && next == cursor {
                        break;
                    }
                    changes.extend(batch);
                    cursor = next;
                    if next >= journal.next_usn {
                        break;
                    }
                }
                Err(mft::MftError::JournalReset) => {
                    reset = true;
                    break;
                }
                Err(err) => {
                    if read_errors.allow() {
                        log::warn!("[index] reading the USN journal failed: {err:?}");
                    }
                    break;
                }
            }
        }
        if reset {
            drop(store);
            super::commands::spawn_build(app.clone(), Some(vec![volume.clone()]));
            return;
        }
        if changes.is_empty() {
            if cursor != next_usn {
                if let Ok(mut index) = handle.write() {
                    index.next_usn = cursor;
                }
                dirty = true;
            }
        } else {
            apply(&handle, &changes, cursor);
            dirty = true;
        }
        if dirty && last_save.elapsed() >= SAVE_PERIOD {
            last_save = Instant::now();
            if save(&app, &volume, &handle) {
                dirty = false;
            }
        }
    }
    if let Some(handle) = app.state::<IndexStore>().index_for(&volume) {
        save(&app, &volume, &handle);
    }
}

/// Writes the current index and resets the delta log (it is now part of the
/// base snapshot).
fn save(app: &AppHandle, volume: &str, handle: &Arc<RwLock<Index>>) -> bool {
    let Ok(index) = handle.read() else {
        return false;
    };
    if persist::save(app, &index).is_ok() {
        persist::reset_delta(app, volume);
        true
    } else {
        false
    }
}

struct Resolved {
    change: mft::UsnChange,
    parent: u32,
    size: u64,
    mtime: i64,
    deleted: bool,
}

fn apply(handle: &Arc<RwLock<Index>>, changes: &[mft::UsnChange], next_usn: i64) {
    // Resolve parent paths and read metadata before taking the write lock so
    // queries stay responsive. Metadata calls are budgeted per batch.
    let mut resolved: Vec<Resolved> = Vec::with_capacity(changes.len());
    {
        let Ok(index) = handle.read() else {
            return;
        };
        let mut parent_path = String::new();
        let mut full = String::new();
        let mut metadata_budget = METADATA_BUDGET;
        for change in changes {
            let parent = if change.parent_ref == VOLUME_ROOT_REF {
                0
            } else {
                index.lookup_ref(change.parent_ref).unwrap_or(0)
            };
            if change.reason & REASON_FILE_DELETE != 0 {
                resolved.push(Resolved {
                    change: change.clone(),
                    parent,
                    size: 0,
                    mtime: 0,
                    deleted: true,
                });
                continue;
            }
            let (size, mtime) = if metadata_budget > 0 {
                metadata_budget -= 1;
                index.path_into(parent, &mut parent_path);
                full.clear();
                full.push_str(parent_path.trim_end_matches('\\'));
                full.push('\\');
                full.push_str(&change.name);
                std::fs::metadata(&full)
                    .map(|metadata| {
                        let mtime = metadata
                            .modified()
                            .ok()
                            .and_then(|value| value.duration_since(std::time::UNIX_EPOCH).ok())
                            .map(|value| value.as_secs() as i64)
                            .unwrap_or(0);
                        if metadata.is_dir() {
                            (0, mtime)
                        } else {
                            (metadata.len(), mtime)
                        }
                    })
                    .unwrap_or((0, 0))
            } else {
                (0, 0)
            };
            resolved.push(Resolved {
                change: change.clone(),
                parent,
                size,
                mtime,
                deleted: false,
            });
        }
    }

    let Ok(mut index) = handle.write() else {
        return;
    };
    // Deletions are collected and propagated once, so a bulk delete costs one
    // pass instead of O(n) per entry.
    let mut deleted: HashMap<u64, u32> = HashMap::new();
    for item in resolved {
        let change = &item.change;
        if item.deleted {
            if let Some(idx) = index.lookup_ref(change.file_ref) {
                deleted.insert(change.file_ref, idx);
            }
            continue;
        }
        deleted.remove(&change.file_ref);
        let changed = change.reason
            & (REASON_FILE_CREATE
                | REASON_RENAME_NEW_NAME
                | REASON_DATA_OVERWRITE
                | REASON_DATA_EXTEND
                | REASON_DATA_TRUNCATION
                | REASON_CLOSE)
            != 0;
        if changed || index.lookup_ref(change.file_ref).is_none() {
            index.upsert(
                change.file_ref,
                item.parent,
                &change.name,
                change.attributes,
                item.size,
                item.mtime,
            );
        }
    }
    if !deleted.is_empty() {
        let roots: Vec<u32> = deleted.into_values().collect();
        index.mark_many_deleted(&roots);
    }
    index.next_usn = next_usn;
    index.version = index.version.wrapping_add(1);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn created_and_deleted_entries_are_applied() {
        let mut index = Index::new("C");
        index.journal_id = 5;
        index.next_usn = 10;
        let handle = Arc::new(RwLock::new(index));

        let created = mft::UsnChange {
            file_ref: 42,
            parent_ref: VOLUME_ROOT_REF,
            name: "hello.txt".to_string(),
            attributes: 0,
            reason: REASON_FILE_CREATE | REASON_CLOSE,
        };
        apply(&handle, &[created], 11);
        {
            let index = handle.read().unwrap();
            let idx = index.lookup_ref(42).expect("created entry");
            assert_eq!(index.path_of(idx), "C:\\hello.txt");
            assert_eq!(index.next_usn, 11);
        }

        let deleted = mft::UsnChange {
            file_ref: 42,
            parent_ref: VOLUME_ROOT_REF,
            name: "hello.txt".to_string(),
            attributes: 0,
            reason: REASON_FILE_DELETE | REASON_CLOSE,
        };
        apply(&handle, &[deleted], 12);
        {
            let index = handle.read().unwrap();
            let idx = index.lookup_ref(42).expect("slot kept");
            assert!(!index.is_live(idx));
            assert_eq!(index.next_usn, 12);
        }
    }

    #[test]
    fn rename_reuses_the_same_slot() {
        let mut index = Index::new("C");
        let idx = index.push_entry(7, 0, "before.log", 0, 1, 0);
        let handle = Arc::new(RwLock::new(index));
        let renamed = mft::UsnChange {
            file_ref: 7,
            parent_ref: VOLUME_ROOT_REF,
            name: "after.log".to_string(),
            attributes: 0,
            reason: REASON_RENAME_NEW_NAME,
        };
        apply(&handle, &[renamed], 20);
        let index = handle.read().unwrap();
        assert_eq!(index.name(idx), "after.log");
        assert_eq!(index.ext(idx), "log");
        assert_eq!(index.lookup_ref(7), Some(idx));
    }

    #[test]
    fn journal_reset_detection_uses_first_usn() {
        let journal = mft::JournalSnapshot {
            journal_id: 9,
            first_usn: 100,
            next_usn: 200,
            lowest_valid_usn: 0,
        };
        assert!(journal_reset(&journal, 9, 50));
        assert!(!journal_reset(&journal, 9, 150));
        assert!(journal_reset(&journal, 8, 150));
        assert!(journal_reset(&journal, 9, 250));
    }

    #[test]
    fn deleted_then_recreated_in_one_batch_stays_live() {
        let mut index = Index::new("C");
        let idx = index.push_entry(42, 0, "old.txt", 0, 1, 0);
        let handle = Arc::new(RwLock::new(index));
        let deleted = mft::UsnChange {
            file_ref: 42,
            parent_ref: VOLUME_ROOT_REF,
            name: "old.txt".to_string(),
            attributes: 0,
            reason: REASON_FILE_DELETE,
        };
        let recreated = mft::UsnChange {
            file_ref: 42,
            parent_ref: VOLUME_ROOT_REF,
            name: "new.txt".to_string(),
            attributes: 0,
            reason: REASON_FILE_CREATE | REASON_CLOSE,
        };
        apply(&handle, &[deleted, recreated], 30);
        let index = handle.read().unwrap();
        assert!(index.is_live(idx));
        assert_eq!(index.name(idx), "new.txt");
    }
}
