//! Runtime state shared by the index commands, the builder and the watchers.
//! One slot exists per indexed volume; queries clone the volume handles under a
//! short state lock so builds and incremental updates never block the UI.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

use crate::mft;

use super::model::Index;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VolumeStatus {
    pub(crate) volume: String,
    pub(crate) status: String,
    pub(crate) count: u64,
    pub(crate) built_at: i64,
    pub(crate) elapsed_ms: u64,
    pub(crate) message: String,
    pub(crate) memory_bytes: u64,
    pub(crate) can_build: bool,
    pub(crate) snapshot: bool,
    pub(crate) engine: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct IndexStatus {
    pub(crate) status: String,
    pub(crate) volume: String,
    pub(crate) count: u64,
    pub(crate) built_at: i64,
    pub(crate) elapsed_ms: u64,
    pub(crate) message: String,
    pub(crate) memory_bytes: u64,
    pub(crate) can_build: bool,
    pub(crate) realtime: bool,
    pub(crate) generation: u64,
    pub(crate) snapshot: bool,
    pub(crate) engine: String,
    pub(crate) writable: bool,
    pub(crate) volumes: Vec<VolumeStatus>,
}

#[derive(Clone)]
pub(crate) struct CachedResponse {
    pub(crate) key: String,
    pub(crate) version: String,
    pub(crate) response: super::query::SearchResponse,
}

pub(crate) struct VolumeSlot {
    pub(crate) status: String,
    pub(crate) index: Option<Arc<RwLock<Index>>>,
    pub(crate) message: String,
    pub(crate) built_at: i64,
    pub(crate) elapsed_ms: u64,
    pub(crate) can_build: bool,
    pub(crate) snapshot: bool,
    pub(crate) generation: u64,
}

impl VolumeSlot {
    pub(crate) fn new(volume: &str) -> Self {
        Self {
            status: "idle".to_string(),
            index: None,
            message: String::new(),
            built_at: 0,
            elapsed_ms: 0,
            can_build: mft::is_supported(Path::new(&volume_root(volume))),
            snapshot: false,
            generation: 0,
        }
    }
}

pub(crate) struct IndexState {
    pub(crate) volumes: BTreeMap<String, VolumeSlot>,
    /// A global override ("disabled", fatal errors). Empty means the aggregate
    /// status is derived from the volume slots.
    pub(crate) status: String,
    pub(crate) message: String,
    pub(crate) realtime: bool,
    pub(crate) writable: bool,
}

impl Default for IndexState {
    fn default() -> Self {
        Self {
            volumes: BTreeMap::new(),
            status: String::new(),
            message: String::new(),
            realtime: false,
            writable: true,
        }
    }
}

pub struct IndexStore {
    pub(crate) state: RwLock<IndexState>,
    pub(crate) cancel: Mutex<Option<Arc<AtomicBool>>>,
    pub(crate) watchers: Mutex<BTreeMap<String, Arc<AtomicBool>>>,
    pub(crate) generation: AtomicU64,
    pub(crate) query_cache: Mutex<Option<CachedResponse>>,
}

impl Default for IndexStore {
    fn default() -> Self {
        Self {
            state: RwLock::new(IndexState::default()),
            cancel: Mutex::new(None),
            watchers: Mutex::new(BTreeMap::new()),
            generation: AtomicU64::new(0),
            query_cache: Mutex::new(None),
        }
    }
}

impl IndexStore {
    pub(crate) fn indexes(&self) -> Vec<(String, Arc<RwLock<Index>>)> {
        let Ok(state) = self.state.read() else {
            return Vec::new();
        };
        state
            .volumes
            .iter()
            .filter_map(|(volume, slot)| {
                slot.index
                    .clone()
                    .map(|handle| (volume.clone(), handle))
            })
            .collect()
    }

    pub(crate) fn index_for(&self, volume: &str) -> Option<Arc<RwLock<Index>>> {
        self.state
            .read()
            .ok()
            .and_then(|state| state.volumes.get(volume).and_then(|slot| slot.index.clone()))
    }

    pub(crate) fn status(&self) -> IndexStatus {
        let fallback = |message: &str| IndexStatus {
            status: "error".to_string(),
            volume: default_volume(),
            count: 0,
            built_at: 0,
            elapsed_ms: 0,
            message: message.to_string(),
            memory_bytes: 0,
            can_build: false,
            realtime: false,
            generation: self.generation.load(Ordering::SeqCst),
            snapshot: false,
            engine: String::new(),
            writable: false,
            volumes: Vec::new(),
        };
        let Ok(state) = self.state.read() else {
            return fallback("index state is unavailable");
        };

        let mut volumes: Vec<VolumeStatus> = Vec::with_capacity(state.volumes.len());
        let mut count = 0u64;
        let mut memory_bytes = 0u64;
        let mut built_at = 0i64;
        let mut elapsed_ms = 0u64;
        let mut snapshot = false;
        let mut any_ready = false;
        let mut any_building = false;
        let mut any_error = false;
        let mut any_mft = false;
        let mut any_walk = false;
        let mut primary: Option<String> = None;
        let mut messages: Vec<String> = Vec::new();

        for (volume, slot) in &state.volumes {
            let (volume_count, volume_memory, engine) = match &slot.index {
                Some(handle) => match handle.read() {
                    Ok(index) => (
                        index.live_count(),
                        index.memory_bytes(),
                        if index.journal_id != 0 { "mft" } else { "walk" },
                    ),
                    Err(_) => (0, 0, ""),
                },
                None => (0, 0, ""),
            };
            count += volume_count;
            memory_bytes += volume_memory;
            built_at = built_at.max(slot.built_at);
            elapsed_ms = elapsed_ms.max(slot.elapsed_ms);
            snapshot |= slot.snapshot;
            if engine == "mft" {
                any_mft = true;
            } else if engine == "walk" {
                any_walk = true;
            }
            match slot.status.as_str() {
                "building" => any_building = true,
                "ready" => {
                    any_ready = true;
                    if primary.is_none() {
                        primary = Some(volume.clone());
                    }
                }
                "error" => {
                    any_error = true;
                    if !slot.message.is_empty() {
                        messages.push(format!("{volume}: {}", slot.message));
                    }
                }
                _ => {}
            }
            if primary.is_none() && slots_usable(&slot.status) {
                primary = Some(volume.clone());
            }
            volumes.push(VolumeStatus {
                volume: volume.clone(),
                status: slot.status.clone(),
                count: volume_count,
                built_at: slot.built_at,
                elapsed_ms: slot.elapsed_ms,
                message: slot.message.clone(),
                memory_bytes: volume_memory,
                can_build: slot.can_build,
                snapshot: slot.snapshot,
                engine: engine.to_string(),
            });
        }

        let status = if !state.status.is_empty() {
            state.status.clone()
        } else if any_building {
            "building".to_string()
        } else if any_ready {
            "ready".to_string()
        } else if any_error {
            "error".to_string()
        } else {
            "idle".to_string()
        };
        let message = if !state.message.is_empty() {
            state.message.clone()
        } else {
            messages.join("; ")
        };
        let volume = primary
            .or_else(|| state.volumes.keys().next().cloned())
            .unwrap_or_else(default_volume);

        IndexStatus {
            status,
            volume,
            count,
            built_at,
            elapsed_ms,
            message,
            memory_bytes,
            can_build: state.volumes.values().any(|slot| slot.can_build),
            realtime: state.realtime,
            generation: self.generation.load(Ordering::SeqCst),
            snapshot,
            engine: if any_mft {
                "mft".to_string()
            } else if any_walk {
                "walk".to_string()
            } else {
                String::new()
            },
            writable: state.writable,
            volumes,
        }
    }

    pub(crate) fn stop_watcher(&self, volume: &str) {
        if let Ok(mut map) = self.watchers.lock() {
            if let Some(flag) = map.remove(volume) {
                flag.store(true, Ordering::Relaxed);
            }
        }
    }

    pub(crate) fn stop_watchers(&self) {
        if let Ok(mut map) = self.watchers.lock() {
            for (_, flag) in std::mem::take(&mut *map) {
                flag.store(true, Ordering::Relaxed);
            }
        }
    }

    pub(crate) fn register_watcher(&self, volume: String, stop: Arc<AtomicBool>) {
        if let Ok(mut map) = self.watchers.lock() {
            if let Some(previous) = map.insert(volume, stop) {
                previous.store(true, Ordering::Relaxed);
            }
        }
    }

    pub(crate) fn stop_build(&self) {
        if let Ok(mut guard) = self.cancel.lock() {
            if let Some(flag) = guard.take() {
                flag.store(true, Ordering::Relaxed);
            }
        }
    }

    pub(crate) fn writable(&self) -> bool {
        self.state
            .read()
            .map(|state| state.writable)
            .unwrap_or(false)
    }

    pub(crate) fn clear_query_cache(&self) {
        if let Ok(mut guard) = self.query_cache.lock() {
            *guard = None;
        }
    }
}

fn slots_usable(status: &str) -> bool {
    status == "ready" || status == "building"
}

pub(crate) fn default_volume() -> String {
    std::env::var("SystemDrive")
        .ok()
        .and_then(|value| value.trim().chars().next())
        .filter(|value| value.is_ascii_alphabetic())
        .map(|value| value.to_ascii_uppercase().to_string())
        .unwrap_or_else(|| "C".to_string())
}

pub(crate) fn normalize_volume(value: &str) -> String {
    let text = value.trim();
    match text.chars().next() {
        Some(letter) if letter.is_ascii_alphabetic() => letter.to_ascii_uppercase().to_string(),
        _ => default_volume(),
    }
}

pub(crate) fn volume_root(volume: &str) -> String {
    format!("{volume}:\\")
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct IndexProgress {
    volume: String,
    phase: String,
    count: u64,
}

pub(crate) fn emit_status(app: &AppHandle) {
    let status = app.state::<IndexStore>().status();
    let _ = app.emit("index://status", status);
}

pub(crate) fn emit_progress(app: &AppHandle, volume: &str, phase: &str, count: u64) {
    let _ = app.emit(
        "index://progress",
        IndexProgress {
            volume: volume.to_string(),
            phase: phase.to_string(),
            count,
        },
    );
}

pub(crate) fn update_status(app: &AppHandle, status: &str, message: &str) {
    if let Ok(mut state) = app.state::<IndexStore>().state.write() {
        state.status = status.to_string();
        state.message = message.to_string();
    }
    emit_status(app);
}

/// Drops a global override so the aggregate status reflects the volume slots.
pub(crate) fn clear_status_override(app: &AppHandle) {
    if let Ok(mut state) = app.state::<IndexStore>().state.write() {
        state.status.clear();
        state.message.clear();
    }
}
