//! Compact, array-of-structs index layout. Full paths are never stored: names
//! live in one UTF-8 pool and paths are rebuilt from the parent chain only when
//! a caller actually needs them.

pub(crate) const FLAG_DIRECTORY: u8 = 0x01;
pub(crate) const FLAG_HIDDEN: u8 = 0x02;
pub(crate) const FLAG_DELETED: u8 = 0x80;

const ATTRIBUTE_HIDDEN: u32 = 0x2;
const ATTRIBUTE_DIRECTORY: u32 = 0x10;
const MAX_DEPTH: u32 = 512;

/// Root of a volume in the USN journal is always file reference 5.
pub(crate) const VOLUME_ROOT_REF: u64 = 5;

pub(crate) fn extension_of(name: &str) -> &str {
    match name.rfind('.') {
        Some(index) if index + 1 < name.len() => &name[index + 1..],
        _ => "",
    }
}

pub(crate) fn flags_for(attributes: u32, name: &str) -> u8 {
    let mut flags = 0u8;
    if attributes & ATTRIBUTE_DIRECTORY != 0 {
        flags |= FLAG_DIRECTORY;
    }
    if attributes & ATTRIBUTE_HIDDEN != 0 || name.starts_with('.') || name.starts_with('$') {
        flags |= FLAG_HIDDEN;
    }
    flags
}

/// ASCII-only case folding keeps byte offsets stable for the name pool.
pub(crate) fn fold_char(value: char) -> char {
    if value.is_ascii() {
        value.to_ascii_lowercase()
    } else {
        value
    }
}

/// ASCII-insensitive substring test without allocating a folded copy.
pub(crate) fn fold_contains(haystack: &str, needle_folded: &str) -> bool {
    let hay = haystack.as_bytes();
    let needle = needle_folded.as_bytes();
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    for start in 0..=(hay.len() - needle.len()) {
        if hay[start..start + needle.len()]
            .iter()
            .zip(needle)
            .all(|(left, right)| left.to_ascii_lowercase() == *right)
        {
            return true;
        }
    }
    false
}

pub(crate) struct Index {
    pub(crate) volume: String,
    pub(crate) volume_serial: u64,
    pub(crate) journal_id: u64,
    pub(crate) next_usn: i64,
    pub(crate) built_at: i64,
    pub(crate) version: u64,

    pub(crate) names: String,
    pub(crate) name_offsets: Vec<u32>,
    pub(crate) name_lens: Vec<u16>,
    pub(crate) parents: Vec<u32>,
    pub(crate) file_refs: Vec<u64>,
    pub(crate) sizes: Vec<u64>,
    pub(crate) mtimes: Vec<i64>,
    pub(crate) ext_ids: Vec<u16>,
    pub(crate) flags: Vec<u8>,
    pub(crate) exts: Vec<String>,
    pub(crate) ext_buckets: Vec<Vec<u32>>,

    /// Slot indices sorted by `file_refs[slot]`. Four bytes per entry instead
    /// of a hash map's ~24, and USN lookups become a binary search.
    pub(crate) ref_order: Vec<u32>,
}

impl Index {
    pub(crate) fn new(volume: &str) -> Self {
        let mut index = Self {
            volume: volume.to_string(),
            volume_serial: 0,
            journal_id: 0,
            next_usn: 0,
            built_at: 0,
            version: 0,
            names: String::new(),
            name_offsets: Vec::new(),
            name_lens: Vec::new(),
            parents: Vec::new(),
            file_refs: Vec::new(),
            sizes: Vec::new(),
            mtimes: Vec::new(),
            ext_ids: Vec::new(),
            flags: Vec::new(),
            exts: vec![String::new()],
            ext_buckets: vec![Vec::new()],
            ref_order: Vec::new(),
        };
        // Slot 0 is the volume root: an empty name whose parent is itself.
        index.push_entry(VOLUME_ROOT_REF, u32::MAX, "", ATTRIBUTE_DIRECTORY, 0, 0);
        index
    }

    pub(crate) fn len(&self) -> usize {
        self.file_refs.len()
    }

    pub(crate) fn name(&self, idx: u32) -> &str {
        let slot = idx as usize;
        let Some(offset) = self.name_offsets.get(slot).copied() else {
            return "";
        };
        let Some(length) = self.name_lens.get(slot).copied() else {
            return "";
        };
        let start = offset as usize;
        let end = start + length as usize;
        self.names.get(start..end).unwrap_or("")
    }

    pub(crate) fn ext(&self, idx: u32) -> &str {
        self.ext_ids
            .get(idx as usize)
            .and_then(|id| self.exts.get(*id as usize))
            .map(String::as_str)
            .unwrap_or("")
    }

    /// Entry slots for one extension, used to shrink `ext:` queries.
    pub(crate) fn ext_bucket(&self, extension: &str) -> Option<&[u32]> {
        let id = self.exts.iter().position(|value| value == extension)?;
        self.ext_buckets.get(id).map(Vec::as_slice)
    }

    pub(crate) fn build_ext_buckets(&mut self) {
        self.ext_buckets = vec![Vec::new(); self.exts.len()];
        for (position, id) in self.ext_ids.iter().enumerate() {
            if let Some(bucket) = self.ext_buckets.get_mut(*id as usize) {
                bucket.push(position as u32);
            }
        }
    }

    pub(crate) fn is_dir(&self, idx: u32) -> bool {
        self.flags
            .get(idx as usize)
            .map(|flags| flags & FLAG_DIRECTORY != 0)
            .unwrap_or(false)
    }

    pub(crate) fn is_hidden(&self, idx: u32) -> bool {
        self.flags
            .get(idx as usize)
            .map(|flags| flags & FLAG_HIDDEN != 0)
            .unwrap_or(false)
    }

    pub(crate) fn is_live(&self, idx: u32) -> bool {
        self.flags
            .get(idx as usize)
            .map(|flags| flags & FLAG_DELETED == 0)
            .unwrap_or(false)
    }

    pub(crate) fn live_count(&self) -> u64 {
        self.flags
            .iter()
            .filter(|flags| *flags & FLAG_DELETED == 0)
            .count() as u64
    }

    pub(crate) fn memory_bytes(&self) -> u64 {
        let exts: usize = self.exts.iter().map(|ext| ext.len() + 16).sum();
        (self.names.len()
            + self.name_offsets.len() * 4
            + self.name_lens.len() * 2
            + self.parents.len() * 4
            + self.file_refs.len() * 8
            + self.sizes.len() * 8
            + self.mtimes.len() * 8
            + self.ext_ids.len() * 2
            + self.flags.len()
            + exts
            + self.ext_buckets.iter().map(|bucket| bucket.len() * 4 + 24).sum::<usize>()
            + self.ref_order.len() * 4) as u64
    }

    /// Releases the growth slack left by `push`-based construction. Run once
    /// after a build or a snapshot decode so long-lived memory matches usage.
    pub(crate) fn compact(&mut self) {
        self.names.shrink_to_fit();
        self.name_offsets.shrink_to_fit();
        self.name_lens.shrink_to_fit();
        self.parents.shrink_to_fit();
        self.file_refs.shrink_to_fit();
        self.sizes.shrink_to_fit();
        self.mtimes.shrink_to_fit();
        self.ext_ids.shrink_to_fit();
        self.flags.shrink_to_fit();
        self.exts.shrink_to_fit();
        for bucket in &mut self.ext_buckets {
            bucket.shrink_to_fit();
        }
        self.ext_buckets.shrink_to_fit();
        self.ref_order.shrink_to_fit();
    }

    /// Finds the slot for a file reference with a binary search over
    /// `ref_order`, which stays sorted by `file_refs`.
    pub(crate) fn lookup_ref(&self, file_ref: u64) -> Option<u32> {
        let position = self
            .ref_order
            .binary_search_by(|slot| self.file_refs[*slot as usize].cmp(&file_ref))
            .ok()?;
        self.ref_order.get(position).copied()
    }

    pub(crate) fn intern_ext(&mut self, name: &str) -> u16 {
        let extension = extension_of(name).to_ascii_lowercase();
        if extension.is_empty() {
            return 0;
        }
        if let Some(position) = self.exts.iter().position(|ext| *ext == extension) {
            return position as u16;
        }
        self.exts.push(extension);
        (self.exts.len() - 1) as u16
    }

    pub(crate) fn push_entry(
        &mut self,
        file_ref: u64,
        parent: u32,
        name: &str,
        attributes: u32,
        size: u64,
        mtime: i64,
    ) -> u32 {
        let idx = self.file_refs.len() as u32;
        let ext_id = self.intern_ext(name);
        self.name_offsets.push(self.names.len() as u32);
        self.name_lens
            .push(name.len().min(u16::MAX as usize) as u16);
        self.names.push_str(name);
        self.parents.push(parent);
        self.file_refs.push(file_ref);
        self.sizes.push(size);
        self.mtimes.push(mtime);
        self.ext_ids.push(ext_id);
        self.flags.push(flags_for(attributes, name));
        self.ext_buckets.resize(self.exts.len(), Vec::new());
        if let Some(bucket) = self.ext_buckets.get_mut(ext_id as usize) {
            bucket.push(idx);
        }
        // Appended here; `rebuild_refs` sorts the bulk build, and `upsert`
        // re-inserts a single new slot in order.
        self.ref_order.push(idx);
        idx
    }

    pub(crate) fn set_parent(&mut self, idx: u32, parent: u32) {
        if let Some(slot) = self.parents.get_mut(idx as usize) {
            *slot = parent;
        }
    }

    fn set_name(&mut self, idx: u32, name: &str) {
        let slot = idx as usize;
        if slot >= self.name_offsets.len() {
            return;
        }
        self.name_offsets[slot] = self.names.len() as u32;
        self.name_lens[slot] = name.len().min(u16::MAX as usize) as u16;
        self.names.push_str(name);
    }

    pub(crate) fn path_of(&self, idx: u32) -> String {
        let mut parts: Vec<&str> = Vec::new();
        let mut current = idx;
        let mut depth = 0u32;
        loop {
            let name = self.name(current);
            if name.is_empty() {
                break;
            }
            parts.push(name);
            let Some(parent) = self.parents.get(current as usize).copied() else {
                break;
            };
            if parent == u32::MAX || parent == current {
                break;
            }
            current = parent;
            depth += 1;
            if depth > MAX_DEPTH {
                break;
            }
        }
        let mut path = format!("{}:\\", self.volume);
        for (position, part) in parts.iter().rev().enumerate() {
            if position > 0 {
                path.push('\\');
            }
            path.push_str(part);
        }
        path
    }

    /// Marks an entry and everything below it as deleted. The index keeps its
    /// slots so file reference numbers stay stable until the next rebuild.
    pub(crate) fn mark_deleted(&mut self, idx: u32) {
        let Some(slot) = self.flags.get_mut(idx as usize) else {
            return;
        };
        *slot |= FLAG_DELETED;
        loop {
            let mut changed = false;
            for position in 1..self.len() {
                if self.flags[position] & FLAG_DELETED != 0 {
                    continue;
                }
                let parent = self.parents[position];
                if parent != u32::MAX
                    && (parent as usize) < self.flags.len()
                    && self.flags[parent as usize] & FLAG_DELETED != 0
                {
                    self.flags[position] |= FLAG_DELETED;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
    }

    pub(crate) fn mark_ref_deleted(&mut self, file_ref: u64) {
        if let Some(idx) = self.lookup_ref(file_ref) {
            self.mark_deleted(idx);
        }
    }

    /// Inserts or refreshes one entry after a USN change.
    pub(crate) fn upsert(
        &mut self,
        file_ref: u64,
        parent: u32,
        name: &str,
        attributes: u32,
        size: u64,
        mtime: i64,
    ) -> u32 {
        if let Some(idx) = self.lookup_ref(file_ref) {
            let slot = idx as usize;
            self.set_name(idx, name);
            self.parents[slot] = parent;
            self.sizes[slot] = size;
            self.mtimes[slot] = mtime;
            let previous = self.ext_ids[slot] as usize;
            if let Some(bucket) = self.ext_buckets.get_mut(previous) {
                if let Some(position) = bucket.iter().position(|value| *value == idx) {
                    bucket.remove(position);
                }
            }
            let ext_id = self.intern_ext(name);
            self.ext_ids[slot] = ext_id;
            self.flags[slot] = flags_for(attributes, name);
            self.ext_buckets.resize(self.exts.len(), Vec::new());
            if let Some(bucket) = self.ext_buckets.get_mut(ext_id as usize) {
                bucket.push(idx);
            }
            idx
        } else {
            let idx = self.push_entry(file_ref, parent, name, attributes, size, mtime);
            // `push_entry` appended the slot; move it to its sorted position in
            // the (still sorted) reference order.
            self.ref_order.pop();
            let position = self
                .ref_order
                .partition_point(|slot| self.file_refs[*slot as usize] < file_ref);
            self.ref_order.insert(position, idx);
            idx
        }
    }

    pub(crate) fn rebuild_refs(&mut self) {
        self.ref_order.clear();
        self.ref_order.extend(0..self.file_refs.len() as u32);
        self.ref_order
            .sort_unstable_by_key(|slot| self.file_refs[*slot as usize]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_are_rebuilt_from_the_parent_chain() {
        let mut index = Index::new("C");
        let users = index.push_entry(10, 0, "Users", ATTRIBUTE_DIRECTORY, 0, 0);
        let file = index.push_entry(11, users, "notes.txt", 0, 120, 1_700_000_000);
        index.set_parent(users, 0);
        assert_eq!(index.path_of(0), "C:\\");
        assert_eq!(index.path_of(users), "C:\\Users");
        assert_eq!(index.path_of(file), "C:\\Users\\notes.txt");
        assert_eq!(index.ext(file), "txt");
        assert!(index.is_live(file));
    }

    #[test]
    fn deleting_a_directory_marks_the_whole_subtree() {
        let mut index = Index::new("C");
        let users = index.push_entry(10, 0, "Users", ATTRIBUTE_DIRECTORY, 0, 0);
        let folder = index.push_entry(11, users, "cache", ATTRIBUTE_DIRECTORY, 0, 0);
        let file = index.push_entry(12, folder, "a.bin", 0, 10, 0);
        index.set_parent(users, 0);
        index.set_parent(folder, users);
        index.set_parent(file, folder);
        index.mark_deleted(folder);
        assert!(!index.is_live(folder));
        assert!(!index.is_live(file));
        assert!(index.is_live(users));
    }

    #[test]
    fn name_lengths_survive_utf8_and_case_folding_is_ascii_only() {
        let mut index = Index::new("C");
        let idx = index.push_entry(10, 0, "报告 Final.TXT", 0, 1, 0);
        assert_eq!(index.name(idx), "报告 Final.TXT");
        assert!(fold_contains(index.name(idx), "final.txt"));
        assert!(!fold_contains("ÄPFEL", "äpfel"));
    }

    #[test]
    fn upsert_keeps_a_stable_slot_for_renames() {
        let mut index = Index::new("C");
        let first = index.push_entry(20, 0, "old.log", 0, 5, 0);
        let second = index.upsert(20, 0, "new.log", 0, 9, 42);
        assert_eq!(first, second);
        assert_eq!(index.name(first), "new.log");
        assert_eq!(index.sizes[first as usize], 9);
        assert_eq!(index.ext(first), "log");
    }
}
