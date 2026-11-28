//! Snapshot persistence (format v3): one flat binary file per volume written
//! with streaming IO and an xxh3 checksum over everything after the header.
//!
//! Also owns the append-only USN delta log (`index-<vol>.delta`) and its tiny
//! commit marker (`index-<vol>.state`) used by startup maintenance.

use std::fs::{self, File, OpenOptions};
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::os::windows::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::sync::OnceLock;
use std::time::Instant;

use memmap2::Mmap;
use tauri::{AppHandle, Manager};
use xxhash_rust::xxh3::{self, Xxh3};

use super::model::Index;

const MAGIC: &[u8; 8] = b"NCDUIDX2";
const VERSION: u32 = 3;
const HEADER_LEN: usize = 256;
const CHECKSUM_OFFSET: usize = 168;

const DELTA_MAGIC: &[u8; 8] = b"NCDUDLT1";
const DELTA_VERSION: u32 = 1;
const STATE_MAGIC: &[u8; 8] = b"NCDUSTA1";
const STATE_VERSION: u32 = 1;
const STATE_LEN: usize = 128;

/// Held for the lifetime of the process so a second instance never writes the
/// same snapshot. The file itself stays on disk; the exclusive share mode is
/// what serialises writers.
static WRITER_LOCK: OnceLock<Option<File>> = OnceLock::new();
/// Serialises snapshot/delta temp-file replacement so two builds or a build
/// and the watcher never write the same `.tmp` path at once.
static SAVE_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn save_guard() -> std::sync::MutexGuard<'static, ()> {
    SAVE_LOCK.lock().unwrap_or_else(|err| err.into_inner())
}

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

fn delta_path_for(app: &AppHandle, volume: &str) -> Result<PathBuf, String> {
    let dir = config_dir(app)?;
    Ok(dir.join(format!("index-{volume}.delta")))
}

fn state_path_for(app: &AppHandle, volume: &str) -> Result<PathBuf, String> {
    let dir = config_dir(app)?;
    Ok(dir.join(format!("index-{volume}.state")))
}

/// Snapshot metadata read from the first 256 bytes (no decode, no checksum).
#[derive(Debug, Clone)]
pub(crate) struct IndexHeader {
    pub(crate) volume: String,
    pub(crate) count: u32,
    pub(crate) live_count: u32,
    pub(crate) truncated: u64,
    pub(crate) volume_serial: u64,
    pub(crate) journal_id: u64,
    pub(crate) next_usn: i64,
    pub(crate) built_at: i64,
    pub(crate) index_version: u64,
}

impl IndexHeader {
    pub(crate) fn engine(&self) -> &'static str {
        if self.journal_id != 0 {
            "mft"
        } else {
            "walk"
        }
    }
}

#[derive(Default)]
struct SectionOffsets {
    name_offsets: u64,
    name_lens: u64,
    parents: u64,
    file_refs: u64,
    sizes: u64,
    mtimes: u64,
    ext_ids: u64,
    flags: u64,
    names: u64,
    exts: u64,
}

struct HeaderData {
    volume_len: u32,
    count: u32,
    live_count: u32,
    truncated: u32,
    ext_count: u32,
    volume_serial: u64,
    journal_id: u64,
    next_usn: i64,
    built_at: i64,
    index_version: u64,
    names_len: u64,
    offsets: SectionOffsets,
    payload_xxh3: u64,
}

fn put(haystack: &mut [u8], offset: usize, value: &[u8]) {
    haystack[offset..offset + value.len()].copy_from_slice(value);
}

fn get_u32(haystack: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(haystack[offset..offset + 4].try_into().unwrap())
}

fn get_u64(haystack: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(haystack[offset..offset + 8].try_into().unwrap())
}

fn get_i64(haystack: &[u8], offset: usize) -> i64 {
    i64::from_le_bytes(haystack[offset..offset + 8].try_into().unwrap())
}

fn align8(value: usize) -> usize {
    (value + 7) & !7
}

fn encode_exts(exts: &[String]) -> Vec<u8> {
    let mut out = Vec::with_capacity(exts.iter().map(|ext| ext.len() + 2).sum());
    for extension in exts {
        let length = extension.len().min(u16::MAX as usize) as u16;
        out.extend_from_slice(&length.to_le_bytes());
        out.extend_from_slice(&extension.as_bytes()[..length as usize]);
    }
    out
}

fn build_header(
    index: &Index,
    volume_len: usize,
    ext_count: usize,
    names_len: usize,
    offsets: &SectionOffsets,
) -> [u8; HEADER_LEN] {
    let mut header = [0u8; HEADER_LEN];
    put(&mut header, 0, MAGIC);
    put(&mut header, 8, &VERSION.to_le_bytes());
    put(&mut header, 12, &(HEADER_LEN as u32).to_le_bytes());
    put(&mut header, 16, &(volume_len as u32).to_le_bytes());
    put(&mut header, 20, &(index.len() as u32).to_le_bytes());
    put(&mut header, 24, &index.live_count.to_le_bytes());
    put(
        &mut header,
        28,
        &(index.truncated_entries.min(u32::MAX as u64) as u32).to_le_bytes(),
    );
    put(&mut header, 32, &(ext_count as u32).to_le_bytes());
    put(&mut header, 40, &index.volume_serial.to_le_bytes());
    put(&mut header, 48, &index.journal_id.to_le_bytes());
    put(&mut header, 56, &index.next_usn.to_le_bytes());
    put(&mut header, 64, &index.built_at.to_le_bytes());
    put(&mut header, 72, &index.version.to_le_bytes());
    put(&mut header, 80, &(names_len as u64).to_le_bytes());
    put(&mut header, 88, &offsets.name_offsets.to_le_bytes());
    put(&mut header, 96, &offsets.name_lens.to_le_bytes());
    put(&mut header, 104, &offsets.parents.to_le_bytes());
    put(&mut header, 112, &offsets.file_refs.to_le_bytes());
    put(&mut header, 120, &offsets.sizes.to_le_bytes());
    put(&mut header, 128, &offsets.mtimes.to_le_bytes());
    put(&mut header, 136, &offsets.ext_ids.to_le_bytes());
    put(&mut header, 144, &offsets.flags.to_le_bytes());
    put(&mut header, 152, &offsets.names.to_le_bytes());
    put(&mut header, 160, &offsets.exts.to_le_bytes());
    header
}

fn write_hashed<W: Write>(writer: &mut W, hasher: &mut Xxh3, bytes: &[u8]) -> Result<(), String> {
    writer
        .write_all(bytes)
        .map_err(|err| format!("cannot write snapshot: {err}"))?;
    hasher.update(bytes);
    Ok(())
}

fn pad_to<W: Write>(
    writer: &mut W,
    hasher: &mut Xxh3,
    written: &mut usize,
    target: u64,
) -> Result<(), String> {
    const ZEROS: [u8; 8] = [0u8; 8];
    while (*written as u64) < target {
        let remaining = target as usize - *written;
        let chunk = remaining.min(ZEROS.len());
        write_hashed(writer, hasher, &ZEROS[..chunk])?;
        *written += chunk;
    }
    Ok(())
}

fn write_snapshot<W: Write + Seek>(writer: &mut W, index: &Index) -> Result<(), String> {
    let count = index.len();
    let volume = index.volume.as_bytes();
    let names = index.names.as_bytes();
    let ext_bytes = encode_exts(&index.exts);
    let mut offsets = SectionOffsets::default();
    let mut cursor = align8(HEADER_LEN + volume.len());
    let mut reserve = |size: usize| -> u64 {
        let start = cursor;
        cursor = align8(cursor + size);
        start as u64
    };
    offsets.name_offsets = reserve(count * 4);
    offsets.name_lens = reserve(count * 2);
    offsets.parents = reserve(count * 4);
    offsets.file_refs = reserve(count * 8);
    offsets.sizes = reserve(count * 8);
    offsets.mtimes = reserve(count * 4);
    offsets.ext_ids = reserve(count * 2);
    offsets.flags = reserve(count);
    offsets.names = reserve(names.len());
    offsets.exts = reserve(ext_bytes.len());

    let header = build_header(index, volume.len(), index.exts.len(), names.len(), &offsets);
    writer
        .write_all(&header)
        .map_err(|err| format!("cannot write snapshot header: {err}"))?;
    let mut hasher = Xxh3::new();
    let mut written = HEADER_LEN;
    write_hashed(writer, &mut hasher, volume)?;
    written += volume.len();
    pad_to(writer, &mut hasher, &mut written, offsets.name_offsets)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u32, u8>(&index.name_offsets),
    )?;
    written += count * 4;
    pad_to(writer, &mut hasher, &mut written, offsets.name_lens)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u16, u8>(&index.name_lens),
    )?;
    written += count * 2;
    pad_to(writer, &mut hasher, &mut written, offsets.parents)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u32, u8>(&index.parents),
    )?;
    written += count * 4;
    pad_to(writer, &mut hasher, &mut written, offsets.file_refs)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u64, u8>(&index.file_refs),
    )?;
    written += count * 8;
    pad_to(writer, &mut hasher, &mut written, offsets.sizes)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u64, u8>(&index.sizes),
    )?;
    written += count * 8;
    pad_to(writer, &mut hasher, &mut written, offsets.mtimes)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u32, u8>(&index.mtimes),
    )?;
    written += count * 4;
    pad_to(writer, &mut hasher, &mut written, offsets.ext_ids)?;
    write_hashed(
        writer,
        &mut hasher,
        bytemuck::cast_slice::<u16, u8>(&index.ext_ids),
    )?;
    written += count * 2;
    pad_to(writer, &mut hasher, &mut written, offsets.flags)?;
    write_hashed(writer, &mut hasher, &index.flags)?;
    written += count;
    pad_to(writer, &mut hasher, &mut written, offsets.names)?;
    write_hashed(writer, &mut hasher, names)?;
    written += names.len();
    pad_to(writer, &mut hasher, &mut written, offsets.exts)?;
    write_hashed(writer, &mut hasher, &ext_bytes)?;

    writer
        .flush()
        .map_err(|err| format!("cannot flush snapshot: {err}"))?;
    writer
        .seek(SeekFrom::Start(CHECKSUM_OFFSET as u64))
        .map_err(|err| format!("cannot seek snapshot: {err}"))?;
    writer
        .write_all(&hasher.digest().to_le_bytes())
        .map_err(|err| format!("cannot write snapshot checksum: {err}"))?;
    writer
        .flush()
        .map_err(|err| format!("cannot flush snapshot: {err}"))?;
    Ok(())
}

pub(crate) fn save(app: &AppHandle, index: &Index) -> Result<(), String> {
    let _guard = save_guard();
    let path = path_for(app, &index.volume)?;
    let temporary = path.with_extension("idx.tmp");
    {
        let file = File::create(&temporary)
            .map_err(|err| format!("cannot create {}: {err}", temporary.display()))?;
        let mut writer = BufWriter::with_capacity(4 * 1024 * 1024, file);
        write_snapshot(&mut writer, index)?;
        writer
            .into_inner()
            .map_err(|err| format!("cannot flush {}: {err}", temporary.display()))?;
    }
    fs::rename(&temporary, &path).map_err(|err| {
        let _ = fs::remove_file(&temporary);
        format!("cannot replace {}: {err}", path.display())
    })
}

/// Encodes to memory. Used by tests and the ignored benchmark only.
#[cfg(test)]
pub(crate) fn encode(index: &Index) -> Vec<u8> {
    let mut cursor =
        std::io::Cursor::new(Vec::with_capacity(index.memory_bytes() as usize + HEADER_LEN));
    write_snapshot(&mut cursor, index).expect("in-memory snapshot write");
    cursor.into_inner()
}

fn parse_header(bytes: &[u8]) -> Result<HeaderData, String> {
    if bytes.len() < HEADER_LEN {
        return Err("snapshot is too small".to_string());
    }
    if &bytes[0..8] != MAGIC {
        return Err("snapshot magic mismatch".to_string());
    }
    let version = get_u32(bytes, 8);
    if version != VERSION {
        return Err(format!("unsupported snapshot version {version}"));
    }
    if get_u32(bytes, 12) as usize != HEADER_LEN {
        return Err("unsupported snapshot header length".to_string());
    }
    Ok(HeaderData {
        volume_len: get_u32(bytes, 16),
        count: get_u32(bytes, 20),
        live_count: get_u32(bytes, 24),
        truncated: get_u32(bytes, 28),
        ext_count: get_u32(bytes, 32),
        volume_serial: get_u64(bytes, 40),
        journal_id: get_u64(bytes, 48),
        next_usn: get_i64(bytes, 56),
        built_at: get_i64(bytes, 64),
        index_version: get_u64(bytes, 72),
        names_len: get_u64(bytes, 80),
        offsets: SectionOffsets {
            name_offsets: get_u64(bytes, 88),
            name_lens: get_u64(bytes, 96),
            parents: get_u64(bytes, 104),
            file_refs: get_u64(bytes, 112),
            sizes: get_u64(bytes, 120),
            mtimes: get_u64(bytes, 128),
            ext_ids: get_u64(bytes, 136),
            flags: get_u64(bytes, 144),
            names: get_u64(bytes, 152),
            exts: get_u64(bytes, 160),
        },
        payload_xxh3: get_u64(bytes, CHECKSUM_OFFSET),
    })
}

fn section<T: bytemuck::Pod>(bytes: &[u8], offset: u64, count: usize) -> Result<Vec<T>, String> {
    let start = offset as usize;
    let size = std::mem::size_of::<T>()
        .checked_mul(count)
        .ok_or_else(|| "snapshot section is too large".to_string())?;
    let end = start
        .checked_add(size)
        .ok_or_else(|| "snapshot section is too large".to_string())?;
    let data = bytes
        .get(start..end)
        .ok_or_else(|| "snapshot section is truncated".to_string())?;
    match bytemuck::try_cast_slice::<u8, T>(data) {
        Ok(values) => Ok(values.to_vec()),
        Err(_) => {
            let size = std::mem::size_of::<T>();
            let mut values = Vec::with_capacity(count);
            for chunk in data.chunks_exact(size) {
                values.push(bytemuck::pod_read_unaligned::<T>(chunk));
            }
            Ok(values)
        }
    }
}

fn decode_bytes(bytes: &[u8]) -> Result<Index, String> {
    let header = parse_header(bytes)?;
    let payload = &bytes[HEADER_LEN..];
    let digest = xxh3::xxh3_64(payload);
    if digest != header.payload_xxh3 {
        return Err("snapshot checksum mismatch".to_string());
    }
    let volume_start = HEADER_LEN;
    let volume_end = volume_start + header.volume_len as usize;
    let volume = String::from_utf8(
        bytes
            .get(volume_start..volume_end)
            .ok_or_else(|| "snapshot volume is truncated".to_string())?
            .to_vec(),
    )
    .map_err(|_| "snapshot volume is not UTF-8".to_string())?;
    let count = header.count as usize;
    let name_offsets = section::<u32>(bytes, header.offsets.name_offsets, count)?;
    let name_lens = section::<u16>(bytes, header.offsets.name_lens, count)?;
    let parents = section::<u32>(bytes, header.offsets.parents, count)?;
    let file_refs = section::<u64>(bytes, header.offsets.file_refs, count)?;
    let sizes = section::<u64>(bytes, header.offsets.sizes, count)?;
    let mtimes = section::<u32>(bytes, header.offsets.mtimes, count)?;
    let ext_ids = section::<u16>(bytes, header.offsets.ext_ids, count)?;
    let flags = bytes
        .get(header.offsets.flags as usize..header.offsets.flags as usize + count)
        .ok_or_else(|| "snapshot flags are truncated".to_string())?
        .to_vec();
    let names_start = header.offsets.names as usize;
    let names = String::from_utf8(
        bytes
            .get(names_start..names_start + header.names_len as usize)
            .ok_or_else(|| "snapshot name pool is truncated".to_string())?
            .to_vec(),
    )
    .map_err(|_| "snapshot name pool is not UTF-8".to_string())?;
    let mut position = header.offsets.exts as usize;
    let mut exts = Vec::with_capacity(header.ext_count as usize);
    for _ in 0..header.ext_count {
        let length = bytes
            .get(position..position + 2)
            .ok_or_else(|| "snapshot extension list is truncated".to_string())?;
        let length = u16::from_le_bytes(length.try_into().unwrap()) as usize;
        position += 2;
        let text = bytes
            .get(position..position + length)
            .ok_or_else(|| "snapshot extension is truncated".to_string())?;
        exts.push(
            String::from_utf8(text.to_vec())
                .map_err(|_| "snapshot extension is not UTF-8".to_string())?,
        );
        position += length;
    }
    if exts.is_empty() {
        exts.push(String::new());
    }
    for (offset, length) in name_offsets.iter().zip(&name_lens) {
        if (*offset as usize).saturating_add(*length as usize) > names.len() {
            return Err("snapshot name pool is inconsistent".to_string());
        }
    }

    let mut index = Index {
        volume,
        volume_serial: header.volume_serial,
        journal_id: header.journal_id,
        next_usn: header.next_usn,
        built_at: header.built_at,
        version: header.index_version,
        truncated_entries: header.truncated as u64,
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
        live_count: header.live_count,
        ref_order: Vec::new(),
    };
    index.rebuild_refs();
    Ok(index)
}

#[cfg(test)]
pub(crate) fn decode(bytes: &[u8]) -> Result<Index, String> {
    decode_bytes(bytes)
}

pub(crate) fn load(app: &AppHandle, volume: &str) -> Result<Index, String> {
    let path = path_for(app, volume)?;
    let started = Instant::now();
    let file =
        File::open(&path).map_err(|err| format!("cannot read {}: {err}", path.display()))?;
    let map = unsafe { Mmap::map(&file) }
        .map_err(|err| format!("cannot map {}: {err}", path.display()))?;
    let index = decode_bytes(&map)?;
    log::info!(
        "index snapshot loaded for volume {volume} in {} ms",
        started.elapsed().as_millis()
    );
    Ok(index)
}

/// Reads only the fixed header plus the volume string. Used at startup so a
/// valid snapshot costs one ~256B read instead of a full decode.
pub(crate) fn read_header(app: &AppHandle, volume: &str) -> Option<IndexHeader> {
    let path = path_for(app, volume).ok()?;
    let mut file = File::open(path).ok()?;
    let mut header = [0u8; HEADER_LEN];
    file.read_exact(&mut header).ok()?;
    let parsed = parse_header(&header).ok()?;
    if parsed.volume_len == 0 || parsed.volume_len > 8 {
        return None;
    }
    let mut volume_bytes = vec![0u8; parsed.volume_len as usize];
    file.read_exact(&mut volume_bytes).ok()?;
    Some(IndexHeader {
        volume: String::from_utf8(volume_bytes).ok()?,
        count: parsed.count,
        live_count: parsed.live_count,
        truncated: parsed.truncated as u64,
        volume_serial: parsed.volume_serial,
        journal_id: parsed.journal_id,
        next_usn: parsed.next_usn,
        built_at: parsed.built_at,
        index_version: parsed.index_version,
    })
}

pub(crate) fn remove(app: &AppHandle, volume: &str) {
    if let Ok(path) = path_for(app, volume) {
        if path.exists() {
            let _ = fs::remove_file(&path);
        }
    }
    reset_delta(app, volume);
}

// ---------------------------------------------------------------------------
// Append-only delta log
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DeltaKind {
    Upsert = 1,
    Delete = 2,
}

#[derive(Debug, Clone)]
pub(crate) struct DeltaRecord {
    pub(crate) kind: DeltaKind,
    pub(crate) hidden: bool,
    pub(crate) is_dir: bool,
    pub(crate) file_ref: u64,
    pub(crate) parent_ref: u64,
    pub(crate) size: u64,
    pub(crate) mtime: i64,
    pub(crate) name: String,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct DeltaState {
    pub(crate) volume_serial: u64,
    pub(crate) journal_id: u64,
    pub(crate) base_index_version: u64,
    pub(crate) tail_usn: i64,
    pub(crate) delta_len: u64,
}

fn delta_header(volume: &str, volume_serial: u64, journal_id: u64, base: u64) -> Vec<u8> {
    let bytes = volume.as_bytes();
    let mut out = Vec::with_capacity(8 + 8 + bytes.len() + 24);
    out.extend_from_slice(DELTA_MAGIC);
    out.extend_from_slice(&DELTA_VERSION.to_le_bytes());
    out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(bytes);
    out.extend_from_slice(&volume_serial.to_le_bytes());
    out.extend_from_slice(&journal_id.to_le_bytes());
    out.extend_from_slice(&base.to_le_bytes());
    out
}

fn encode_record(record: &DeltaRecord) -> Option<Vec<u8>> {
    if record.name.len() > u32::MAX as usize {
        return None;
    }
    let mut out = Vec::with_capacity(40 + record.name.len());
    out.push(record.kind as u8);
    // bit0 hidden, bit1 directory: enough to restore the index flags.
    let flags = (if record.hidden { 1u8 } else { 0 })
        | (if record.is_dir { 2u8 } else { 0 });
    out.push(flags);
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&(record.name.len() as u32).to_le_bytes());
    out.extend_from_slice(&record.file_ref.to_le_bytes());
    out.extend_from_slice(&record.parent_ref.to_le_bytes());
    out.extend_from_slice(&record.size.to_le_bytes());
    out.extend_from_slice(&record.mtime.to_le_bytes());
    out.extend_from_slice(record.name.as_bytes());
    Some(out)
}

pub(crate) fn read_delta_state(app: &AppHandle, volume: &str) -> Option<DeltaState> {
    let path = state_path_for(app, volume).ok()?;
    let bytes = fs::read(path).ok()?;
    if bytes.len() < STATE_LEN || &bytes[0..8] != STATE_MAGIC {
        return None;
    }
    if get_u32(&bytes, 8) != STATE_VERSION {
        return None;
    }
    Some(DeltaState {
        volume_serial: get_u64(&bytes, 16),
        journal_id: get_u64(&bytes, 24),
        base_index_version: get_u64(&bytes, 32),
        tail_usn: get_i64(&bytes, 40),
        delta_len: get_u64(&bytes, 48),
    })
}

/// USN position actually covered by the snapshot plus its committed delta log.
/// Falls back to the snapshot's own position when the delta is missing or was
/// written against a different base.
pub(crate) fn effective_next_usn(
    app: &AppHandle,
    volume: &str,
    volume_serial: u64,
    journal_id: u64,
    base_index_version: u64,
    snapshot_next_usn: i64,
) -> i64 {
    match read_delta_state(app, volume) {
        Some(state)
            if state.volume_serial == volume_serial
                && state.journal_id == journal_id
                && state.base_index_version == base_index_version =>
        {
            state.tail_usn
        }
        _ => snapshot_next_usn,
    }
}

fn write_delta_state(app: &AppHandle, volume: &str, state: &DeltaState) -> Result<(), String> {
    let path = state_path_for(app, volume)?;
    let temporary = path.with_extension("state.tmp");
    let mut bytes = [0u8; STATE_LEN];
    put(&mut bytes, 0, STATE_MAGIC);
    put(&mut bytes, 8, &STATE_VERSION.to_le_bytes());
    put(&mut bytes, 16, &state.volume_serial.to_le_bytes());
    put(&mut bytes, 24, &state.journal_id.to_le_bytes());
    put(&mut bytes, 32, &state.base_index_version.to_le_bytes());
    put(&mut bytes, 40, &state.tail_usn.to_le_bytes());
    put(&mut bytes, 48, &state.delta_len.to_le_bytes());
    put(
        &mut bytes,
        56,
        &(std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|value| value.as_secs() as i64)
            .unwrap_or(0))
        .to_le_bytes(),
    );
    fs::write(&temporary, bytes)
        .map_err(|err| format!("cannot write {}: {err}", temporary.display()))?;
    fs::rename(&temporary, &path).map_err(|err| {
        let _ = fs::remove_file(&temporary);
        format!("cannot replace {}: {err}", path.display())
    })
}

pub(crate) fn reset_delta(app: &AppHandle, volume: &str) {
    let _guard = save_guard();
    if let Ok(path) = delta_path_for(app, volume) {
        let _ = fs::remove_file(path);
    }
    if let Ok(path) = state_path_for(app, volume) {
        let _ = fs::remove_file(path);
    }
}

/// Appends `records` to the delta log and commits the new tail atomically.
/// Does nothing when there are no changes.
pub(crate) fn append_delta(
    app: &AppHandle,
    volume: &str,
    volume_serial: u64,
    journal_id: u64,
    base_index_version: u64,
    records: &[DeltaRecord],
    tail_usn: i64,
) -> Result<(), String> {
    let _guard = save_guard();
    let path = delta_path_for(app, volume)?;
    if records.is_empty() {
        // No index-relevant records, but the tail still advanced: commit the
        // position so startup/load does not replay the same range again.
        let delta_len = fs::metadata(&path).map(|meta| meta.len()).unwrap_or(0);
        return write_delta_state(
            app,
            volume,
            &DeltaState {
                volume_serial,
                journal_id,
                base_index_version,
                tail_usn,
                delta_len,
            },
        );
    }
    let header = delta_header(volume, volume_serial, journal_id, base_index_version);
    let mut file = OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .open(&path)
        .map_err(|err| format!("cannot open {}: {err}", path.display()))?;
    let mut length = file
        .metadata()
        .map_err(|err| format!("cannot stat {}: {err}", path.display()))?
        .len() as usize;
    // Drop any uncommitted tail from an interrupted append before adding more.
    if let Some(state) = read_delta_state(app, volume) {
        let committed = state.delta_len as usize;
        if committed < length {
            file.set_len(state.delta_len)
                .map_err(|err| format!("cannot truncate {}: {err}", path.display()))?;
            length = committed;
        }
    }
    if length == 0 || length < header.len() {
        file.set_len(0)
            .map_err(|err| format!("cannot reset {}: {err}", path.display()))?;
        file.write_all(&header)
            .map_err(|err| format!("cannot write {}: {err}", path.display()))?;
    } else {
        let mut existing = vec![0u8; header.len().min(length)];
        let mut reader = File::open(&path)
            .map_err(|err| format!("cannot read {}: {err}", path.display()))?;
        reader
            .read_exact(&mut existing)
            .map_err(|err| format!("cannot read {}: {err}", path.display()))?;
        if existing != header {
            // A different base: drop the stale log and start over.
            file.set_len(0)
                .map_err(|err| format!("cannot reset {}: {err}", path.display()))?;
            file.write_all(&header)
                .map_err(|err| format!("cannot write {}: {err}", path.display()))?;
        }
    }
    // Append after whatever header/base is already in the file.
    file.seek(SeekFrom::End(0))
        .map_err(|err| format!("cannot seek {}: {err}", path.display()))?;
    for record in records {
        if let Some(bytes) = encode_record(record) {
            file.write_all(&bytes)
                .map_err(|err| format!("cannot append {}: {err}", path.display()))?;
        }
    }
    file.flush()
        .map_err(|err| format!("cannot flush {}: {err}", path.display()))?;
    file.sync_data()
        .map_err(|err| format!("cannot sync {}: {err}", path.display()))?;
    let delta_len = file
        .metadata()
        .map_err(|err| format!("cannot stat {}: {err}", path.display()))?
        .len();
    write_delta_state(
        app,
        volume,
        &DeltaState {
            volume_serial,
            journal_id,
            base_index_version,
            tail_usn,
            delta_len,
        },
    )
}

/// Reads the committed part of the delta log. A record that does not parse
/// stops the replay without failing the load (conservative tail drop).
pub(crate) fn read_delta(app: &AppHandle, volume: &str, state: &DeltaState) -> Vec<DeltaRecord> {
    let Ok(path) = delta_path_for(app, volume) else {
        return Vec::new();
    };
    let Ok(bytes) = fs::read(path) else {
        return Vec::new();
    };
    let limit = (state.delta_len as usize).min(bytes.len());
    let bytes = &bytes[..limit];
    let header = delta_header(
        volume,
        state.volume_serial,
        state.journal_id,
        state.base_index_version,
    );
    if bytes.len() < header.len() || &bytes[..header.len()] != header.as_slice() {
        return Vec::new();
    }
    parse_delta_records(bytes, header.len())
}

fn parse_delta_records(bytes: &[u8], start: usize) -> Vec<DeltaRecord> {
    let mut position = start;
    let mut records = Vec::new();
    while position + 40 <= bytes.len() {
        let kind = match bytes[position] {
            1 => DeltaKind::Upsert,
            2 => DeltaKind::Delete,
            _ => break,
        };
        let hidden = bytes[position + 1] & 1 != 0;
        let is_dir = bytes[position + 1] & 2 != 0;
        let name_len = get_u32(bytes, position + 4) as usize;
        let record_end = position + 40 + name_len;
        if record_end > bytes.len() {
            break;
        }
        let file_ref = get_u64(bytes, position + 8);
        let parent_ref = get_u64(bytes, position + 16);
        let size = get_u64(bytes, position + 24);
        let mtime = get_i64(bytes, position + 32);
        let name = match String::from_utf8(bytes[position + 40..record_end].to_vec()) {
            Ok(name) => name,
            Err(_) => break,
        };
        records.push(DeltaRecord {
            kind,
            hidden,
            is_dir,
            file_ref,
            parent_ref,
            size,
            mtime,
            name,
        });
        position = record_end;
    }
    records
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
        assert_eq!(decoded.live_count(), 3);
        assert_eq!(decoded.lookup_ref(11), Some(2));
    }

    #[test]
    fn a_corrupted_checksum_is_rejected() {
        let mut bytes = encode(&sample());
        assert!(bytes.len() > HEADER_LEN + 8);
        bytes[HEADER_LEN + 4] ^= 0xFF;
        assert!(decode(&bytes).is_err());
    }

    #[test]
    fn an_unknown_version_is_rejected() {
        let mut bytes = encode(&sample());
        bytes[8..12].copy_from_slice(&99u32.to_le_bytes());
        assert!(decode(&bytes).is_err());
    }

    #[test]
    fn a_v2_snapshot_is_rejected() {
        let mut bytes = encode(&sample());
        bytes[0..8].copy_from_slice(b"NCDUIDX1");
        assert!(decode(&bytes).is_err());
    }

    fn delta_bytes(records: &[DeltaRecord]) -> Vec<u8> {
        let mut bytes = delta_header("C", 9, 7, 3);
        for record in records {
            bytes.extend_from_slice(&encode_record(record).unwrap());
        }
        bytes
    }

    fn delta_records() -> Vec<DeltaRecord> {
        vec![
            DeltaRecord {
                kind: DeltaKind::Upsert,
                hidden: false,
                is_dir: false,
                file_ref: 42,
                parent_ref: 5,
                size: 10,
                mtime: 7,
                name: "new.txt".to_string(),
            },
            DeltaRecord {
                kind: DeltaKind::Upsert,
                hidden: true,
                is_dir: true,
                file_ref: 44,
                parent_ref: 5,
                size: 0,
                mtime: 0,
                name: ".hidden".to_string(),
            },
            DeltaRecord {
                kind: DeltaKind::Delete,
                hidden: false,
                is_dir: false,
                file_ref: 43,
                parent_ref: 5,
                size: 0,
                mtime: 0,
                name: "old.txt".to_string(),
            },
        ]
    }

    #[test]
    fn delta_append_and_replay_roundtrip() {
        let records = delta_records();
        let bytes = delta_bytes(&records);
        let header_len = delta_header("C", 9, 7, 3).len();
        let parsed = parse_delta_records(&bytes, header_len);
        assert_eq!(parsed.len(), 3);
        assert_eq!(parsed[0].name, "new.txt");
        assert_eq!(parsed[1].kind, DeltaKind::Upsert);
        assert!(parsed[1].hidden);
        assert!(parsed[1].is_dir);
        assert_eq!(parsed[2].kind, DeltaKind::Delete);
    }

    #[test]
    fn delta_truncated_tail_is_discarded() {
        let records = delta_records();
        let bytes = delta_bytes(&records);
        let header_len = delta_header("C", 9, 7, 3).len();
        let first_len = encode_record(&records[0]).unwrap().len();
        // Cut in the middle of the second record: only the first survives.
        let truncated = &bytes[..header_len + first_len + 5];
        let parsed = parse_delta_records(truncated, header_len);
        assert_eq!(parsed.len(), 1);
        assert_eq!(parsed[0].name, "new.txt");
    }

    #[test]
    fn no_changes_means_no_records() {
        let bytes = delta_bytes(&[]);
        let header_len = delta_header("C", 9, 7, 3).len();
        assert!(parse_delta_records(&bytes, header_len).is_empty());
    }
}
