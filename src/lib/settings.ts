import { invoke } from "@tauri-apps/api/core";
import type { AppSettings, ImportReport } from "../types/settings";

export function loadSettings(): Promise<AppSettings> {
  return invoke<AppSettings>("load_settings");
}

export function saveSettings(settings: AppSettings): Promise<void> {
  return invoke("save_settings", { settings });
}

export function importLegacySettings(): Promise<ImportReport> {
  return invoke<ImportReport>("import_legacy_settings");
}

export function setAiApiKey(apiKey: string): Promise<void> {
  return invoke("set_ai_api_key", { apiKey });
}

export function clearAiApiKey(): Promise<void> {
  return invoke("clear_ai_api_key");
}
