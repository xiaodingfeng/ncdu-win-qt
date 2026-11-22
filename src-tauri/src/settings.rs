use std::fs;
use std::path::PathBuf;

use keyring::Entry;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Manager};
use winreg::enums::{HKEY_CURRENT_USER, KEY_SET_VALUE};
use winreg::RegKey;

use crate::legacy::{self, ImportReport};

const LEGACY_KEY: &str = "Software\\NcduWin";
const LEGACY_AI_KEY: &str = "aiApiKey";
pub(crate) const KEYRING_SERVICE: &str = "NcduWin";
pub(crate) const KEYRING_ACCOUNT: &str = "aiApiKey";
const SETTINGS_FILE: &str = "settings.json";

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AppSettings {
    pub language: String,
    pub theme: String,
    pub custom_theme: Option<CustomTheme>,
    pub scan: ScanPrefs,
    pub default_move_path: String,
    pub move_scan: MoveScanPrefs,
    pub auto_update_guard: bool,
    pub ai: AiSettings,
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            language: default_language(),
            theme: "system".to_string(),
            custom_theme: None,
            scan: ScanPrefs::default(),
            default_move_path: String::new(),
            move_scan: MoveScanPrefs::default(),
            auto_update_guard: false,
            ai: AiSettings::default(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct CustomTheme {
    pub base: String,
    pub colors: std::collections::BTreeMap<String, String>,
}

impl Default for CustomTheme {
    fn default() -> Self {
        Self {
            base: "dark".to_string(),
            colors: Default::default(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ScanPrefs {
    pub skip_heavy_dirs: bool,
    pub show_files: bool,
    pub cleanup_categories: bool,
    pub cleanup_large: bool,
    pub cleanup_duplicates: bool,
}

impl Default for ScanPrefs {
    fn default() -> Self {
        Self {
            skip_heavy_dirs: true,
            show_files: true,
            cleanup_categories: true,
            cleanup_large: true,
            cleanup_duplicates: true,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct MoveScanPrefs {
    pub custom_dirs: Vec<String>,
    pub scope_index: u32,
    pub last_target_root: String,
}

impl Default for MoveScanPrefs {
    fn default() -> Self {
        Self {
            custom_dirs: Vec::new(),
            scope_index: 0,
            last_target_root: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AiSettings {
    pub base_url: String,
    pub model: String,
    pub has_api_key: bool,
}

impl Default for AiSettings {
    fn default() -> Self {
        Self {
            base_url: String::new(),
            model: String::new(),
            has_api_key: false,
        }
    }
}

fn default_language() -> String {
    let locale = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey("Control Panel\\International")
        .and_then(|key| key.get_value::<String, _>("LocaleName"));
    match locale {
        Ok(name) if name.to_lowercase().starts_with("zh") => "zh".to_string(),
        Ok(_) => "en".to_string(),
        Err(_) => "zh".to_string(),
    }
}

fn settings_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|err| format!("cannot resolve app config dir: {err}"))?;
    fs::create_dir_all(&dir).map_err(|err| format!("cannot create {}: {err}", dir.display()))?;
    Ok(dir.join(SETTINGS_FILE))
}

fn keyring_entry() -> Result<Entry, keyring::Error> {
    Entry::new(KEYRING_SERVICE, KEYRING_ACCOUNT)
}

fn has_stored_api_key() -> bool {
    keyring_entry()
        .and_then(|entry| entry.get_password())
        .map(|secret| !secret.is_empty())
        .unwrap_or(false)
}

/// Reads the API key from Windows Credential Manager. The key never travels
/// to the frontend; only backend HTTP calls use it.
pub fn stored_api_key() -> Option<String> {
    keyring_entry()
        .ok()
        .and_then(|entry| entry.get_password().ok())
        .filter(|secret| !secret.is_empty())
}

pub fn save(app: &AppHandle, settings: &AppSettings) -> Result<(), String> {
    let path = settings_path(app)?;
    let json = serde_json::to_string_pretty(settings)
        .map_err(|err| format!("cannot serialize settings: {err}"))?;
    fs::write(&path, json).map_err(|err| format!("cannot write {}: {err}", path.display()))
}

pub fn load_or_init(app: &AppHandle) -> Result<AppSettings, String> {
    let path = settings_path(app)?;
    let mut settings = if path.exists() {
        match fs::read_to_string(&path)
            .map_err(|err| err.to_string())
            .and_then(|text| {
                let text = text.trim_start_matches('\u{feff}');
                serde_json::from_str::<AppSettings>(text).map_err(|err| err.to_string())
            })
        {
            Ok(settings) => settings,
            Err(err) => {
                // A broken settings file must not stop the app from starting.
                log::warn!("settings file is unreadable, using defaults: {err}");
                AppSettings::default()
            }
        }
    } else {
        let mut fresh = AppSettings::default();
        let report = legacy::import_into(&mut fresh);
        if !report.migrated_keys.iter().any(|key| key == "AutoUpdateGuard") {
            // Same first-run rule as the Qt build: keep updates on only when the
            // machine's update configuration is actually intact.
            fresh.auto_update_guard = !crate::sysopt::query_status().blocked;
        }
        log_report(&report);
        save(app, &fresh)?;
        fresh
    };

    settings.ai.has_api_key = has_stored_api_key();
    Ok(settings)
}

pub fn import_legacy(app: &AppHandle) -> Result<ImportReport, String> {
    let mut settings = load_or_init(app)?;
    let report = legacy::import_into(&mut settings);
    settings.ai.has_api_key = has_stored_api_key();
    save(app, &settings)?;
    log_report(&report);
    Ok(report)
}

pub fn set_api_key(app: &AppHandle, api_key: &str) -> Result<(), String> {
    let secret = api_key.trim();
    if secret.is_empty() {
        return Err("API key is empty".to_string());
    }
    keyring_entry()
        .and_then(|entry| entry.set_password(secret))
        .map_err(|err| format!("cannot write Windows credential: {err}"))?;

    // The key now lives in Credential Manager; drop the legacy plaintext copy.
    if let Ok(key) = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey_with_flags(LEGACY_KEY, KEY_SET_VALUE)
    {
        let _ = key.delete_value(LEGACY_AI_KEY);
    }

    let mut settings = load_or_init(app)?;
    settings.ai.has_api_key = true;
    save(app, &settings)
}

pub fn clear_api_key(app: &AppHandle) -> Result<(), String> {
    if let Ok(entry) = keyring_entry() {
        match entry.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => {}
            Err(err) => return Err(format!("cannot delete Windows credential: {err}")),
        }
    }
    let mut settings = load_or_init(app)?;
    settings.ai.has_api_key = false;
    save(app, &settings)
}

fn log_report(report: &ImportReport) {
    if report.imported {
        log::info!("imported legacy settings: {:?}", report.migrated_keys);
    } else {
        log::info!("no legacy settings to import");
    }
    for warning in &report.warnings {
        log::warn!("legacy settings: {warning}");
    }
}
