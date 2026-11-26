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
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
const MAX_USN_RECORDS: u64 = 5_000_000;
const BUFFER_SIZE: usize = 1024 * 1024;
const ATTRIBUTE_TYPE_STANDARD_INFORMATION: u32 = 0x10;
const ATTRIBUTE_TYPE_DATA: u32 = 0x80;
const ATTRIBUTE_TYPE_END: u32 = 0xFFFF_FFFF;

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
            "cannot open volume {device} (error {err}); administrator rights are required"
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

struct MftEntry {
    file_ref: u64,
    parent_ref: u64,
    name: String,
    attributes: u32,
    skipped: bool,
}

struct MftNode {
    name: String,
    attributes: u32,
    skipped: bool,
    children: Vec<u64>,
    parent: Option<u64>,
    path: String,
}

fn is_hidden(name: &str, attributes: u32) -> bool {
    name.starts_with('.') || name.starts_with('$') || attributes & FILE_ATTRIBUTE_HIDDEN != 0
}

fn is_dir(attributes: u32) -> bool {
    attributes & FILE_ATTRIBUTE_DIRECTORY != 0
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

struct Tree {
    nodes: HashMap<u64, MftNode>,
    sizes: HashMap<u64, i64>,
}

impl Tree {
    fn computed(&self, _root: u64) -> HashMap<u64, (u64, u32, u32)> {
        let mut result: HashMap<u64, (u64, u32, u32)> = HashMap::new();
        let mut state: HashMap<u64, u8> = HashMap::new();
        for start in self.nodes.keys().copied() {
            if state.get(&start).copied() == Some(2) {
                continue;
            }
            let mut stack = vec![(start, false)];
            while let Some((reference, expanded)) = stack.pop() {
                match state.get(&reference).copied() {
                    Some(2) => continue,
                    Some(1) if !expanded => continue,
                    _ => {}
                }
                let Some(node) = self.nodes.get(&reference) else {
                    state.insert(reference, 2);
                    continue;
                };
                if expanded {
                    let is_file = !is_dir(node.attributes) && !is_symlink(node.attributes);
                    let mut size = if is_file {
                        self.sizes.get(&reference).copied().unwrap_or(0).max(0) as u64
                    } else {
                        0
                    };
                    let mut files = 0u32;
                    let mut dirs = 0u32;
                    for child in &node.children {
                        let Some((child_size, child_files, child_dirs)) = result.get(child) else {
                            continue;
                        };
                        let Some(child_node) = self.nodes.get(child) else {
                            continue;
                        };
                        if child_node.skipped {
                            continue;
                        }
                        size += child_size;
                        if is_dir(child_node.attributes) && !is_symlink(child_node.attributes) {
                            dirs += 1 + child_dirs;
                            files += child_files;
                        } else if !is_symlink(child_node.attributes) {
                            files += 1;
                        }
                    }
                    result.insert(reference, (size, files, dirs));
                    state.insert(reference, 2);
                } else {
                    state.insert(reference, 1);
                    stack.push((reference, true));
                    for child in node.children.iter().rev() {
                        if state.get(child).copied().unwrap_or(0) == 0 {
                            stack.push((*child, false));
                        }
                    }
                }
            }
        }
        result
    }

    fn build_raw(
        &self,
        reference: u64,
        path: String,
        name: String,
        kind: NodeKind,
        computed: &HashMap<u64, (u64, u32, u32)>,
        depth: u32,
    ) -> RawNode {
        let node = self.nodes.get(&reference);
        let skipped = node.map(|node| node.skipped).unwrap_or(false);
        let hidden = node
            .map(|node| is_hidden(&node.name, node.attributes))
            .unwrap_or(false);
        let mut raw = RawNode {
            path: path.clone().into(),
            name,
            kind,
            size: if kind == NodeKind::File {
                self.sizes.get(&reference).copied().unwrap_or(0).max(0) as u64
            } else {
                computed.get(&reference).map(|value| value.0).unwrap_or(0)
            },
            file_count: computed.get(&reference).map(|value| value.1).unwrap_or(0),
            dir_count: computed.get(&reference).map(|value| value.2).unwrap_or(0),
            is_hidden: hidden,
            skipped,
            error: None,
            children: Vec::new(),
        };
        if skipped || depth >= 512 {
            return raw;
        }
        if let Some(node) = node {
            for child in &node.children {
                let Some(child_node) = self.nodes.get(child) else {
                    continue;
                };
                if is_dir(child_node.attributes) && is_symlink(child_node.attributes) {
                    let child_path = join_path(&path, &child_node.name);
                    raw.children.push(RawNode {
                        path: child_path.into(),
                        name: child_node.name.clone(),
                        kind: NodeKind::Symlink,
                        size: self.sizes.get(child).copied().unwrap_or(0).max(0) as u64,
                        file_count: 0,
                        dir_count: 0,
                        is_hidden: is_hidden(&child_node.name, child_node.attributes),
                        skipped: false,
                        error: None,
                        children: Vec::new(),
                    });
                    continue;
                }
                let child_path = join_path(&path, &child_node.name);
                let child_kind = if is_dir(child_node.attributes) {
                    NodeKind::Directory
                } else {
                    NodeKind::File
                };
                raw.children.push(self.build_raw(
                    *child,
                    child_path,
                    child_node.name.clone(),
                    child_kind,
                    computed,
                    depth + 1,
                ));
            }
        }
        raw
    }
}

fn aggregate_root(node: &mut RawNode) {
    let mut size = 0u64;
    let mut files = 0u32;
    let mut dirs = 0u32;
    for child in &node.children {
        if child.skipped {
            continue;
        }
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

/// Reads every live `$MFT` record once and returns `(size, modified)` per file
/// reference. The index uses the same pass so it never has to `stat` files.
///
/// The third tuple element reports whether low memory cut the read short.
fn read_mft_records(
    handle: HANDLE,
    volume_data: &NtfsVolumeData,
    cancel: &AtomicBool,
    low_memory: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(MftPhase, u64),
) -> Result<(HashMap<u64, (i64, i64)>, u64, bool), MftError> {
    let bytes_per_cluster = volume_data.bytes_per_cluster;
    let bytes_per_sector = volume_data.bytes_per_sector.max(512);
    let record_size = if volume_data.bytes_per_file_record_segment == 0 {
        1024
    } else {
        volume_data.bytes_per_file_record_segment as usize
    };
    let mut records: HashMap<u64, (i64, i64)> = HashMap::new();
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
                let file_ref = if base_ref != 0 { base_ref } else { record_number };
                if !apply_fixup(record, bytes_per_sector) {
                    continue;
                }
                let mut size_value = 0i64;
                let mut mtime_value = 0i64;
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
                    }
                    position += length;
                }
                records
                    .entry(file_ref)
                    .or_insert((size_value.max(0), mtime_value));
            }
            offset += to_read as i64;
        }
    }
    progress(MftPhase::ReadingMft, record_number);
    Ok((records, record_number, truncated))
}

/// Enumerates the USN journal once and returns every named record.
fn enumerate_volume(
    handle: HANDLE,
    next_usn: i64,
    cancel: &AtomicBool,
    low_memory: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(MftPhase, u64),
) -> Result<(Vec<UsnEntry>, bool), MftError> {
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
    Ok((entries, low_memory_seen))
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct JournalSnapshot {
    pub(crate) journal_id: u64,
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
    .map_err(|err| MftError::Failed(format!("FSCTL_QUERY_USN_JOURNAL failed (error {err})")))?;
    Ok(JournalSnapshot {
        journal_id: journal.usn_journal_id,
        next_usn: journal.next_usn,
        lowest_valid_usn: journal.lowest_valid_usn,
    })
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
    .map_err(|err| MftError::Failed(format!("FSCTL_READ_USN_JOURNAL failed (error {err})")))? as usize;
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
    let (usn_entries, low_memory_seen) =
        enumerate_volume(volume.handle, journal.next_usn, cancel, low_memory, progress)?;
    if low_memory_seen {
        return Err(MftError::Failed("low memory".to_string()));
    }

    let (records, _, truncated) =
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
    })
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

    let journal = query_journal(&volume)?;
    let (usn_entries, low_memory_seen) =
        enumerate_volume(volume.handle, journal.next_usn, cancel, low_memory, progress)?;
    let entries: Vec<MftEntry> = usn_entries
        .into_iter()
        .map(|entry| MftEntry {
            file_ref: entry.file_ref,
            parent_ref: entry.parent_ref,
            skipped: skip_name(&entry.name, skip_heavy),
            name: entry.name,
            attributes: entry.attributes,
        })
        .collect();

    progress(MftPhase::ReadingMft, 0);
    let mut sizes: HashMap<u64, i64> = HashMap::new();
    if !low_memory_seen {
        let (records, _, _) =
            read_mft_records(volume.handle, &volume_data, cancel, low_memory, progress)?;
        sizes = records
            .into_iter()
            .map(|(reference, (size, _))| (reference, size))
            .collect();
    }

    progress(MftPhase::BuildingTree, 0);
    let drive_prefix = format!("{letter}:\\");
    let mut nodes: HashMap<u64, MftNode> = HashMap::new();
    let mut parent_refs: HashMap<u64, u64> = HashMap::new();
    for entry in &entries {
        if entry.parent_ref == 0 {
            continue;
        }
        parent_refs.entry(entry.file_ref).or_insert(entry.parent_ref);
        nodes.entry(entry.file_ref).or_insert_with(|| MftNode {
            name: entry.name.clone(),
            attributes: entry.attributes,
            skipped: entry.skipped,
            children: Vec::new(),
            parent: None,
            path: String::new(),
        });
    }
    // Record 5 is the volume root; some USN journals omit it or report no parent.
    nodes.entry(5).or_insert_with(|| MftNode {
        name: ".".to_string(),
        attributes: FILE_ATTRIBUTE_DIRECTORY,
        skipped: false,
        children: Vec::new(),
        parent: None,
        path: String::new(),
    });
    let mut parent_links: Vec<(u64, Option<u64>)> = nodes
        .keys()
        .map(|reference| {
            let parent = parent_refs
                .get(reference)
                .copied()
                .filter(|parent| *parent != 0 && *parent != *reference && nodes.contains_key(parent));
            (*reference, parent)
        })
        .collect();
    for (reference, parent) in parent_links.drain(..) {
        if let Some(node) = nodes.get_mut(&reference) {
            node.parent = parent;
        }
    }
    let parents: Vec<(u64, u64)> = nodes
        .iter()
        .filter_map(|(reference, node)| node.parent.map(|parent| (*reference, parent)))
        .collect();
    for (child, parent) in parents {
        if let Some(parent_node) = nodes.get_mut(&parent) {
            parent_node.children.push(child);
        }
    }

    let root_ref = if is_drive_root(root) {
        Some(5u64)
    } else {
        let target = normalize(&root.to_string_lossy());
        nodes
            .iter()
            .find(|(reference, node)| {
                if **reference == 5 {
                    return false;
                }
                let path = if let Some(parent) = node.parent {
                    if parent == 5 {
                        join_path(&drive_prefix, &node.name)
                    } else {
                        String::new()
                    }
                } else {
                    join_path(&drive_prefix, &node.name)
                };
                !path.is_empty() && path_key(&path) == path_key(&target)
            })
            .map(|(reference, _)| *reference)
    };
    let root_ref = root_ref.ok_or_else(|| {
        MftError::Failed(format!(
            "cannot locate {} in the MFT",
            root.to_string_lossy()
        ))
    })?;

    // Assign paths top-down from the drive root and any orphan subtrees.
    let mut stack: Vec<(u64, String)> = Vec::new();
    if let Some(node) = nodes.get(&5) {
        stack.push((5, drive_prefix.clone()));
        let _ = node;
    }
    for (reference, node) in &nodes {
        if node.parent.is_none() && *reference != 5 {
            stack.push((*reference, join_path(&drive_prefix, &node.name)));
        }
    }
    while let Some((reference, path)) = stack.pop() {
        let Some(node) = nodes.get_mut(&reference) else {
            continue;
        };
        if !node.path.is_empty() {
            continue;
        }
        node.path = path.clone();
        for child in node.children.clone() {
            if let Some(child_node) = nodes.get(&child) {
                if child_node.path.is_empty() {
                    stack.push((child, join_path(&path, &child_node.name)));
                }
            }
        }
    }

    let tree = Tree { nodes, sizes };
    let computed = tree.computed(root_ref);
    let root_node = tree.nodes.get(&root_ref);
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
    let root_path = root_node
        .map(|node| node.path.clone())
        .filter(|path| !path.is_empty())
        .unwrap_or_else(|| normalize(&root.to_string_lossy()));
    let mut raw = tree.build_raw(root_ref, root_path, root_name, root_kind, &computed, 0);
    aggregate_root(&mut raw);
    raw.size = computed.get(&root_ref).map(|value| value.0).unwrap_or(raw.size);
    raw.file_count = computed.get(&root_ref).map(|value| value.1).unwrap_or(raw.file_count);
    raw.dir_count = computed.get(&root_ref).map(|value| value.2).unwrap_or(raw.dir_count);
    progress(MftPhase::BuildingTree, entries.len() as u64);
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
        assert!(phases.contains(&MftPhase::Enumerating));
        assert!(phases.contains(&MftPhase::ReadingMft));
    }
}
