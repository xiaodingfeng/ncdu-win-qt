//! Packs a volume read into the compact search index.

use std::collections::HashMap;
use std::os::windows::fs::MetadataExt;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::mft::VolumeIndexData;

use super::model::{fold_contains, Index, VOLUME_ROOT_REF};

#[derive(Debug)]
pub(crate) enum WalkError {
    Cancelled,
    LowMemory,
}

/// Degraded fallback for non-NTFS volumes: a plain directory walk. It keeps
/// the same index layout so search and result actions behave identically.
pub(crate) fn walk_volume(
    volume: &str,
    root: &Path,
    cancel: &AtomicBool,
    low_memory: &dyn Fn() -> bool,
    excludes: &[String],
    max_entries: u32,
    progress: &mut dyn FnMut(u64),
) -> Result<Index, WalkError> {
    let mut index = Index::new(volume);
    index.built_at = now_secs();
    let excludes: Vec<String> = excludes
        .iter()
        .map(|rule| rule.trim().to_ascii_lowercase())
        .filter(|rule| !rule.is_empty())
        .collect();
    let limit = max_entries.max(1) as usize;
    let mut next_ref: u64 = VOLUME_ROOT_REF + 1;
    let mut stack: Vec<(std::path::PathBuf, u32)> = vec![(root.to_path_buf(), 0)];
    let mut count = 0u64;

    while let Some((dir, parent)) = stack.pop() {
        if cancel.load(Ordering::Relaxed) {
            return Err(WalkError::Cancelled);
        }
        if low_memory() {
            return Err(WalkError::LowMemory);
        }
        let Ok(reader) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in reader.flatten() {
            if index.len() >= limit {
                index.rebuild_refs();
                index.build_ext_buckets();
                index.compact();
                return Ok(index);
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.is_empty() || excludes.iter().any(|rule| fold_contains(&name, rule)) {
                continue;
            }
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            let attributes = metadata.file_attributes();
            let reparse = attributes & 0x400 != 0;
            let is_dir = metadata.is_dir() && !reparse;
            let size = if is_dir { 0 } else { metadata.len() };
            let mtime = metadata
                .modified()
                .ok()
                .and_then(|value| value.duration_since(std::time::UNIX_EPOCH).ok())
                .map(|value| value.as_secs() as i64)
                .unwrap_or(0);
            let file_ref = next_ref;
            next_ref += 1;
            let idx = index.push_entry(file_ref, parent, &name, attributes, size, mtime);
            count += 1;
            if count % 5_000 == 0 {
                progress(count);
            }
            if is_dir {
                stack.push((entry.path(), idx));
            }
        }
    }
    index.rebuild_refs();
    index.build_ext_buckets();
    index.compact();
    Ok(index)
}

pub(crate) fn build(
    volume: &str,
    data: VolumeIndexData,
    excludes: &[String],
    max_entries: u32,
) -> Index {
    let mut index = Index::new(volume);
    index.volume_serial = data.volume_serial;
    index.journal_id = data.journal_id;
    index.next_usn = data.next_usn;
    index.built_at = now_secs();

    let limit = max_entries.max(1) as usize;
    let excludes: Vec<String> = excludes
        .iter()
        .map(|rule| rule.trim().to_ascii_lowercase())
        .filter(|rule| !rule.is_empty())
        .collect();

    // Parent references can only be resolved once every entry slot exists.
    let mut parent_links: Vec<(u64, u32)> = Vec::with_capacity(data.entries.len());
    for entry in data.entries {
        if index.len() >= limit {
            break;
        }
        if entry.file_ref == VOLUME_ROOT_REF {
            continue;
        }
        if excludes.iter().any(|rule| fold_contains(&entry.name, rule)) {
            continue;
        }
        let idx = index.push_entry(
            entry.file_ref,
            u32::MAX,
            &entry.name,
            entry.attributes,
            entry.size,
            entry.mtime,
        );
        parent_links.push((entry.parent_ref, idx));
    }

    let mut by_ref: HashMap<u64, u32> = index
        .file_refs
        .iter()
        .enumerate()
        .map(|(position, file_ref)| (*file_ref, position as u32))
        .collect();
    for (parent_ref, idx) in parent_links {
        let parent = by_ref
            .get(&parent_ref)
            .copied()
            .filter(|parent| *parent != idx)
            .unwrap_or(0);
        index.set_parent(idx, parent);
    }
    by_ref.clear();

    index.rebuild_refs();
    index.build_ext_buckets();
    index.compact();
    index
}

pub(crate) fn now_secs() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|value| value.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mft::VolumeEntry;

    fn entry(file_ref: u64, parent_ref: u64, name: &str, attributes: u32) -> VolumeEntry {
        VolumeEntry {
            file_ref,
            parent_ref,
            name: name.to_string(),
            attributes,
            size: 0,
            mtime: 0,
        }
    }

    fn data(entries: Vec<VolumeEntry>) -> VolumeIndexData {
        VolumeIndexData {
            entries,
            journal_id: 7,
            next_usn: 100,
            volume_serial: 9,
        }
    }

    #[test]
    fn parents_resolve_to_slots_and_orphans_hang_off_the_root() {
        let entries = vec![
            entry(10, 5, "Windows", 0x10),
            entry(11, 10, "System32", 0x10),
            entry(12, 999, "orphan.txt", 0),
        ];
        let index = build("C", data(entries), &[], 1_000);
        assert_eq!(index.parents[1], 0);
        assert_eq!(index.path_of(2), "C:\\Windows\\System32");
        assert_eq!(index.path_of(3), "C:\\orphan.txt");
        assert_eq!(index.journal_id, 7);
        assert_eq!(index.next_usn, 100);
    }

    #[test]
    fn max_entries_and_excludes_are_applied() {
        let entries = vec![
            entry(10, 5, "keep", 0x10),
            entry(11, 5, "$Recycle.Bin", 0x10),
            entry(12, 5, "extra", 0),
        ];
        let index = build("C", data(entries), &["recycle.bin".to_string()], 2);
        assert_eq!(index.len(), 2);
        assert_eq!(index.path_of(1), "C:\\keep");
        assert!(index.lookup_ref(11).is_none());
    }

    #[test]
    fn the_directory_walk_fallback_builds_a_searchable_index() {
        let root = std::env::temp_dir().join(format!("ncduwin-walk-{}", std::process::id()));
        let nested = root.join("sub");
        std::fs::create_dir_all(&nested).expect("create temp tree");
        std::fs::write(root.join("a.txt"), b"hello").expect("write a");
        std::fs::write(nested.join("b.log"), b"world!!").expect("write b");

        let cancel = AtomicBool::new(false);
        let index = walk_volume("T", &root, &cancel, &|| false, &[], 1_000, &mut |_| {})
            .expect("walk");
        let names: Vec<&str> = (1..index.len() as u32).map(|idx| index.name(idx)).collect();
        assert!(names.contains(&"a.txt"));
        assert!(names.contains(&"b.log"));
        assert!(names.contains(&"sub"));
        let log = (1..index.len() as u32)
            .find(|idx| index.name(*idx) == "b.log")
            .expect("b.log slot");
        assert!(index.path_of(log).ends_with("sub\\b.log"));
        assert_eq!(index.journal_id, 0);

        std::fs::remove_dir_all(&root).ok();
    }
}
