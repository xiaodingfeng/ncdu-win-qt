use std::collections::BTreeMap;

use keyring::Entry;
use serde::Serialize;
use winreg::enums::{HKEY_CURRENT_USER, KEY_SET_VALUE, RegType};
use winreg::RegKey;

use crate::settings::{
    AppSettings, CustomTheme, KEYRING_ACCOUNT, KEYRING_SERVICE,
};

const LEGACY_KEY: &str = "Software\\NcduWin";
const LEGACY_AI_KEY: &str = "aiApiKey";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ImportReport {
    pub imported: bool,
    pub migrated_keys: Vec<String>,
    pub warnings: Vec<String>,
}

#[derive(Default)]
struct LegacyValues {
    language: Option<String>,
    theme: Option<String>,
    theme_custom: Option<String>,
    default_move_path: Option<String>,
    auto_update_guard: Option<u32>,
    ai_base_url: Option<String>,
    ai_model: Option<String>,
    ai_api_key: Option<String>,
    move_custom_dirs: Option<Vec<String>>,
    move_scope_index: Option<u32>,
    move_target_root: Option<String>,
}

fn read_legacy() -> Result<LegacyValues, String> {
    let key = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey(LEGACY_KEY)
        .map_err(|err| format!("no legacy settings: {err}"))?;
    Ok(LegacyValues {
        language: read_string(&key, "language"),
        theme: read_string(&key, "theme"),
        theme_custom: read_string(&key, "themeCustom"),
        default_move_path: read_string(&key, "defaultMovePath"),
        auto_update_guard: read_u32(&key, "AutoUpdateGuard"),
        ai_base_url: read_string(&key, "aiBaseUrl"),
        ai_model: read_string(&key, "aiModel"),
        ai_api_key: read_string(&key, LEGACY_AI_KEY),
        move_custom_dirs: read_multi_sz(&key, "MoveScanCustomDirs"),
        move_scope_index: read_u32(&key, "MoveScanScopeIndex"),
        move_target_root: read_string(&key, "MoveTargetRoot"),
    })
}

fn read_string(key: &RegKey, name: &str) -> Option<String> {
    key.get_value::<String, _>(name).ok()
}

fn read_u32(key: &RegKey, name: &str) -> Option<u32> {
    key.get_value::<u32, _>(name).ok()
}

fn read_multi_sz(key: &RegKey, name: &str) -> Option<Vec<String>> {
    let raw = key.get_raw_value(name).ok()?;
    if raw.vtype != RegType::REG_MULTI_SZ {
        return None;
    }
    let units: Vec<u16> = raw
        .bytes
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect();
    let mut entries = Vec::new();
    let mut current = Vec::new();
    for unit in units {
        if unit == 0 {
            if !current.is_empty() {
                entries.push(String::from_utf16_lossy(&current));
                current.clear();
            }
        } else {
            current.push(unit);
        }
    }
    if !current.is_empty() {
        entries.push(String::from_utf16_lossy(&current));
    }
    Some(entries)
}

fn parse_custom(raw: &str) -> Option<CustomTheme> {
    let mut base = "dark".to_string();
    let mut colors = BTreeMap::new();
    for part in raw.split(';') {
        let Some((name, value)) = part.split_once('=') else {
            continue;
        };
        if name == "base" {
            if value == "light" || value == "dark" {
                base = value.to_string();
            }
        } else if value.starts_with('#') && value.len() == 7 {
            colors.insert(name.to_string(), value.to_string());
        }
    }
    if colors.is_empty() {
        return None;
    }
    Some(CustomTheme { base, colors })
}

fn delete_legacy_value(name: &str) {
    if let Ok(key) =
        RegKey::predef(HKEY_CURRENT_USER).open_subkey_with_flags(LEGACY_KEY, KEY_SET_VALUE)
    {
        let _ = key.delete_value(name);
    }
}

/// Copies `HKCU\Software\NcduWin` into the new settings model. The AI key goes
/// to Credential Manager and only then is the registry copy removed.
pub fn import_into(settings: &mut AppSettings) -> ImportReport {
    let mut report = ImportReport {
        imported: false,
        migrated_keys: Vec::new(),
        warnings: Vec::new(),
    };

    let legacy = match read_legacy() {
        Ok(legacy) => legacy,
        Err(err) => {
            report.warnings.push(err);
            return report;
        }
    };

    if let Some(language) = legacy.language {
        if language == "zh" || language == "en" {
            settings.language = language;
            report.migrated_keys.push("language".to_string());
        }
    }
    if let Some(theme) = legacy.theme {
        if matches!(theme.as_str(), "light" | "dark" | "system" | "custom") {
            settings.theme = theme;
            report.migrated_keys.push("theme".to_string());
        }
    }
    if let Some(raw) = legacy.theme_custom {
        if let Some(custom) = parse_custom(&raw) {
            settings.custom_theme = Some(custom);
            report.migrated_keys.push("themeCustom".to_string());
        }
    }
    if let Some(path) = legacy.default_move_path {
        settings.default_move_path = path;
        report.migrated_keys.push("defaultMovePath".to_string());
    }
    if let Some(guard) = legacy.auto_update_guard {
        settings.auto_update_guard = guard != 0;
        report.migrated_keys.push("AutoUpdateGuard".to_string());
    }
    if let Some(url) = legacy.ai_base_url {
        settings.ai.base_url = url;
        report.migrated_keys.push("aiBaseUrl".to_string());
    }
    if let Some(model) = legacy.ai_model {
        settings.ai.model = model;
        report.migrated_keys.push("aiModel".to_string());
    }
    if let Some(dirs) = legacy.move_custom_dirs {
        settings.move_scan.custom_dirs = dirs;
        report.migrated_keys.push("MoveScanCustomDirs".to_string());
    }
    if let Some(index) = legacy.move_scope_index {
        settings.move_scan.scope_index = index;
        report.migrated_keys.push("MoveScanScopeIndex".to_string());
    }
    if let Some(root) = legacy.move_target_root {
        settings.move_scan.last_target_root = root;
        report.migrated_keys.push("MoveTargetRoot".to_string());
    }

    if let Some(api_key) = legacy.ai_api_key {
        if !api_key.trim().is_empty() {
            let stored = Entry::new(KEYRING_SERVICE, KEYRING_ACCOUNT)
                .and_then(|entry| entry.set_password(api_key.trim()));
            match stored {
                Ok(()) => {
                    settings.ai.has_api_key = true;
                    delete_legacy_value(LEGACY_AI_KEY);
                    report.migrated_keys.push(LEGACY_AI_KEY.to_string());
                }
                Err(err) => report.warnings.push(format!(
                    "could not move the AI key to Credential Manager: {err}"
                )),
            }
        }
    }

    report.imported = !report.migrated_keys.is_empty();
    report
}
