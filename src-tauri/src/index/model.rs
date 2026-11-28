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
    /// Candidate entries dropped by the `max_entries` cap (0 when untruncated).
    pub(crate) truncated_entries: u64,

    pub(crate) names: String,
    pub(crate) name_offsets: Vec<u32>,
    pub(crate) name_lens: Vec<u16>,
    pub(crate) parents: Vec<u32>,
    pub(crate) file_refs: Vec<u64>,
    pub(crate) sizes: Vec<u64>,
    /// Unix seconds. u32 keeps the fixed per-entry cost low and covers
    /// 1970-2106, which is the whole useful range here.
    pub(crate) mtimes: Vec<u32>,
    pub(crate) ext_ids: Vec<u16>,
    pub(crate) flags: Vec<u8>,
    pub(crate) exts: Vec<String>,
    /// Number of slots without `FLAG_DELETED`, maintained incrementally so
    /// status queries never scan the whole index.
    pub(crate) live_count: u32,

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
            truncated_entries: 0,
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
            live_count: 0,
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

    #[cfg(test)]
    pub(crate) fn ext(&self, idx: u32) -> &str {
        self.ext_ids
            .get(idx as usize)
            .and_then(|id| self.exts.get(*id as usize))
            .map(String::as_str)
            .unwrap_or("")
    }

    /// Resolves an extension to its numeric id, used to compile `ext:` terms.
    pub(crate) fn ext_id(&self, extension: &str) -> Option<u16> {
        self.exts
            .iter()
            .position(|value| value == extension)
            .map(|position| position as u16)
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

    /// Constant-time: deletion/insertion maintain the counter.
    pub(crate) fn live_count(&self) -> u64 {
        self.live_count as u64
    }

    pub(crate) fn memory_bytes(&self) -> u64 {
        let exts: usize = self.exts.iter().map(|ext| ext.len() + 16).sum();
        (self.names.len()
            + self.name_offsets.len() * 4
            + self.name_lens.len() * 2
            + self.parents.len() * 4
            + self.file_refs.len() * 8
            + self.sizes.len() * 8
            + self.mtimes.len() * 4
            + self.ext_ids.len() * 2
            + self.flags.len()
            + exts
            + self.ref_order.len() * 4) as u64
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
        self.mtimes.push(clamp_mtime(mtime));
        self.ext_ids.push(ext_id);
        self.flags.push(flags_for(attributes, name));
        self.live_count = self.live_count.saturating_add(1);
        // Appended here; `rebuild_refs` sorts the bulk build, and `upsert`
        // re-inserts a single new slot in order.
        self.ref_order.push(idx);
        idx
    }

    /// Pre-sizes every parallel array for a bulk build so no `Vec` has to grow
    /// geometrically and no `shrink_to_fit` copy is needed afterwards.
    pub(crate) fn reserve_entries(&mut self, count: usize, name_bytes: usize) {
        self.name_offsets.reserve_exact(count);
        self.name_lens.reserve_exact(count);
        self.parents.reserve_exact(count);
        self.file_refs.reserve_exact(count);
        self.sizes.reserve_exact(count);
        self.mtimes.reserve_exact(count);
        self.ext_ids.reserve_exact(count);
        self.flags.reserve_exact(count);
        self.names.reserve_exact(name_bytes);
        self.ref_order.reserve_exact(count);
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

    /// Rebuilds the full path into `out`, reusing its allocation.
    pub(crate) fn path_into(&self, idx: u32, out: &mut String) {
        out.clear();
        out.push_str(&self.volume);
        out.push_str(":\\");
        // The common case fits in a fixed stack array; deeper trees fall back
        // to a small overflow vector.
        let mut chain = [u32::MAX; 96];
        let mut len = 0usize;
        let mut overflow: Vec<u32> = Vec::new();
        let mut current = idx;
        let mut depth = 0u32;
        loop {
            let name = self.name(current);
            if name.is_empty() {
                break;
            }
            if len < chain.len() {
                chain[len] = current;
                len += 1;
            } else {
                overflow.push(current);
            }
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
        let mut first = true;
        for slot in overflow.iter().rev() {
            if !first {
                out.push('\\');
            }
            first = false;
            out.push_str(self.name(*slot));
        }
        for slot in chain[..len].iter().rev() {
            if !first {
                out.push('\\');
            }
            first = false;
            out.push_str(self.name(*slot));
        }
    }

    pub(crate) fn path_of(&self, idx: u32) -> String {
        let mut path = String::new();
        self.path_into(idx, &mut path);
        path
    }

    /// Marks an entry and everything below it as deleted. Several roots can be
    /// batched so a bulk delete costs one pass instead of one per root.
    pub(crate) fn mark_many_deleted(&mut self, roots: &[u32]) {
        if roots.is_empty() {
            return;
        }
        // 0 unknown / 1 alive / 2 deleted. Resolving every slot once with an
        // ancestor walk is O(n) and never rescans the table.
        let mut memo = vec![0u8; self.flags.len()];
        for &root in roots {
            let slot = root as usize;
            if slot >= self.flags.len() {
                continue;
            }
            if self.flags[slot] & FLAG_DELETED != 0 {
                memo[slot] = 2;
                continue;
            }
            self.flags[slot] |= FLAG_DELETED;
            self.live_count = self.live_count.saturating_sub(1);
            memo[slot] = 2;
        }
        let mut stack: Vec<u32> = Vec::new();
        for position in 1..self.flags.len() {
            if memo[position] != 0 {
                continue;
            }
            if self.flags[position] & FLAG_DELETED != 0 {
                memo[position] = 2;
                continue;
            }
            stack.clear();
            let mut current = position as u32;
            let resolved = loop {
                let slot = current as usize;
                if memo[slot] != 0 {
                    break memo[slot];
                }
                if self.flags[slot] & FLAG_DELETED != 0 {
                    break 2;
                }
                stack.push(current);
                let parent = self.parents[slot];
                if parent == u32::MAX || parent as usize == slot || parent as usize >= self.flags.len()
                {
                    break 1;
                }
                current = parent;
            };
            for &node in &stack {
                memo[node as usize] = resolved;
            }
        }
        for position in 1..self.flags.len() {
            if memo[position] == 2 && self.flags[position] & FLAG_DELETED == 0 {
                self.flags[position] |= FLAG_DELETED;
                self.live_count = self.live_count.saturating_sub(1);
            }
        }
    }

    #[cfg(test)]
    pub(crate) fn mark_deleted(&mut self, idx: u32) {
        self.mark_many_deleted(&[idx]);
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
            let was_deleted = self.flags[slot] & FLAG_DELETED != 0;
            self.set_name(idx, name);
            self.parents[slot] = parent;
            self.sizes[slot] = size;
            self.mtimes[slot] = clamp_mtime(mtime);
            self.ext_ids[slot] = self.intern_ext(name);
            self.flags[slot] = flags_for(attributes, name);
            if was_deleted {
                self.live_count = self.live_count.saturating_add(1);
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

    /// Rebuilds `ref_order` with a 4-pass LSD radix sort over the 64-bit file
    /// references (O(n), no comparison sort).
    pub(crate) fn rebuild_refs(&mut self) {
        let count = self.file_refs.len();
        let mut order: Vec<u32> = (0..count as u32).collect();
        if count < 2 {
            self.ref_order = order;
            return;
        }
        let mut buffer = vec![0u32; count];
        for shift in (0..64).step_by(16) {
            let mut counts = vec![0usize; 1 << 16];
            for &slot in &order {
                let digit = ((self.file_refs[slot as usize] >> shift) & 0xFFFF) as usize;
                counts[digit] += 1;
            }
            let mut sum = 0usize;
            for count in counts.iter_mut() {
                let value = *count;
                *count = sum;
                sum += value;
            }
            for &slot in &order {
                let digit = ((self.file_refs[slot as usize] >> shift) & 0xFFFF) as usize;
                buffer[counts[digit]] = slot;
                counts[digit] += 1;
            }
            std::mem::swap(&mut order, &mut buffer);
        }
        self.ref_order = order;
    }
}

fn clamp_mtime(value: i64) -> u32 {
    value.clamp(0, u32::MAX as i64) as u32
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
        let mut reuse = String::from("stale");
        index.path_into(file, &mut reuse);
        assert_eq!(reuse, "C:\\Users\\notes.txt");
        assert_eq!(index.ext(file), "txt");
        assert!(index.is_live(file));
        assert_eq!(index.live_count(), index.len() as u64);
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
        let before = index.live_count();
        index.mark_deleted(folder);
        assert!(!index.is_live(folder));
        assert!(!index.is_live(file));
        assert!(index.is_live(users));
        assert_eq!(index.live_count(), before - 2);
    }

    #[test]
    fn batch_delete_marks_every_subtree_once() {
        let mut index = Index::new("C");
        let a = index.push_entry(10, 0, "a", ATTRIBUTE_DIRECTORY, 0, 0);
        let a_file = index.push_entry(11, a, "x.txt", 0, 1, 0);
        let b = index.push_entry(12, 0, "b", ATTRIBUTE_DIRECTORY, 0, 0);
        let b_file = index.push_entry(13, b, "y.txt", 0, 1, 0);
        index.set_parent(a, 0);
        index.set_parent(b, 0);
        index.set_parent(a_file, a);
        index.set_parent(b_file, b);
        let before = index.live_count();
        index.mark_many_deleted(&[a, b]);
        assert!(!index.is_live(a));
        assert!(!index.is_live(a_file));
        assert!(!index.is_live(b));
        assert!(!index.is_live(b_file));
        assert_eq!(index.live_count(), before - 4);
        // Applying the same roots again must not double-count.
        index.mark_many_deleted(&[a, b]);
        assert_eq!(index.live_count(), before - 4);
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

    #[test]
    fn upsert_revives_a_deleted_slot() {
        let mut index = Index::new("C");
        let idx = index.push_entry(20, 0, "gone.log", 0, 5, 0);
        index.mark_deleted(idx);
        assert!(!index.is_live(idx));
        index.upsert(20, 0, "back.log", 0, 7, 10);
        assert!(index.is_live(idx));
        assert_eq!(index.name(idx), "back.log");
    }

    #[test]
    fn rebuild_refs_sorts_by_file_reference() {
        let mut index = Index::new("C");
        index.push_entry(90, 0, "c", 0, 0, 0);
        index.push_entry(10, 0, "a", 0, 0, 0);
        index.push_entry(50, 0, "b", 0, 0, 0);
        index.rebuild_refs();
        let refs: Vec<u64> = index
            .ref_order
            .iter()
            .map(|slot| index.file_refs[*slot as usize])
            .collect();
        assert_eq!(refs, vec![5, 10, 50, 90]);
        assert_eq!(index.lookup_ref(50).map(|idx| index.name(idx)), Some("b"));
    }
}
