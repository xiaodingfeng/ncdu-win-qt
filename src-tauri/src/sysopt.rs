use std::fs;
use std::os::windows::process::CommandExt;
use std::path::PathBuf;
use std::process::Command;

use serde::Serialize;
use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ};
use winreg::RegKey;

use crate::elevate::{self, HelperOp};

const AU_KEY: &str = "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
const WU_KEY: &str = "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate";
const LEGACY_KEY: &str = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update";
const SERVICES_KEY: &str = "SYSTEM\\CurrentControlSet\\Services";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateStatus {
    pub blocked: bool,
    pub reasons: Vec<String>,
    pub service_disabled: bool,
    pub policy_blocked: bool,
    pub hosts_blocked: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct JobResult {
    pub ok: bool,
    pub needs_reboot: bool,
    pub message: String,
    pub total: u32,
}

fn read_dword(key: &str, name: &str) -> Option<u32> {
    RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(key, KEY_READ)
        .ok()
        .and_then(|key| key.get_value::<u32, _>(name).ok())
}

fn read_string(key: &str, name: &str) -> Option<String> {
    RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(key, KEY_READ)
        .ok()
        .and_then(|key| key.get_value::<String, _>(name).ok())
}

fn service_disabled(name: &str) -> bool {
    read_dword(&format!("{SERVICES_KEY}\\{name}"), "Start") == Some(4)
}

fn hosts_blocks_updates() -> bool {
    let Some(windir) = std::env::var_os("WINDIR") else {
        return false;
    };
    let path = PathBuf::from(windir).join("System32/drivers/etc/hosts");
    let Ok(text) = fs::read_to_string(path) else {
        return false;
    };
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim().to_lowercase();
        if line.is_empty() {
            continue;
        }
        if line.contains("windowsupdate") || line.contains("update.microsoft") || line.contains("delivery.mp.microsoft") {
            return true;
        }
    }
    false
}

pub(crate) fn query_status() -> UpdateStatus {
    let mut reasons = Vec::new();

    let wu = service_disabled("wuauserv");
    let uso = service_disabled("UsoSvc");
    let bits = service_disabled("BITS");
    if wu {
        reasons.push("sysopt.reason.svc_wu".to_string());
    }
    if uso {
        reasons.push("sysopt.reason.svc_uso".to_string());
    }
    if bits {
        reasons.push("sysopt.reason.svc_bits".to_string());
    }

    let mut policy_blocked = false;
    if read_dword(AU_KEY, "NoAutoUpdate") == Some(1) {
        reasons.push("sysopt.reason.policy_noauto".to_string());
        policy_blocked = true;
    }
    if read_dword(WU_KEY, "DoNotConnectToWindowsUpdateInternetLocations") == Some(1) {
        reasons.push("sysopt.reason.policy_nonet".to_string());
        policy_blocked = true;
    }
    if read_dword(WU_KEY, "DisableWindowsUpdateAccess") == Some(1) {
        reasons.push("sysopt.reason.policy_noaccess".to_string());
        policy_blocked = true;
    }
    if read_dword(WU_KEY, "SetDisableUXWUAccess") == Some(1) {
        reasons.push("sysopt.reason.policy_noux".to_string());
        policy_blocked = true;
    }
    if read_dword(LEGACY_KEY, "AUOptions") == Some(1) {
        reasons.push("sysopt.reason.auoptions".to_string());
        policy_blocked = true;
    }
    if read_dword(LEGACY_KEY, "NoAutoUpdate") == Some(1) {
        reasons.push("sysopt.reason.legacy".to_string());
        policy_blocked = true;
    }
    if read_dword(AU_KEY, "UseWUServer") == Some(1) {
        let server = read_string(WU_KEY, "WUServer").unwrap_or_default().to_lowercase();
        if server.is_empty()
            || server.contains("127.0.0.1")
            || server.contains("localhost")
            || server.contains("0.0.0.0")
        {
            reasons.push("sysopt.reason.wsus".to_string());
            policy_blocked = true;
        }
    }

    let hosts = hosts_blocks_updates();
    if hosts {
        reasons.push("sysopt.reason.hosts".to_string());
    }

    let blocked = wu || uso || bits || policy_blocked || hosts;
    UpdateStatus {
        blocked,
        reasons,
        service_disabled: wu || uso || bits,
        policy_blocked,
        hosts_blocked: hosts,
    }
}

const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const CREATE_UNICODE_ENVIRONMENT: u32 = 0x0000_0400;

#[tauri::command]
pub async fn get_update_status() -> Result<UpdateStatus, String> {
    run_blocking(query_status).await
}

async fn run_blocking<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|err| err.to_string())
}

// ---------------------------------------------------------------------------
// Elevated execution through the restricted Rust helper
// ---------------------------------------------------------------------------

fn helper_job(op: HelperOp) -> JobResult {
    match elevate::run_elevated(op) {
        Ok(outcome) => JobResult {
            ok: outcome.ok,
            needs_reboot: outcome.needs_reboot,
            message: outcome.message,
            total: outcome.total,
        },
        Err(err) => JobResult {
            ok: false,
            needs_reboot: false,
            message: format!("elevation was cancelled or the helper failed: {err}"),
            total: 0,
        },
    }
}

#[tauri::command]
pub async fn set_update_enabled(enable: bool) -> Result<JobResult, String> {
    let mut result = run_blocking(move || helper_job(HelperOp::SetUpdateEnabled { enable })).await?;
    if result.ok {
        result.message = if enable {
            "sysopt.update.repaired".to_string()
        } else {
            "sysopt.task_done".to_string()
        };
    }
    Ok(result)
}

#[tauri::command]
pub async fn run_network_fix() -> Result<JobResult, String> {
    run_blocking(|| helper_job(HelperOp::NetworkFix)).await
}

#[tauri::command]
pub async fn flush_dns() -> Result<JobResult, String> {
    let mut result = run_blocking(|| helper_job(HelperOp::FlushDns)).await?;
    if result.ok {
        result.message = "sysopt.task_done".to_string();
    }
    Ok(result)
}

#[tauri::command]
pub async fn reset_store() -> Result<JobResult, String> {
    run_blocking(|| {
        let windir = std::env::var_os("WINDIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("C:/Windows"));
        let wsreset = windir.join("System32/wsreset.exe");
        if !wsreset.exists() {
            return JobResult {
                ok: false,
                needs_reboot: false,
                message: "wsreset.exe not found".to_string(),
                total: 0,
            };
        }
        match Command::new(wsreset)
            .creation_flags(CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT)
            .spawn()
        {
            Ok(_) => JobResult {
                ok: true,
                needs_reboot: false,
                message: "sysopt.store.launched".to_string(),
                total: 1,
            },
            Err(err) => JobResult {
                ok: false,
                needs_reboot: false,
                message: err.to_string(),
                total: 0,
            },
        }
    })
    .await
}
