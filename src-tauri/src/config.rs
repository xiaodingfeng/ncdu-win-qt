use serde::{Deserialize, Serialize};
use tauri::AppHandle;

/// Build-time configuration bundled into the executable. Editing this file
/// requires a rebuild, so end users cannot change branding or service URLs.
const CONFIG_JSON: &str = include_str!("../app-config.json");

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AiConfig {
    /// Default API base URL. Empty means "ask the remote endpoint below".
    pub base_url: String,
    /// Default model name. "auto" lets the backend pick the first model.
    pub model: String,
    /// Endpoint that returns the default API base URL.
    pub base_url_endpoint: String,
    /// Endpoint that returns the default API key.
    pub api_key_endpoint: String,
}

impl Default for AiConfig {
    fn default() -> Self {
        Self {
            base_url: String::new(),
            model: "auto".to_string(),
            base_url_endpoint: "{homepage}aiBaseUrl".to_string(),
            api_key_endpoint: "{homepage}aiApiKey".to_string(),
        }
    }
}

/// Application metadata and service endpoints supplied at build time.
///
/// Templates accept `{name}`, `{homepage}` and `{version}`.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AppConfig {
    pub name: String,
    pub homepage: String,
    pub source_url: String,
    pub download_page: String,
    pub download_url_template: String,
    pub version_check_url: String,
    pub feedback_url: String,
    pub feedback_api_key: String,
    /// Overrides the version reported by the binary when non-empty.
    pub version: String,
    pub version_info: String,
    pub copyright: String,
    pub ai: AiConfig,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            name: "NcduWin".to_string(),
            homepage: "https://ncduwin.fengzhengx.cn/".to_string(),
            source_url: "https://github.com/xiaodingfeng/ncdu-win-qt".to_string(),
            download_page: "https://ncduwin.fengzhengx.cn/".to_string(),
            download_url_template: "{homepage}downloads/{name}_{version}_x64-setup.exe".to_string(),
            version_check_url: "{homepage}version".to_string(),
            feedback_url: "https://notify.fengzhengx.cn/api/feedback".to_string(),
            feedback_api_key: "ncduwin".to_string(),
            version: String::new(),
            version_info: String::new(),
            copyright: String::new(),
            ai: AiConfig::default(),
        }
    }
}

impl AppConfig {
    pub fn expand(&self, template: &str, version: &str) -> String {
        template
            .replace("{homepage}", &self.homepage)
            .replace("{name}", &self.name)
            .replace("{version}", version)
    }

    pub fn download_url(&self, version: &str) -> String {
        self.expand(&self.download_url_template, version)
    }

    /// Candidate download URLs, newest naming first. The packaged installer is
    /// uploaded untouched as `{name}_{version}_x64-setup.exe`; the older
    /// renamed `{name}_{version}_Setup.exe` stays as a fallback.
    pub fn download_urls(&self, version: &str) -> Vec<String> {
        let mut urls = vec![self.download_url(version)];
        let legacy = self.expand("{homepage}downloads/{name}_{version}_Setup.exe", version);
        if !urls.contains(&legacy) {
            urls.push(legacy);
        }
        urls
    }

    pub fn version_url(&self) -> String {
        self.expand(&self.version_check_url, "")
    }

    pub fn ai_base_endpoint(&self) -> String {
        self.expand(&self.ai.base_url_endpoint, "")
    }

    pub fn ai_key_endpoint(&self) -> String {
        self.expand(&self.ai.api_key_endpoint, "")
    }

    pub fn effective_version(&self, app: &AppHandle) -> String {
        if self.version.trim().is_empty() {
            app.package_info().version.to_string()
        } else {
            self.version.trim().to_string()
        }
    }
}

/// Returns the configuration compiled into the binary.
///
/// A malformed file falls back to defaults instead of failing the command.
pub fn load_or_init(_app: &AppHandle) -> AppConfig {
    serde_json::from_str(CONFIG_JSON).unwrap_or_default()
}

#[tauri::command]
pub fn app_config(app: AppHandle) -> Result<AppConfig, String> {
    Ok(load_or_init(&app))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn download_urls_use_the_packaged_name_and_keep_a_legacy_fallback() {
        let cfg = AppConfig::default();
        let urls = cfg.download_urls("1.2.0");
        assert_eq!(
            urls[0],
            "https://ncduwin.fengzhengx.cn/downloads/NcduWin_1.2.0_x64-setup.exe"
        );
        assert_eq!(
            urls[1],
            "https://ncduwin.fengzhengx.cn/downloads/NcduWin_1.2.0_Setup.exe"
        );
        assert_eq!(urls.len(), 2);
    }

    #[test]
    fn download_urls_do_not_duplicate_a_custom_template() {
        let mut cfg = AppConfig::default();
        cfg.download_url_template =
            "{homepage}downloads/{name}_{version}_Setup.exe".to_string();
        assert_eq!(cfg.download_urls("1.2.0").len(), 1);
    }
}
