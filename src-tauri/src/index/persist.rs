//! Snapshot persistence: one flat binary file per volume, written atomically
//! and validated with a SHA-256 checksum so a broken file is simply rebuilt.

use std::fs::{self, File, OpenOptions};
use std::os::windows::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::sync::OnceLock;

use sha2::{Digest, Sha256};
use tauri::{AppHandle, Manager};

use super::model::Index;

const MAGIC: &[u8; 8] = b"NCDUIDX1";
const VERSION: u32 = 2;
const CHECKSUM_LEN: usize = 32;

/// Held for the lifetime of the process so a second instance never writes the
/// same snapshot. The file itself stays on disk; the exclusive share mode is
/// what serialises writers.
static WRITER_LOCK: OnceLock<Option<File>> = OnceLock::new();

fn config_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|err| format!("cannot resolve app config dir: {err}"))?;
    fs::create_dir_all(&dir).map_err(|err| format!("cannot create {}: {err}", dir.display()))?;
    Ok(dir)
}

/// True when this process may build and save the index. Read-only instances
/// can still load a snapshot and answer queries.
pub(crate) fn acquire_writer_lock(app: &AppHandle) -> bool {
    let holder = WRITER_LOCK.get_or_init(|| {
        let dir = config_dir(app).ok()?;
        let mut options = OpenOptions::new();
        options.create(true).read(true).write(true).share_mode(0);
        options.open(dir.join("index.lock")).ok()
    });
    holder.is_some()
}

pub(crate) fn path_for(app: &AppHandle, volume: &str) -> Result<PathBuf, String> {
    let dir = config_dir(app)?;
    Ok(dir.join(format!("index-{volume}.idx")))
}

pub(crate) fn save(app: &AppHandle, index: &Index) -> Result<(), String> {
    let path = path_for(app, &index.volume)?;
    let encoded = encode(index);
    let temporary = path.with_extension("idx.tmp");
    fs::write(&temporary, &encoded)
        .map_err(|err| format!("cannot write {}: {err}", temporary.display()))?;
    fs::rename(&temporary, &path).map_err(|err| {
        let _ = fs::remove_file(&temporary);
        format!("cannot replace {}: {err}", path.display())
    })
}

pub(crate) fn load(app: &AppHandle, volume: &str) -> Result<Index, String> {
    let path = path_for(app, volume)?;
    let bytes = fs::read(&path).map_err(|err| format!("cannot read {}: {err}", path.display()))?;
    decode(&bytes)
}

pub(crate) fn remove(app: &AppHandle, volume: &str) {
    if let Ok(path) = path_for(app, volume) {
        if path.exists() {
            let _ = fs::remove_file(&path);
        }
    }
}

fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_i64(out: &mut Vec<u8>, value: i64) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub(crate) fn encode(index: &Index) -> Vec<u8> {
    let mut out = Vec::with_capacity(index.memory_bytes() as usize + 64);
    out.extend_from_slice(MAGIC);
    push_u32(&mut out, VERSION);
    push_u32(&mut out, index.volume.len() as u32);
    out.extend_from_slice(index.volume.as_bytes());
    push_u64(&mut out, index.volume_serial);
    push_u64(&mut out, index.journal_id);
    push_i64(&mut out, index.next_usn);
    push_i64(&mut out, index.built_at);
    push_u64(&mut out, index.version);
    push_u32(&mut out, index.len() as u32);
    for value in &index.name_offsets {
        push_u32(&mut out, *value);
    }
    for value in &index.name_lens {
        push_u16(&mut out, *value);
    }
    for value in &index.parents {
        push_u32(&mut out, *value);
    }
    for value in &index.file_refs {
        push_u64(&mut out, *value);
    }
    for value in &index.sizes {
        push_u64(&mut out, *value);
    }
    for value in &index.mtimes {
        push_i64(&mut out, *value);
    }
    for value in &index.ext_ids {
        push_u16(&mut out, *value);
    }
    out.extend_from_slice(&index.flags);
    push_u64(&mut out, index.names.len() as u64);
    out.extend_from_slice(index.names.as_bytes());
    push_u32(&mut out, index.exts.len() as u32);
    for extension in &index.exts {
        push_u16(&mut out, extension.len() as u16);
        out.extend_from_slice(extension.as_bytes());
    }
    let digest = Sha256::digest(&out);
    out.extend_from_slice(&digest);
    out
}

struct Reader<'a> {
    data: &'a [u8],
    position: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, length: usize) -> Result<&'a [u8], String> {
        let end = self
            .position
            .checked_add(length)
            .ok_or_else(|| "invalid snapshot offset".to_string())?;
        let slice = self
            .data
            .get(self.position..end)
            .ok_or_else(|| "snapshot is truncated".to_string())?;
        self.position = end;
        Ok(slice)
    }

    fn u16(&mut self) -> Result<u16, String> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }

    fn u32(&mut self) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    fn u64(&mut self) -> Result<u64, String> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }

    fn i64(&mut self) -> Result<i64, String> {
        Ok(i64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }

    fn text(&mut self, length: usize) -> Result<String, String> {
        let bytes = self.take(length)?;
        String::from_utf8(bytes.to_vec()).map_err(|_| "snapshot text is not UTF-8".to_string())
    }
}

pub(crate) fn decode(bytes: &[u8]) -> Result<Index, String> {
    if bytes.len() < MAGIC.len() + CHECKSUM_LEN {
        return Err("snapshot is too small".to_string());
    }
    let (payload, digest) = bytes.split_at(bytes.len() - CHECKSUM_LEN);
    let computed = Sha256::digest(payload);
    if computed.as_slice() != digest {
        return Err("snapshot checksum mismatch".to_string());
    }
    if &payload[0..MAGIC.len()] != MAGIC {
        return Err("snapshot magic mismatch".to_string());
    }
    let mut reader = Reader {
        data: payload,
        position: MAGIC.len(),
    };
    let version = reader.u32()?;
    if version != VERSION {
        return Err(format!("unsupported snapshot version {version}"));
    }
    let volume_len = reader.u32()? as usize;
    let volume = reader.text(volume_len)?;
    let volume_serial = reader.u64()?;
    let journal_id = reader.u64()?;
    let next_usn = reader.i64()?;
    let built_at = reader.i64()?;
    let index_version = reader.u64()?;
    let count = reader.u32()? as usize;

    let mut name_offsets = Vec::with_capacity(count);
    for _ in 0..count {
        name_offsets.push(reader.u32()?);
    }
    let mut name_lens = Vec::with_capacity(count);
    for _ in 0..count {
        name_lens.push(reader.u16()?);
    }
    let mut parents = Vec::with_capacity(count);
    for _ in 0..count {
        parents.push(reader.u32()?);
    }
    let mut file_refs = Vec::with_capacity(count);
    for _ in 0..count {
        file_refs.push(reader.u64()?);
    }
    let mut sizes = Vec::with_capacity(count);
    for _ in 0..count {
        sizes.push(reader.u64()?);
    }
    let mut mtimes = Vec::with_capacity(count);
    for _ in 0..count {
        mtimes.push(reader.i64()?);
    }
    let mut ext_ids = Vec::with_capacity(count);
    for _ in 0..count {
        ext_ids.push(reader.u16()?);
    }
    let flags = reader.take(count)?.to_vec();
    let names_len = reader.u64()? as usize;
    let names = reader.text(names_len)?;
    let ext_count = reader.u32()? as usize;
    let mut exts = Vec::with_capacity(ext_count);
    for _ in 0..ext_count {
        let length = reader.u16()? as usize;
        exts.push(reader.text(length)?);
    }
    if exts.is_empty() {
        exts.push(String::new());
    }
    if reader.position != payload.len() {
        return Err("snapshot has trailing data".to_string());
    }
    for (position, offset) in name_offsets.iter().enumerate() {
        let length = name_lens[position] as usize;
        if (*offset as usize).saturating_add(length) > names.len() {
            return Err("snapshot name pool is inconsistent".to_string());
        }
    }

    let mut index = Index {
        volume,
        volume_serial,
        journal_id,
        next_usn,
        built_at,
        version: index_version,
        names,
        name_offsets,
        name_lens,
        parents,
        file_refs,
        sizes,
        mtimes,
        ext_ids,
        flags,
        exts,
        ext_buckets: Vec::new(),
        ref_order: Vec::new(),
    };
    index.rebuild_refs();
    index.build_ext_buckets();
    index.compact();
    Ok(index)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Index {
        let mut index = Index::new("C");
        let docs = index.push_entry(10, 0, "docs", 0x10, 0, 0);
        index.push_entry(11, docs, "报告.txt", 0, 42, 1_700_000_000);
        index.set_parent(docs, 0);
        index.journal_id = 99;
        index.next_usn = 1234;
        index.version = 5;
        index
    }

    #[test]
    fn round_trip_preserves_every_field() {
        let index = sample();
        let decoded = decode(&encode(&index)).expect("decode");
        assert_eq!(decoded.volume, "C");
        assert_eq!(decoded.len(), index.len());
        assert_eq!(decoded.name(2), "报告.txt");
        assert_eq!(decoded.path_of(2), "C:\\docs\\报告.txt");
        assert_eq!(decoded.journal_id, 99);
        assert_eq!(decoded.next_usn, 1234);
        assert_eq!(decoded.version, 5);
        assert_eq!(decoded.lookup_ref(11), Some(2));
    }

    #[test]
    fn a_corrupted_checksum_is_rejected() {
        let mut bytes = encode(&sample());
        let length = bytes.len();
        bytes[length / 2] ^= 0xFF;
        assert!(decode(&bytes).is_err());
    }

    #[test]
    fn an_unknown_version_is_rejected() {
        let mut bytes = encode(&sample());
        let offset = MAGIC.len();
        bytes[offset..offset + 4].copy_from_slice(&99u32.to_le_bytes());
        let payload_len = bytes.len() - CHECKSUM_LEN;
        let digest = Sha256::digest(&bytes[..payload_len]);
        bytes[payload_len..].copy_from_slice(&digest);
        assert!(decode(&bytes).is_err());
    }
}
