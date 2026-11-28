//! NTFS $MFT fast scan, mirroring the Qt build's MftScanner: enumerate USN
//! records for names and parent references, then read the raw $MFT for sizes.

use std::collections::HashMap;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, GENERIC_READ, HANDLE, ERROR_HANDLE_EOF,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, GetVolumeInformationW, ReadFile, SetFilePointerEx, FILE_FLAG_BACKUP_SEMANTICS,
    FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
};
use windows_sys::Win32::System::IO::DeviceIoControl;

use crate::scan::{NodeKind, RawNode};

const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x10;
/// NTFS stores this extra directory bit inside FILE_NAME attribute flags.
const FILE_ATTRIBUTE_DIRECTORY_NTFS: u32 = 0x1000_0000;
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const MAX_USN_RECORDS: u64 = 12_000_000;
const BUFFER_SIZE: usize = 1024 * 1024;
const ATTRIBUTE_TYPE_STANDARD_INFORMATION: u32 = 0x10;
const ATTRIBUTE_TYPE_FILE_NAME: u32 = 0x30;
const ATTRIBUTE_TYPE_DATA: u32 = 0x80;
const ATTRIBUTE_TYPE_END: u32 = 0xFFFF_FFFF;
/// FILE_NAME attribute value: parent ref (8) + timestamps/sizes (0x38) +
/// length (1) + namespace (1); the UTF-16 name starts at 0x42.
const FILE_NAME_HEADER_LEN: usize = 0x42;
/// DOS 8.3 aliases are not real paths, so they must not create tree nodes.
const FILE_NAME_NAMESPACE_DOS: u8 = 2;

const FILE_DEVICE_FILE_SYSTEM: u32 = 0x0000_0009;
const METHOD_BUFFERED: u32 = 0;
const METHOD_NEITHER: u32 = 3;
const FILE_ANY_ACCESS: u32 = 0;

const fn ctl_code(device: u32, function: u32, method: u32, access: u32) -> u32 {
    (device << 16) | (access << 14) | (function << 2) | method
}

const FSCTL_GET_NTFS_VOLUME_DATA: u32 = ctl_code(FILE_DEVICE_FILE_SYSTEM, 25, METHOD_BUFFERED, 0);
const FSCTL_QUERY_USN_JOURNAL: u32 = ctl_code(FILE_DEVICE_FILE_SYSTEM, 61, METHOD_BUFFERED, 0);
const FSCTL_ENUM_USN_DATA: u32 = ctl_code(FILE_DEVICE_FILE_SYSTEM, 44, METHOD_NEITHER, FILE_ANY_ACCESS);
const FSCTL_READ_USN_JOURNAL: u32 = ctl_code(FILE_DEVICE_FILE_SYSTEM, 46, METHOD_NEITHER, FILE_ANY_ACCESS);

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct NtfsVolumeData {
    volume_serial_number: i64,
    number_sectors: i64,
    total_clusters: i64,
    free_clusters: i64,
    total_reserved: i64,
    bytes_per_sector: u32,
    bytes_per_cluster: u32,
    bytes_per_file_record_segment: u32,
    clusters_per_file_record_segment: u32,
    mft_valid_data_length: i64,
    mft_start_lcn: i64,
    mft2_start_lcn: i64,
    mft_zone_start: i64,
    mft_zone_end: i64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct UsnJournalData {
    usn_journal_id: u64,
    first_usn: i64,
    next_usn: i64,
    lowest_valid_usn: i64,
    max_usn: i64,
    maximum_size: u64,
    allocation_delta: u64,
}

#[repr(C)]
struct MftEnumData {
    start_file_reference_number: u64,
    low_usn: i64,
    high_usn: i64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum MftPhase {
    Enumerating,
    ReadingMft,
    BuildingTree,
}

#[derive(Debug)]
pub(crate) enum MftError {
    Cancelled,
    /// The USN journal was deleted or rewound; the index must be rebuilt.
    JournalReset,
    Failed(String),
}

pub(crate) struct Volume {
    pub(crate) handle: HANDLE,
}

impl Drop for Volume {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.handle);
        }
    }
}

fn wide(value: &str) -> Vec<u16> {
    OsStr::new(value).encode_wide().chain(std::iter::once(0)).collect()
}

fn drive_letter(path: &Path) -> Option<char> {
    let text = path.to_string_lossy();
    let bytes = text.as_bytes();
    if bytes.len() >= 2 && bytes[1] == b':' {
        Some((bytes[0] as char).to_ascii_uppercase())
    } else {
        None
    }
}

fn is_drive_root(path: &Path) -> bool {
    let text = path.to_string_lossy();
    let trimmed = text.trim_end_matches(['\\', '/']);
    trimmed.len() == 2 && trimmed.as_bytes()[1] == b':'
}

pub(crate) fn open_volume(letter: char) -> Result<Volume, MftError> {
    let device = format!(r"\\.\{}:", letter);
    let name = wide(&device);
    let handle = unsafe {
        CreateFileW(
            name.as_ptr(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            std::ptr::null(),
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            std::ptr::null_mut(),
        )
    };
    if handle.is_null() || handle == windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE {
        let err = unsafe { GetLastError() };
        return Err(MftError::Failed(format!(
            "cannot open volume {device} (error {err})"
        )));
    }
    Ok(Volume { handle })
}

pub(crate) unsafe fn ioctl(
    handle: HANDLE,
    code: u32,
    input: *const core::ffi::c_void,
    input_len: u32,
    output: *mut core::ffi::c_void,
    output_len: u32,
) -> Result<u32, u32> {
    let mut returned = 0u32;
    let ok = DeviceIoControl(
        handle,
        code,
        input,
        input_len,
        output,
        output_len,
        &mut returned,
        std::ptr::null_mut(),
    );
    if ok == 0 {
        Err(GetLastError())
    } else {
        Ok(returned)
    }
}

/// True when the volume is NTFS, determined without opening the raw device so
/// a non-elevated process can still pick the directory-walk fallback.
pub(crate) fn is_ntfs_volume(path: &Path) -> bool {
    let Some(letter) = drive_letter(path) else {
        return false;
    };
    let name = wide(&format!("{letter}:\\"));
    let mut file_system = [0u16; 32];
    let ok = unsafe {
        GetVolumeInformationW(
            name.as_ptr(),
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            file_system.as_mut_ptr(),
            file_system.len() as u32,
        )
    };
    if ok == 0 {
        return false;
    }
    let length = file_system
        .iter()
        .position(|value| *value == 0)
        .unwrap_or(file_system.len());
    String::from_utf16_lossy(&file_system[..length]).eq_ignore_ascii_case("NTFS")
}

/// True when the volume is NTFS and readable by this process.
pub(crate) fn is_supported(path: &Path) -> bool {
    let Some(letter) = drive_letter(path) else {
        return false;
    };
    let Ok(volume) = open_volume(letter) else {
        return false;
    };
    let mut data: NtfsVolumeData = Default::default();
    let ok = unsafe {
        ioctl(
            volume.handle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            std::ptr::null(),
            0,
            &mut data as *mut _ as *mut core::ffi::c_void,
            std::mem::size_of::<NtfsVolumeData>() as u32,
        )
    };
    ok.is_ok()
}

unsafe fn read_at(handle: HANDLE, offset: i64, buffer: &mut [u8]) -> Result<(), String> {
    if SetFilePointerEx(handle, offset, std::ptr::null_mut(), 0) == 0 {
        return Err(format!("seek to {offset} failed (error {})", GetLastError()));
    }
    let mut read = 0u32;
    let ok = ReadFile(
        handle,
        buffer.as_mut_ptr(),
        buffer.len() as u32,
        &mut read,
        std::ptr::null_mut(),
    );
    if ok == 0 {
        return Err(format!("read at {offset} failed (error {})", GetLastError()));
    }
    if read as usize != buffer.len() {
        return Err(format!(
            "read at {offset} returned {read} of {} bytes",
            buffer.len()
        ));
    }
    Ok(())
}

fn u16_at(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(data.get(offset..offset + 2)?.try_into().ok()?))
}

fn u32_at(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(offset..offset + 4)?.try_into().ok()?))
}

fn i64_at(data: &[u8], offset: usize) -> Option<i64> {
    Some(i64::from_le_bytes(data.get(offset..offset + 8)?.try_into().ok()?))
}

fn u64_at(data: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_le_bytes(data.get(offset..offset + 8)?.try_into().ok()?))
}

struct UsnEntry {
    file_ref: u64,
    parent_ref: u64,
    name: String,
    attributes: u32,
}

fn parse_usn_record(data: &[u8], offset: usize, end: usize) -> Option<(UsnEntry, usize)> {
    if offset + 60 > end {
        return None;
    }
    let record_length = u32_at(data, offset)? as usize;
    if record_length < 60 || offset + record_length > end {
        return None;
    }
    let major = u16_at(data, offset + 4)?;
    if major != 2 {
        return Some((
            UsnEntry {
                file_ref: 0,
                parent_ref: 0,
                name: String::new(),
                attributes: 0,
            },
            record_length,
        ));
    }
    let file_ref = u64_at(data, offset + 8)? & 0x0000_FFFF_FFFF_FFFF;
    let parent_ref = u64_at(data, offset + 16)? & 0x0000_FFFF_FFFF_FFFF;
    let attributes = u32_at(data, offset + 52)?;
    let name_length = u16_at(data, offset + 56)? as usize;
    let name_offset = u16_at(data, offset + 58)? as usize;
    let name_start = offset + name_offset;
    let name_end = name_start + name_length;
    if name_end > offset + record_length || name_end > data.len() {
        return Some((
            UsnEntry {
                file_ref,
                parent_ref,
                name: String::new(),
                attributes,
            },
            record_length,
        ));
    }
    let units: Vec<u16> = data[name_start..name_end]
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect();
    Some((
        UsnEntry {
            file_ref,
            parent_ref,
            name: String::from_utf16_lossy(&units),
            attributes,
        },
        record_length,
    ))
}

fn apply_fixup(record: &mut [u8], bytes_per_sector: u32) -> bool {
    let Some(usa_offset) = u16_at(record, 4).map(|value| value as usize) else {
        return false;
    };
    let Some(usa_count) = u16_at(record, 6).map(|value| value as usize) else {
        return false;
    };
    if usa_count == 0 || usa_offset + usa_count * 2 > record.len() {
        return false;
    }
    let sector = bytes_per_sector.max(512) as usize;
    let usn = u16::from_le_bytes([record[usa_offset], record[usa_offset + 1]]);
    for index in 1..usa_count {
        let position = index * sector;
        if position < 2 || position > record.len() {
            return false;
        }
        let tail = position - 2;
        if record[tail..tail + 2] != usn.to_le_bytes() {
            return false;
        }
        let source = usa_offset + index * 2;
        let replacement = [record[source], record[source + 1]];
        record[tail..tail + 2].copy_from_slice(&replacement);
    }
    true
}

fn parse_data_runs(data: &[u8]) -> Vec<(i64, i64)> {
    let mut runs = Vec::new();
    let mut position = 0usize;
    let mut cluster = 0i64;
    while position < data.len() {
        let header = data[position];
        position += 1;
        if header == 0 {
            break;
        }
        let length_size = (header & 0x0F) as usize;
        let offset_size = (header >> 4) as usize;
        if length_size == 0
            || length_size > 8
            || offset_size > 8
            || position + length_size + offset_size > data.len()
        {
            break;
        }
        let mut length = 0u64;
        for index in 0..length_size {
            length |= (data[position + index] as u64) << (8 * index);
        }
        position += length_size;
        let mut offset = 0i64;
        for index in 0..offset_size {
            offset |= (data[position + index] as i64) << (8 * index);
        }
        if offset_size > 0 && offset_size < 8 && data[position + offset_size - 1] & 0x80 != 0 {
            offset |= -1i64 << (8 * offset_size);
        }
        position += offset_size;
        cluster += offset;
        if length > 0 {
            runs.push((cluster, length as i64));
        }
    }
    runs
}

struct MftNode {
    file_ref: u64,
    name: String,
    attributes: u32,
    skipped: bool,
    /// The file's own size (directories are 0).
    size: u64,
    /// Subtree aggregates, filled in place by `compute_aggregates`.
    agg_size: u64,
    agg_files: u32,
    agg_dirs: u32,
    children: Vec<u32>,
    parent: Option<u32>,
}

fn is_hidden(name: &str, attributes: u32) -> bool {
    name.starts_with('.') || name.starts_with('$') || attributes & FILE_ATTRIBUTE_HIDDEN != 0
}

fn is_dir(attributes: u32) -> bool {
    // The on-disk FILE_NAME entry marks directories with 0x10000000 in addition
    // to the documented FILE_ATTRIBUTE_DIRECTORY (0x10) flag.
    attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DIRECTORY_NTFS) != 0
}

fn is_symlink(attributes: u32) -> bool {
    attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
}

fn skip_name(name: &str, skip_heavy: bool) -> bool {
    name.eq_ignore_ascii_case("winsxs")
        || (skip_heavy && name.eq_ignore_ascii_case("node_modules"))
}

fn normalize(path: &str) -> String {
    let text = path.trim().replace('/', "\\");
    let trimmed = text.trim_end_matches('\\');
    if trimmed.len() == 2 && trimmed.as_bytes()[1] == b':' {
        format!("{trimmed}\\")
    } else {
        trimmed.to_string()
    }
}

fn join_path(parent: &str, name: &str) -> String {
    if parent.ends_with('\\') || parent.ends_with('/') {
        format!("{parent}{name}")
    } else {
        format!("{parent}\\{name}")
    }
}

fn path_key(path: &str) -> String {
    path.trim_end_matches(['\\', '/']).to_lowercase()
}

/// Bottom-up aggregate, equivalent to the old `Tree::computed` but writing the
/// results straight back into each node so no parallel result array is needed.
/// Returns `false` when cancelled.
fn compute_aggregates(nodes: &mut [MftNode], root: u32, cancel: &AtomicBool) -> bool {
    let mut state = vec![0u8; nodes.len()];
    let mut stack = vec![(root, false)];
    let mut visited = 0u64;
    while let Some((index, expanded)) = stack.pop() {
        visited += 1;
        if visited % 100_000 == 0 && cancel.load(Ordering::Relaxed) {
            return false;
        }
        let slot = index as usize;
        match state[slot] {
            2 => continue,
            1 if !expanded => continue,
            _ => {}
        }
        if expanded {
            // Reparse-point files (cloud placeholders, AppX stubs, …) are real
            // files with a size; only reparse *directories* are links.
            let is_file = !is_dir(nodes[slot].attributes);
            let mut size = if is_file { nodes[slot].size } else { 0 };
            let mut files = 0u32;
            let mut dirs = 0u32;
            for &child in &nodes[slot].children {
                let child_node = &nodes[child as usize];
                size += child_node.agg_size;
                if is_dir(child_node.attributes) && !is_symlink(child_node.attributes) {
                    dirs += 1 + child_node.agg_dirs;
                    files += child_node.agg_files;
                } else if !is_dir(child_node.attributes) {
                    files += 1;
                }
            }
            let node = &mut nodes[slot];
            node.agg_size = size;
            node.agg_files = files;
            node.agg_dirs = dirs;
            state[slot] = 2;
        } else {
            state[slot] = 1;
            stack.push((index, true));
            for &child in nodes[slot].children.iter().rev() {
                if state[child as usize] == 0 {
                    stack.push((child, false));
                }
            }
        }
    }
    true
}

/// Converts the flat MFT arena into the scan tree, moving names and child lists
/// out of the arena so every name keeps a single heap allocation.
/// Returns `None` when cancelled.
fn build_raw(
    nodes: &mut [MftNode],
    index: u32,
    name: String,
    kind: NodeKind,
    depth: u32,
    cancel: &AtomicBool,
) -> Option<RawNode> {
    if cancel.load(Ordering::Relaxed) {
        return None;
    }
    let slot = index as usize;
    let own_size = nodes[slot].size;
    let (agg_size, agg_files, agg_dirs, skipped, attributes) = {
        let node = &nodes[slot];
        (
            node.agg_size,
            node.agg_files,
            node.agg_dirs,
            node.skipped,
            node.attributes,
        )
    };
    let mut raw = RawNode {
        name,
        kind,
        size: if kind == NodeKind::File { own_size } else { agg_size },
        file_count: agg_files,
        dir_count: agg_dirs,
        is_hidden: false,
        skipped,
        error: None,
        children: Vec::new(),
    };
    // `name` moved out above, so recompute hidden from the value passed in.
    raw.is_hidden = is_hidden(&raw.name, attributes);
    if skipped || depth >= 512 {
        return Some(raw);
    }
    let children = std::mem::take(&mut nodes[slot].children);
    let mut converted = Vec::with_capacity(children.len());
    for child in children {
        let (child_name, child_attributes, child_size) = {
            let child_node = &mut nodes[child as usize];
            (
                std::mem::take(&mut child_node.name),
                child_node.attributes,
                child_node.size,
            )
        };
        if is_dir(child_attributes) && is_symlink(child_attributes) {
            let hidden = is_hidden(&child_name, child_attributes);
            converted.push(RawNode {
                name: child_name,
                kind: NodeKind::Symlink,
                size: child_size,
                file_count: 0,
                dir_count: 0,
                is_hidden: hidden,
                skipped: false,
                error: None,
                children: Vec::new(),
            });
            continue;
        }
        let child_kind = if is_dir(child_attributes) {
            NodeKind::Directory
        } else {
            NodeKind::File
        };
        converted.push(build_raw(
            nodes,
            child,
            child_name,
            child_kind,
            depth + 1,
            cancel,
        )?);
    }
    raw.children = converted;
    Some(raw)
}

fn aggregate_root(node: &mut RawNode) {
    let mut size = 0u64;
    let mut files = 0u32;
    let mut dirs = 0u32;
    for child in &node.children {
        size += child.size;
        match child.kind {
            NodeKind::Directory | NodeKind::Drive => {
                dirs += 1 + child.dir_count;
                files += child.file_count;
            }
            NodeKind::File => files += 1,
            _ => {}
        }
    }
    node.size = size;
    node.file_count = files;
    node.dir_count = dirs;
}

fn filetime_to_unix(ft: i64) -> i64 {
    if ft <= 0 {
        0
    } else {
        ft / 10_000_000 - 11_644_473_600
    }
}

/// One on-disk FILE_NAME entry: a single path of a file record. Hard links
/// yield several of these for the same `file_ref`, which is why the tree is
/// built from these instead of the single name the USN journal returns.
#[derive(Debug, Clone)]
pub(crate) struct MftPath {
    pub(crate) file_ref: u64,
    pub(crate) parent_ref: u64,
    pub(crate) name: String,
    pub(crate) attributes: u32,
}

/// Reads every live `$MFT` record once and returns `(size, modified)` per file
/// reference. The index uses the same pass so it never has to `stat` files.
///
/// `paths` holds every Win32 FILE_NAME entry (including those stored in
/// extension records) so hard-linked paths are all represented.
///
/// The third tuple element reports whether low memory cut the read short.
fn read_mft_records(
    handle: HANDLE,
    volume_data: &NtfsVolumeData,
    cancel: &AtomicBool,
    low_memory: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(MftPhase, u64),
) -> Result<
    (
        HashMap<u64, (i64, i64)>,
        Vec<MftPath>,
        u64,
        bool,
    ),
    MftError,
> {
    let bytes_per_cluster = volume_data.bytes_per_cluster;
    let bytes_per_sector = volume_data.bytes_per_sector.max(512);
    let record_size = if volume_data.bytes_per_file_record_segment == 0 {
        1024
    } else {
        volume_data.bytes_per_file_record_segment as usize
    };
    // Size both containers from the $MFT length so a whole-volume read never
    // pays for repeated geometric growth. Clamp to the record cap so a broken
    // volume header cannot request an absurd allocation.
    let estimated = (volume_data.mft_valid_data_length.max(0) as usize / record_size)
        .clamp(1_024, MAX_USN_RECORDS as usize + 1_024);
    let mut records: HashMap<u64, (i64, i64)> = HashMap::with_capacity(estimated);
    let mut record0 = vec![0u8; record_size];
    unsafe {
        read_at(
            handle,
            volume_data.mft_start_lcn * bytes_per_cluster as i64,
            &mut record0,
        )
    }
    .map_err(|err| MftError::Failed(format!("cannot read $MFT record 0: {err}")))?;
    if !apply_fixup(&mut record0, bytes_per_sector) {
        return Err(MftError::Failed(
            "$MFT record 0 fixup failed (unsupported sector size?)".to_string(),
        ));
    }
    if &record0[0..4] != b"FILE" {
        return Err(MftError::Failed("$MFT record 0 magic mismatch".to_string()));
    }
    let first_attribute = u16_at(&record0, 20).unwrap_or(0) as usize;
    let mut position = first_attribute;
    let mut runs = Vec::new();
    while position + 16 <= record0.len() {
        let Some(attribute_type) = u32_at(&record0, position) else {
            break;
        };
        let Some(length) = u32_at(&record0, position + 4).map(|value| value as usize) else {
            break;
        };
        if attribute_type == ATTRIBUTE_TYPE_END || length == 0 || position + length > record0.len() {
            break;
        }
        let name_length = record0[position + 9];
        let non_resident = record0[position + 8] != 0;
        if attribute_type == ATTRIBUTE_TYPE_DATA && name_length == 0 && non_resident {
            let run_offset = u16_at(&record0, position + 32).unwrap_or(0) as usize;
            runs = parse_data_runs(&record0[position + run_offset..position + length]);
            break;
        }
        position += length;
    }
    if runs.is_empty() {
        return Err(MftError::Failed(
            "cannot parse $MFT data runs from record 0".to_string(),
        ));
    }

    let mut chunk = vec![0u8; BUFFER_SIZE];
    let mut record_number = 0u64;
    let mut truncated = false;
    let mut paths: Vec<MftPath> = Vec::with_capacity(estimated);
    let mut name_slots: Vec<(u64, usize, usize, u32)> = Vec::new();
    'runs: for (start_cluster, cluster_count) in runs {
        let run_start = start_cluster * bytes_per_cluster as i64;
        let run_length = cluster_count * bytes_per_cluster as i64;
        let mut offset = 0i64;
        while offset < run_length {
            if cancel.load(Ordering::Relaxed) {
                return Err(MftError::Cancelled);
            }
            if low_memory() {
                truncated = true;
                break 'runs;
            }
            let to_read = ((run_length - offset) as usize).min(chunk.len());
            let buffer = &mut chunk[..to_read];
            if let Err(err) = unsafe { read_at(handle, run_start + offset, buffer) } {
                log::warn!("[mft] {err}");
                break 'runs;
            }
            let count = to_read / record_size;
            for index in 0..count {
                let record = &mut buffer[index * record_size..(index + 1) * record_size];
                record_number += 1;
                if record_number % 10_000 == 0 {
                    progress(MftPhase::ReadingMft, record_number);
                }
                if &record[0..4] != b"FILE" {
                    continue;
                }
                let flags = u16_at(record, 22).unwrap_or(0);
                if flags & 0x0001 == 0 {
                    continue;
                }
                let base_ref = u64_at(record, 32).unwrap_or(0) & 0x0000_FFFF_FFFF_FFFF;
                // `record_number` counts 1-based records (1 = $MFT), while MFT
                // references are 0-based, so the current record is one less.
                let file_ref = if base_ref != 0 {
                    base_ref
                } else {
                    record_number - 1
                };
                if !apply_fixup(record, bytes_per_sector) {
                    continue;
                }
                let mut size_value = 0i64;
                let mut mtime_value = 0i64;
                name_slots.clear();
                let first_attribute = u16_at(record, 20).unwrap_or(0) as usize;
                let mut position = first_attribute;
                while position + 16 <= record.len() {
                    let Some(attribute_type) = u32_at(record, position) else {
                        break;
                    };
                    let Some(length) = u32_at(record, position + 4).map(|value| value as usize) else {
                        break;
                    };
                    if attribute_type == ATTRIBUTE_TYPE_END
                        || length == 0
                        || position + length > record.len()
                    {
                        break;
                    }
                    let name_length = record[position + 9];
                    let non_resident = record[position + 8] != 0;
                    if attribute_type == ATTRIBUTE_TYPE_STANDARD_INFORMATION && !non_resident {
                        let content = u16_at(record, position + 20).unwrap_or(0) as usize;
                        if let Some(ft) = u64_at(record, position + content + 8) {
                            mtime_value = filetime_to_unix(ft as i64);
                        }
                    } else if attribute_type == ATTRIBUTE_TYPE_DATA && name_length == 0 {
                        size_value = if non_resident {
                            i64_at(record, position + 48).unwrap_or(0)
                        } else {
                            u32_at(record, position + 16).unwrap_or(0) as i64
                        };
                    } else if attribute_type == ATTRIBUTE_TYPE_FILE_NAME && !non_resident {
                        let value_len = u32_at(record, position + 16).unwrap_or(0) as usize;
                        let content = u16_at(record, position + 20).unwrap_or(0) as usize;
                        let value_start = position + content;
                        if value_len >= FILE_NAME_HEADER_LEN
                            && value_start + value_len <= record.len()
                            && record[value_start + 0x41] != FILE_NAME_NAMESPACE_DOS
                        {
                            let parent_ref =
                                u64_at(record, value_start).unwrap_or(0) & 0x0000_FFFF_FFFF_FFFF;
                            let units = record[value_start + 0x40] as usize;
                            let name_start = value_start + FILE_NAME_HEADER_LEN;
                            if parent_ref != 0
                                && units > 0
                                && name_start + units * 2 <= value_start + value_len
                            {
                                let attributes = u32_at(record, value_start + 0x38).unwrap_or(0);
                                name_slots.push((parent_ref, name_start, units * 2, attributes));
                            }
                        }
                    }
                    position += length;
                }
                // Extension records carry extra hard-link names for their base
                // record, so every FILE_NAME entry is emitted under `file_ref`.
                for (parent_ref, name_start, name_bytes, attributes) in name_slots.drain(..) {
                    let units: Vec<u16> = record[name_start..name_start + name_bytes]
                        .chunks_exact(2)
                        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
                        .collect();
                    paths.push(MftPath {
                        file_ref,
                        parent_ref,
                        name: String::from_utf16_lossy(&units),
                        attributes,
                    });
                }
                // Extension records share the base file reference but carry no
                // size, so keep the largest value seen for that reference.
                records
                    .entry(file_ref)
                    .and_modify(|slot| {
                        if size_value > slot.0 {
                            slot.0 = size_value;
                        }
                    })
                    .or_insert((size_value.max(0), mtime_value));
            }
            offset += to_read as i64;
        }
    }
    progress(MftPhase::ReadingMft, record_number);
    Ok((records, paths, record_number, truncated))
}

/// Enumerates the USN journal once and returns every named record.
fn enumerate_volume(
    handle: HANDLE,
    next_usn: i64,
    cancel: &AtomicBool,
    low_memory: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(MftPhase, u64),
) -> Result<(Vec<UsnEntry>, bool, bool), MftError> {
    progress(MftPhase::Enumerating, 0);
    let mut enum_data = MftEnumData {
        start_file_reference_number: 0,
        low_usn: 0,
        high_usn: next_usn,
    };
    let mut buffer = vec![0u8; BUFFER_SIZE];
    let mut entries: Vec<UsnEntry> = Vec::new();
    let mut total_records = 0u64;
    let mut low_memory_seen = false;
    let mut truncated = false;

    loop {
        if cancel.load(Ordering::Relaxed) {
            return Err(MftError::Cancelled);
        }
        if low_memory() {
            low_memory_seen = true;
            break;
        }
        let returned = unsafe {
            ioctl(
                handle,
                FSCTL_ENUM_USN_DATA,
                &enum_data as *const _ as *const core::ffi::c_void,
                std::mem::size_of::<MftEnumData>() as u32,
                buffer.as_mut_ptr() as *mut core::ffi::c_void,
                buffer.len() as u32,
            )
        };
        let returned = match returned {
            Ok(returned) => returned as usize,
            Err(ERROR_HANDLE_EOF) => break,
            Err(err) => {
                return Err(MftError::Failed(format!(
                    "FSCTL_ENUM_USN_DATA failed (error {err})"
                )))
            }
        };
        if returned <= 8 {
            break;
        }
        let mut offset = 8usize;
        while offset < returned {
            let Some((entry, length)) = parse_usn_record(&buffer, offset, returned) else {
                break;
            };
            offset += length;
            if entry.file_ref == 0 || entry.name.is_empty() {
                continue;
            }
            total_records += 1;
            if total_records > MAX_USN_RECORDS {
                truncated = true;
                break;
            }
            entries.push(entry);
            if total_records % 100_000 == 0 {
                progress(MftPhase::Enumerating, total_records);
            }
        }
        if total_records > MAX_USN_RECORDS {
            break;
        }
        enum_data.start_file_reference_number = u64_at(&buffer, 0).unwrap_or(u64::MAX);
        if enum_data.start_file_reference_number == u64::MAX {
            break;
        }
    }
    progress(MftPhase::Enumerating, total_records);
    Ok((entries, low_memory_seen, truncated))
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct JournalSnapshot {
    pub(crate) journal_id: u64,
    pub(crate) first_usn: i64,
    pub(crate) next_usn: i64,
    pub(crate) lowest_valid_usn: i64,
}

pub(crate) fn query_journal(volume: &Volume) -> Result<JournalSnapshot, MftError> {
    let mut journal: UsnJournalData = Default::default();
    unsafe {
        ioctl(
            volume.handle,
            FSCTL_QUERY_USN_JOURNAL,
            std::ptr::null(),
            0,
            &mut journal as *mut _ as *mut core::ffi::c_void,
            std::mem::size_of::<UsnJournalData>() as u32,
        )
    }
    .map_err(|err| match err {
        1178 | 1179 | 1181 => MftError::JournalReset,
        other => MftError::Failed(format!("FSCTL_QUERY_USN_JOURNAL failed (error {other})")),
    })?;
    Ok(JournalSnapshot {
        journal_id: journal.usn_journal_id,
        first_usn: journal.first_usn,
        next_usn: journal.next_usn,
        lowest_valid_usn: journal.lowest_valid_usn,
    })
}

/// Queries the journal of one volume by its drive letter (for example `"C"`).
pub(crate) fn journal_for_volume(volume: &str) -> Result<JournalSnapshot, MftError> {
    let letter = volume
        .chars()
        .next()
        .filter(|value| value.is_ascii_alphabetic())
        .ok_or_else(|| MftError::Failed("invalid volume letter".to_string()))?
        .to_ascii_uppercase();
    let handle = open_volume(letter)?;
    query_journal(&handle)
}

/// Maps a `FSCTL_READ_USN_JOURNAL` error code to a typed error. Journal entry
/// deleted / journal deleting / journal not active all mean "rebuild".
fn usn_read_error(code: u32) -> MftError {
    match code {
        1178 | 1179 | 1181 => MftError::JournalReset,
        other => MftError::Failed(format!("FSCTL_READ_USN_JOURNAL failed (error {other})")),
    }
}

#[derive(Debug, Clone)]
pub(crate) struct UsnChange {
    pub(crate) file_ref: u64,
    pub(crate) parent_ref: u64,
    pub(crate) name: String,
    pub(crate) attributes: u32,
    pub(crate) reason: u32,
}

fn parse_usn_change(data: &[u8], offset: usize, end: usize) -> Option<(UsnChange, usize)> {
    if offset + 60 > end {
        return None;
    }
    let record_length = u32_at(data, offset)? as usize;
    if record_length < 60 || offset + record_length > end {
        return None;
    }
    let file_ref = u64_at(data, offset + 8)? & 0x0000_FFFF_FFFF_FFFF;
    let parent_ref = u64_at(data, offset + 16)? & 0x0000_FFFF_FFFF_FFFF;
    let reason = u32_at(data, offset + 40)?;
    let attributes = u32_at(data, offset + 52)?;
    let name_length = u16_at(data, offset + 56)? as usize;
    let name_offset = u16_at(data, offset + 58)? as usize;
    let name_start = offset + name_offset;
    let name_end = name_start + name_length;
    let name = if name_end <= offset + record_length && name_end <= data.len() {
        let units: Vec<u16> = data[name_start..name_end]
            .chunks_exact(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
            .collect();
        String::from_utf16_lossy(&units)
    } else {
        String::new()
    };
    Some((
        UsnChange {
            file_ref,
            parent_ref,
            name,
            attributes,
            reason,
        },
        record_length,
    ))
}

#[repr(C)]
struct ReadUsnJournalData {
    start_usn: i64,
    reason_mask: u32,
    return_only_on_close: u32,
    timeout: u64,
    bytes_to_wait_for: u64,
    journal_id: u64,
}

/// Reads the USN journal slice starting at `start_usn`.
pub(crate) fn read_usn_journal(
    volume: &Volume,
    journal_id: u64,
    start_usn: i64,
) -> Result<(Vec<UsnChange>, i64), MftError> {
    let input = ReadUsnJournalData {
        start_usn,
        reason_mask: u32::MAX,
        return_only_on_close: 0,
        timeout: 0,
        bytes_to_wait_for: 0,
        journal_id,
    };
    let mut buffer = vec![0u8; BUFFER_SIZE];
    let returned = unsafe {
        ioctl(
            volume.handle,
            FSCTL_READ_USN_JOURNAL,
            &input as *const _ as *const core::ffi::c_void,
            std::mem::size_of::<ReadUsnJournalData>() as u32,
            buffer.as_mut_ptr() as *mut core::ffi::c_void,
            buffer.len() as u32,
        )
    }
    .map_err(usn_read_error)? as usize;
    if returned < 8 {
        return Ok((Vec::new(), start_usn));
    }
    let next_usn = u64_at(&buffer, 0).unwrap_or(start_usn as u64) as i64;
    let mut changes = Vec::new();
    let mut offset = 8usize;
    while offset < returned {
        let Some((change, length)) = parse_usn_change(&buffer, offset, returned) else {
            break;
        };
        offset += length;
        if change.file_ref == 0 || change.name.is_empty() {
            continue;
        }
        changes.push(change);
    }
    Ok((changes, next_usn))
}

/// One entry of a freshly read volume, ready to be packed into an index.
#[derive(Debug, Clone)]
pub(crate) struct VolumeEntry {
    pub(crate) file_ref: u64,
    pub(crate) parent_ref: u64,
    pub(crate) name: String,
    pub(crate) attributes: u32,
    pub(crate) size: u64,
    pub(crate) mtime: i64,
}

#[derive(Debug)]
pub(crate) struct VolumeIndexData {
    pub(crate) entries: Vec<VolumeEntry>,
    pub(crate) journal_id: u64,
    pub(crate) next_usn: i64,
    pub(crate) volume_serial: u64,
    /// True when the USN enumeration hit `MAX_USN_RECORDS`.
    pub(crate) truncated_records: bool,
}

/// Full-volume read used by the search index: every live entry plus the USN
/// position needed for incremental updates afterwards.
pub(crate) fn collect_volume_entries(
    root: &Path,
    cancel: &AtomicBool,
    low_memory: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(MftPhase, u64),
) -> Result<VolumeIndexData, MftError> {
    let letter = drive_letter(root)
        .ok_or_else(|| MftError::Failed("the path is not on a local drive".to_string()))?;
    let volume = open_volume(letter)?;

    let mut volume_data: NtfsVolumeData = Default::default();
    unsafe {
        ioctl(
            volume.handle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            std::ptr::null(),
            0,
            &mut volume_data as *mut _ as *mut core::ffi::c_void,
            std::mem::size_of::<NtfsVolumeData>() as u32,
        )
    }
    .map_err(|err| MftError::Failed(format!("FSCTL_GET_NTFS_VOLUME_DATA failed (error {err})")))?;
    if volume_data.bytes_per_cluster == 0 || volume_data.mft_start_lcn < 0 {
        return Err(MftError::Failed(
            "invalid NTFS volume data (cluster size or MFT start)".to_string(),
        ));
    }

    let journal = query_journal(&volume)?;
    let (usn_entries, low_memory_seen, truncated_records) =
        enumerate_volume(volume.handle, journal.next_usn, cancel, low_memory, progress)?;
    if low_memory_seen {
        return Err(MftError::Failed("low memory".to_string()));
    }

    let (records, _, _, truncated) =
        read_mft_records(volume.handle, &volume_data, cancel, low_memory, progress)?;
    if truncated {
        return Err(MftError::Failed("low memory".to_string()));
    }

    progress(MftPhase::BuildingTree, 0);
    let mut entries = Vec::with_capacity(usn_entries.len());
    for entry in usn_entries {
        if entry.file_ref == 5 {
            continue;
        }
        let (size, mtime) = records.get(&entry.file_ref).copied().unwrap_or((0, 0));
        entries.push(VolumeEntry {
            file_ref: entry.file_ref,
            parent_ref: entry.parent_ref,
            name: entry.name,
            attributes: entry.attributes,
            size: size.max(0) as u64,
            mtime,
        });
    }
    progress(MftPhase::BuildingTree, entries.len() as u64);
    Ok(VolumeIndexData {
        entries,
        journal_id: journal.journal_id,
        next_usn: journal.next_usn,
        volume_serial: volume_data.volume_serial_number as u64,
        truncated_records,
    })
}

#[allow(clippy::too_many_arguments)]
fn push_mft_node(
    nodes: &mut Vec<MftNode>,
    dir_index: &mut HashMap<u64, u32>,
    pending: &mut Vec<(u32, u64)>,
    file_ref: u64,
    attributes: u32,
    parent_ref: u64,
    name: String,
    size: u64,
    skip_heavy: bool,
) {
    let index = nodes.len() as u32;
    let skipped = skip_name(&name, skip_heavy);
    nodes.push(MftNode {
        file_ref,
        name,
        attributes,
        skipped,
        size,
        agg_size: 0,
        agg_files: 0,
        agg_dirs: 0,
        children: Vec::new(),
        parent: None,
    });
    pending.push((index, parent_ref));
    if is_dir(attributes) {
        dir_index.entry(file_ref).or_insert(index);
    }
}

pub(crate) fn scan_volume(
    root: &Path,
    cancel: &AtomicBool,
    skip_heavy: bool,
    low_memory: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(MftPhase, u64),
) -> Result<RawNode, MftError> {
    let letter = drive_letter(root).ok_or_else(|| {
        MftError::Failed("the path is not on a local drive".to_string())
    })?;
    let volume = open_volume(letter)?;

    let mut volume_data: NtfsVolumeData = Default::default();
    unsafe {
        ioctl(
            volume.handle,
            FSCTL_GET_NTFS_VOLUME_DATA,
            std::ptr::null(),
            0,
            &mut volume_data as *mut _ as *mut core::ffi::c_void,
            std::mem::size_of::<NtfsVolumeData>() as u32,
        )
    }
    .map_err(|err| MftError::Failed(format!("FSCTL_GET_NTFS_VOLUME_DATA failed (error {err})")))?;

    let bytes_per_cluster = volume_data.bytes_per_cluster;
    if bytes_per_cluster == 0 || volume_data.mft_start_lcn < 0 {
        return Err(MftError::Failed(
            "invalid NTFS volume data (cluster size or MFT start)".to_string(),
        ));
    }

    progress(MftPhase::ReadingMft, 0);
    let (records, paths, _, _) =
        read_mft_records(volume.handle, &volume_data, cancel, low_memory, progress)?;

    progress(MftPhase::BuildingTree, 0);
    let drive_prefix = format!("{letter}:\\");
    let path_count = paths.len();
    let mut nodes: Vec<MftNode> = Vec::with_capacity(path_count);
    let mut dir_index: HashMap<u64, u32> = HashMap::new();
    let mut pending: Vec<(u32, u64)> = Vec::with_capacity(path_count);

    // One node per FILE_NAME entry, so every hard-linked path keeps the size
    // Explorer reports for it. `paths` is consumed so the name stays a single
    // allocation instead of being cloned into the node.
    for path in paths {
        if path.parent_ref == 0 || path.name.is_empty() {
            continue;
        }
        let size = records
            .get(&path.file_ref)
            .map(|(value, _)| (*value).max(0) as u64)
            .unwrap_or(0);
        push_mft_node(
            &mut nodes,
            &mut dir_index,
            &mut pending,
            path.file_ref,
            path.attributes,
            path.parent_ref,
            path.name,
            size,
            skip_heavy,
        );
    }
    // Aggregation and tree conversion only need the per-node sizes.
    drop(records);
    // Record 5 is the volume root; some journals omit its FILE_NAME entry.
    if !dir_index.contains_key(&5) {
        let index = nodes.len() as u32;
        nodes.push(MftNode {
            file_ref: 5,
            name: ".".to_string(),
            attributes: FILE_ATTRIBUTE_DIRECTORY,
            skipped: false,
            size: 0,
            agg_size: 0,
            agg_files: 0,
            agg_dirs: 0,
            children: Vec::new(),
            parent: None,
        });
        dir_index.insert(5, index);
    }
    for (child, parent_ref) in pending {
        if parent_ref == 0 {
            continue;
        }
        let Some(&parent) = dir_index.get(&parent_ref) else {
            continue;
        };
        if parent == child {
            continue;
        }
        nodes[child as usize].parent = Some(parent);
        nodes[parent as usize].children.push(child);
    }


    let root_index = if is_drive_root(root) {
        dir_index[&5]
    } else {
        let target = normalize(&root.to_string_lossy());
        nodes
            .iter()
            .enumerate()
            .find(|(_, node)| {
                node.file_ref != 5
                    && node
                        .parent
                        .map(|parent| nodes[parent as usize].file_ref == 5)
                        .unwrap_or(false)
                    && path_key(&join_path(&drive_prefix, &node.name)) == path_key(&target)
            })
            .map(|(index, _)| index as u32)
            .ok_or_else(|| {
                MftError::Failed(format!(
                    "cannot locate {} in the MFT",
                    root.to_string_lossy()
                ))
            })?
    };

    let node_count = nodes.len();
    if !compute_aggregates(&mut nodes, root_index, cancel) {
        return Err(MftError::Cancelled);
    }
    let (root_size, root_files, root_dirs) = {
        let node = &nodes[root_index as usize];
        (node.agg_size, node.agg_files, node.agg_dirs)
    };
    let root_name = if is_drive_root(root) {
        format!("{letter}:")
    } else {
        root.file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| root.to_string_lossy().into_owned())
    };
    let root_kind = if is_drive_root(root) {
        NodeKind::Drive
    } else {
        NodeKind::Directory
    };
    let mut raw = build_raw(&mut nodes, root_index, root_name, root_kind, 0, cancel)
        .ok_or(MftError::Cancelled)?;
    aggregate_root(&mut raw);
    raw.size = root_size;
    raw.file_count = root_files;
    raw.dir_count = root_dirs;
    // The whole tree now lives in `raw`; release the arena before flattening.
    drop(nodes);
    progress(MftPhase::BuildingTree, node_count as u64);
    Ok(raw)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_runs_decode_signed_offsets() {
        // length 3 clusters, offset +5; then length 2, offset -1.
        let runs = parse_data_runs(&[0x11, 0x03, 0x05, 0x11, 0x02, 0xFF, 0x00]);
        assert_eq!(runs, vec![(5, 3), (4, 2)]);
    }

    #[test]
    fn usn_record_parser_reads_name_and_parent() {
        let name: Vec<u16> = "hello.txt".encode_utf16().collect();
        let mut record = vec![0u8; 60 + name.len() * 2];
        let record_length = record.len() as u32;
        record[0..4].copy_from_slice(&record_length.to_le_bytes());
        record[4..6].copy_from_slice(&2u16.to_le_bytes());
        record[8..16].copy_from_slice(&42u64.to_le_bytes());
        record[16..24].copy_from_slice(&5u64.to_le_bytes());
        record[52..56].copy_from_slice(&0u32.to_le_bytes());
        record[56..58].copy_from_slice(&((name.len() * 2) as u16).to_le_bytes());
        record[58..60].copy_from_slice(&60u16.to_le_bytes());
        for (index, unit) in name.iter().enumerate() {
            record[60 + index * 2..62 + index * 2].copy_from_slice(&unit.to_le_bytes());
        }
        let (entry, length) = parse_usn_record(&record, 0, record.len()).expect("record");
        assert_eq!(length, record.len());
        assert_eq!(entry.file_ref, 42);
        assert_eq!(entry.parent_ref, 5);
        assert_eq!(entry.name, "hello.txt");
    }

    fn node(
        file_ref: u64,
        name: &str,
        attributes: u32,
        skipped: bool,
        size: u64,
        children: Vec<u32>,
        parent: Option<u32>,
    ) -> MftNode {
        MftNode {
            file_ref,
            name: name.to_string(),
            attributes,
            skipped,
            size,
            agg_size: 0,
            agg_files: 0,
            agg_dirs: 0,
            children,
            parent,
        }
    }

    #[test]
    fn compute_aggregates_counts_skipped_directories_in_parent() {
        let mut nodes = vec![
            node(5, ".", FILE_ATTRIBUTE_DIRECTORY, false, 0, vec![1, 2], None),
            node(
                10,
                "WinSxS",
                FILE_ATTRIBUTE_DIRECTORY,
                true,
                0,
                vec![3],
                Some(0),
            ),
            node(11, "normal.txt", 0, false, 10, Vec::new(), Some(0)),
            node(12, "big.dll", 0, false, 100, Vec::new(), Some(1)),
        ];
        assert!(compute_aggregates(
            &mut nodes,
            0,
            &AtomicBool::new(false)
        ));
        // WinSxS is skipped (not expanded in the UI) but its 100 bytes and
        // file still count towards the root, matching the directory walk.
        assert_eq!(
            (nodes[0].agg_size, nodes[0].agg_files, nodes[0].agg_dirs),
            (110, 2, 1)
        );
        assert_eq!(
            (nodes[1].agg_size, nodes[1].agg_files, nodes[1].agg_dirs),
            (100, 1, 0)
        );
    }

    #[test]
    fn build_raw_keeps_file_sizes_and_link_semantics() {
        let mut nodes = vec![
            node(5, "root", FILE_ATTRIBUTE_DIRECTORY, false, 0, vec![1, 2, 3], None),
            node(11, "a.txt", 0, false, 10, Vec::new(), Some(0)),
            node(
                12,
                "WinSxS",
                FILE_ATTRIBUTE_DIRECTORY,
                true,
                0,
                Vec::new(),
                Some(0),
            ),
            node(
                13,
                "link",
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
                false,
                7,
                Vec::new(),
                Some(0),
            ),
        ];
        assert!(compute_aggregates(
            &mut nodes,
            0,
            &AtomicBool::new(false)
        ));
        let raw = build_raw(
            &mut nodes,
            0,
            "root".to_string(),
            NodeKind::Directory,
            0,
            &AtomicBool::new(false),
        )
        .expect("build");
        assert_eq!(raw.children.len(), 3);
        let file = &raw.children[0];
        assert_eq!(file.kind, NodeKind::File);
        assert_eq!(file.size, 10);
        let skipped = &raw.children[1];
        assert!(skipped.skipped);
        assert_eq!(skipped.kind, NodeKind::Directory);
        let link = &raw.children[2];
        assert_eq!(link.kind, NodeKind::Symlink);
        assert_eq!(link.size, 7);
    }

    #[test]
    fn usn_read_errors_map_to_journal_reset() {
        assert!(matches!(usn_read_error(1181), MftError::JournalReset));
        assert!(matches!(usn_read_error(1178), MftError::JournalReset));
        assert!(matches!(usn_read_error(1179), MftError::JournalReset));
        assert!(matches!(usn_read_error(5), MftError::Failed(_)));
    }

    #[test]
    #[ignore = "requires administrator rights and a live NTFS system volume"]
    fn scans_the_system_volume() {
        let cancel = AtomicBool::new(false);
        let mut phases = Vec::new();
        let windows = scan_volume(
            Path::new(r"C:\Windows"),
            &cancel,
            false,
            &|| false,
            &mut |phase, _| phases.push(phase),
        )
        .expect("scan C:\\Windows");
        assert!(!windows.children.is_empty(), "Windows should have children");
        assert!(windows.size > 0, "Windows should have a size");

        let root = scan_volume(Path::new(r"C:\"), &cancel, false, &|| false, &mut |_, _| {})
            .expect("scan C:\\");
        assert!(root.children.len() > 1, "the drive root should have children");
        assert!(phases.contains(&MftPhase::ReadingMft));
        assert!(phases.contains(&MftPhase::BuildingTree));
    }
}
