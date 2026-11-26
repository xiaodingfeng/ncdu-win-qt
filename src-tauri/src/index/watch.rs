//! USN journal tailing: applies create / delete / rename / data changes to the
//! live index and persists it periodically.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};

use tauri::{AppHandle, Manager};

use crate::mft;

use super::model::{Index, VOLUME_ROOT_REF};
use super::persist;
use super::store::{self, IndexStore};

const REASON_DATA_OVERWRITE: u32 = 0x0000_0001;
const REASON_DATA_EXTEND: u32 = 0x0000_0002;
const REASON_DATA_TRUNCATION: u32 = 0x0000_0004;
const REASON_FILE_CREATE: u32 = 0x0000_0100;
const REASON_FILE_DELETE: u32 = 0x0000_0200;
const REASON_RENAME_NEW_NAME: u32 = 0x0000_2000;
const REASON_CLOSE: u32 = 0x8000_0000;

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
    std::thread::spawn(move || run(app, volume, stop));
}

fn run(app: AppHandle, volume: String, stop: Arc<AtomicBool>) {
    let letter = volume.chars().next().unwrap_or('C');
    let volume_handle = match mft::open_volume(letter) {
        Ok(handle) => handle,
        Err(err) => {
            log::warn!("[index] watcher cannot open volume {volume}: {err:?}");
            return;
        }
    };
    let mut last_save = Instant::now();
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
                log::warn!("[index] USN journal query failed: {err:?}");
                continue;
            }
        };
        if journal.journal_id != journal_id || next_usn < journal.lowest_valid_usn {
            log::warn!("[index] USN journal was reset; rebuilding volume {volume}");
            drop(store);
            super::commands::spawn_build(app.clone(), Some(vec![volume.clone()]));
            return;
        }
        match mft::read_usn_journal(&volume_handle, journal.journal_id, next_usn) {
            Ok((changes, next)) => {
                if changes.is_empty() {
                    if next != next_usn {
                        if let Ok(mut index) = handle.write() {
                            index.next_usn = next;
                        }
                    }
                } else {
                    apply(&handle, &changes, next);
                }
            }
            Err(err) => log::warn!("[index] reading the USN journal failed: {err:?}"),
        }
        if last_save.elapsed() >= Duration::from_secs(60) {
            last_save = Instant::now();
            if let Ok(index) = handle.read() {
                let _ = persist::save(&app, &index);
            }
        }
    }
    if let Some(handle) = app.state::<IndexStore>().index_for(&volume) {
        if let Ok(index) = handle.read() {
            let _ = persist::save(&app, &index);
        }
    }
}

fn apply(handle: &Arc<RwLock<Index>>, changes: &[mft::UsnChange], next_usn: i64) {
    // Resolve parent paths and read metadata before taking the write lock so
    // queries stay responsive.
    let mut resolved: Vec<(mft::UsnChange, u32, u64, i64)> = Vec::with_capacity(changes.len());
    {
        let Ok(index) = handle.read() else {
            return;
        };
        for change in changes {
            let parent = if change.parent_ref == VOLUME_ROOT_REF {
                0
            } else {
                index.lookup_ref(change.parent_ref).unwrap_or(0)
            };
            let parent_path = index.path_of(parent);
            let full = format!("{}\\{}", parent_path.trim_end_matches('\\'), change.name);
            let (size, mtime) = std::fs::metadata(&full)
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
                .unwrap_or((0, 0));
            resolved.push((change.clone(), parent, size, mtime));
        }
    }

    let Ok(mut index) = handle.write() else {
        return;
    };
    for (change, parent, size, mtime) in resolved {
        if change.reason & REASON_FILE_DELETE != 0 {
            index.mark_ref_deleted(change.file_ref);
            continue;
        }
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
                parent,
                &change.name,
                change.attributes,
                size,
                mtime,
            );
        }
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
}
