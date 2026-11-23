use std::io::Write;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use futures_util::StreamExt;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter};
use windows_sys::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
use windows_sys::Win32::Storage::FileSystem::SYNCHRONIZE;
use windows_sys::Win32::System::Threading::{OpenProcess, WaitForSingleObject};

use crate::config;

const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const APPLY_UPDATE_FLAG: &str = "--apply-update";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateCheck {
    pub has_update: bool,
    pub remote: String,
    pub local: String,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FeedbackResult {
    pub ok: bool,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct UpdateProgress {
    percent: u32,
    received: u64,
    total: u64,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct UpdateError {
    message: String,
}

/// Blocks until `pid` exits (or the timeout elapses); used by the staged
/// updater, which must wait for the running app to release its files.
fn wait_for_exit(pid: u32, timeout: Duration) -> bool {
    if pid == 0 {
        return true;
    }
    unsafe {
        let handle = OpenProcess(SYNCHRONIZE, 0, pid);
        if handle.is_null() {
            return true;
        }
        let waited = WaitForSingleObject(handle, timeout.as_millis() as u32);
        CloseHandle(handle);
        waited == WAIT_OBJECT_0
    }
}

/// Runs inside the temporary copy of this executable: waits for the app to
/// quit, installs the downloaded package silently into the directory the app
/// runs from, then starts the new version again.
pub(crate) fn apply_update_from_args() -> Option<i32> {
    let args: Vec<String> = std::env::args().collect();
    let index = args.iter().position(|arg| arg == APPLY_UPDATE_FLAG)?;
    let installer = args.get(index + 1)?.clone();
    let install_dir = args.get(index + 2)?.clone();
    let app_exe = args.get(index + 3)?.clone();
    let parent = args
        .get(index + 4)
        .and_then(|value| value.parse::<u32>().ok())
        .unwrap_or(0);

    wait_for_exit(parent, Duration::from_secs(120));

    // `/D=` must stay last and unquoted; NSIS reads the rest of the command
    // line as the install directory, which keeps the update in place.
    let installed = Command::new(&installer)
        .arg("/S")
        .raw_arg(format!("/D={install_dir}"))
        .creation_flags(CREATE_NO_WINDOW)
        .status()
        .map(|status| status.success())
        .unwrap_or(false);
    let _ = std::fs::remove_file(&installer);
    if installed {
        let _ = Command::new(&app_exe).spawn();
    }
    Some(if installed { 0 } else { 1 })
}

/// Stages a copy of this executable in %TEMP% (the installed files are about to
/// be replaced) and hands the installer to it.
fn launch_staged_update(installer: &Path) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|err| err.to_string())?;
    let install_dir = exe
        .parent()
        .ok_or_else(|| "cannot locate the install directory".to_string())?;
    let staged = std::env::temp_dir().join(format!("ncduwin-update-{}.exe", std::process::id()));
    std::fs::copy(&exe, &staged).map_err(|err| format!("cannot stage the updater: {err}"))?;
    Command::new(&staged)
        .arg(APPLY_UPDATE_FLAG)
        .arg(installer)
        .arg(install_dir)
        .arg(&exe)
        .arg(std::process::id().to_string())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|err| format!("cannot start the updater: {err}"))?;
    Ok(())
}

fn valid_version(version: &str) -> bool {
    !version.is_empty()
        && version
            .chars()
            .all(|ch| ch.is_ascii_digit() || ch == '.' || ch == '-' || ch == '_')
}

/// Portable builds live in a folder extracted from `portable.zip` and are never
/// registered with the NSIS uninstaller. Installed builds always write
/// `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\NcduWin`.
#[tauri::command]
pub fn is_portable() -> bool {
    use winreg::enums::HKEY_CURRENT_USER;
    use winreg::RegKey;
    RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NcduWin")
        .is_err()
}

#[tauri::command]
pub fn download_update(app: AppHandle, version: String) -> Result<(), String> {
    if !valid_version(&version) {
        return Err("invalid version".to_string());
    }
    tauri::async_runtime::spawn(async move {
        let cfg = config::load_or_init(&app);
        let result = async {
            let client = reqwest::Client::builder()
                .timeout(Duration::from_secs(1800))
                .build()
                .map_err(|err| err.to_string())?;
            // The packaged installer is uploaded unchanged, so try the bundled
            // name first and the older renamed file as a fallback.
            let candidates = cfg.download_urls(&version);
            let mut response = None;
            let mut last_error = String::from("update file not found");
            for url in &candidates {
                match client.get(url).send().await {
                    Ok(resp) if resp.status().is_success() => {
                        log::info!("downloading update from {url}");
                        response = Some(resp);
                        break;
                    }
                    Ok(resp) => last_error = format!("HTTP {}", resp.status()),
                    Err(err) => last_error = err.to_string(),
                }
            }
            let response = response.ok_or(last_error)?;
            let temp: PathBuf = std::env::temp_dir().join(format!("{}_Setup.exe", cfg.name));
            download_to(response, &temp, |received, total| {
                let _ = app.emit(
                    "update://progress",
                    UpdateProgress {
                        percent: if total > 0 {
                            ((received as f64 / total as f64) * 100.0).min(100.0) as u32
                        } else {
                            0
                        },
                        received,
                        total,
                    },
                );
            })
            .await?;

            // Silent in-place update: the staged helper waits for this process
            // to exit, installs into the current directory and restarts it.
            launch_staged_update(&temp)?;
            app.exit(0);
            Ok(())
        }
        .await;
        if let Err(message) = result {
            let _ = app.emit("update://error", UpdateError { message });
        }
    });
    Ok(())
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FeedbackInput {
    pub title: String,
    pub content: String,
    pub contact: String,
    pub level: String,
}

fn parse_version(text: &str) -> Option<(u64, u64, u64)> {
    let parts: Vec<u64> = text
        .trim()
        .trim_start_matches('v')
        .split('.')
        .take(3)
        .filter_map(|part| part.parse::<u64>().ok())
        .collect();
    if parts.len() < 2 {
        return None;
    }
    Some((
        parts[0],
        *parts.get(1).unwrap_or(&0),
        *parts.get(2).unwrap_or(&0),
    ))
}

/// Streams a response to disk, reporting progress, and returns the byte count.
async fn download_to<F: FnMut(u64, u64)>(
    response: reqwest::Response,
    destination: &Path,
    mut on_progress: F,
) -> Result<u64, String> {
    let total = response.content_length().unwrap_or(0);
    let mut file = std::fs::File::create(destination).map_err(|err| err.to_string())?;
    let mut received = 0u64;
    let mut last_emit = 0u64;
    let mut stream = response.bytes_stream();
    while let Some(chunk) = stream.next().await {
        let bytes = chunk.map_err(|err| err.to_string())?;
        file.write_all(&bytes).map_err(|err| err.to_string())?;
        received += bytes.len() as u64;
        if received.saturating_sub(last_emit) > 256 * 1024 {
            last_emit = received;
            on_progress(received, total);
        }
    }
    drop(file);
    on_progress(received, if total == 0 { received } else { total });
    Ok(received)
}

async fn fetch_remote_version(client: &reqwest::Client, url: &str) -> Result<String, String> {
    let response = client.get(url).send().await.map_err(|err| err.to_string())?;
    if !response.status().is_success() {
        return Err(format!("HTTP {}", response.status()));
    }
    Ok(response.text().await.unwrap_or_default().trim().to_string())
}

fn evaluate_update(local: &str, remote: &str) -> UpdateCheck {
    let remote_version = parse_version(remote);
    let local_version = parse_version(local);
    let has_update = match (remote_version, local_version) {
        (Some(remote), Some(local)) => remote > local,
        _ => false,
    };
    UpdateCheck {
        has_update,
        remote: remote.to_string(),
        local: local.to_string(),
        error: if remote_version.is_none() {
            Some("Invalid version response".to_string())
        } else {
            None
        },
    }
}

#[tauri::command]
pub async fn check_update(app: AppHandle) -> Result<UpdateCheck, String> {
    let cfg = config::load_or_init(&app);
    let local = cfg.effective_version(&app);
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()
        .map_err(|err| err.to_string())?;
    match fetch_remote_version(&client, &cfg.version_url()).await {
        Ok(remote) => Ok(evaluate_update(&local, &remote)),
        Err(err) => Ok(UpdateCheck {
            has_update: false,
            remote: String::new(),
            local,
            error: Some(err),
        }),
    }
}

#[tauri::command]
pub async fn submit_feedback(app: AppHandle, input: FeedbackInput) -> Result<FeedbackResult, String> {
    if input.content.trim().is_empty() {
        return Ok(FeedbackResult {
            ok: false,
            error: Some("empty content".to_string()),
        });
    }
    let cfg = config::load_or_init(&app);
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(|err| err.to_string())?;
    let mut body = serde_json::json!({
        "app": cfg.name,
        "version": cfg.effective_version(&app),
        "content": input.content,
        "level": if input.level.is_empty() { "info" } else { &input.level },
        "device": format!("Windows / {}", std::env::consts::ARCH),
        "extra": { "os": "windows", "arch": std::env::consts::ARCH },
    });
    if !input.title.trim().is_empty() {
        body["title"] = serde_json::Value::String(input.title);
    }
    if !input.contact.trim().is_empty() {
        body["contact"] = serde_json::Value::String(input.contact);
    }
    let response = client
        .post(&cfg.feedback_url)
        .header("X-API-Key", &cfg.feedback_api_key)
        .json(&body)
        .send()
        .await
        .map_err(|err| err.to_string())?;
    if !response.status().is_success() {
        return Ok(FeedbackResult {
            ok: false,
            error: Some(format!("HTTP {}", response.status())),
        });
    }
    let value: serde_json::Value = response.json().await.unwrap_or(serde_json::Value::Null);
    let code = value.get("code").and_then(|code| code.as_i64()).unwrap_or(-1);
    Ok(FeedbackResult {
        ok: code == 0,
        error: if code == 0 {
            None
        } else {
            Some(format!("server code {code}"))
        },
    })
}

/// Receives front-end diagnostics so UI errors land in the same log file.
#[tauri::command]
pub fn log_client(level: String, message: String) {
    let text: String = message.chars().take(4000).collect();
    match level.as_str() {
        "error" => log::error!("[ui] {text}"),
        "warn" => log::warn!("[ui] {text}"),
        "debug" => log::debug!("[ui] {text}"),
        _ => log::info!("[ui] {text}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_parser_compares_numerically() {
        assert!(parse_version("1.2.0") > parse_version("1.1.9"));
        assert!(parse_version("v2.0") > parse_version("1.99.99"));
        assert_eq!(parse_version("1.2"), Some((1, 2, 0)));
        assert_eq!(parse_version("garbage"), None);
    }

    #[test]
    fn update_version_input_is_sanitised() {
        assert!(valid_version("1.2.0"));
        assert!(valid_version("1.2.0_1"));
        assert!(!valid_version(""));
        assert!(!valid_version("1.2.0-beta"));
        assert!(!valid_version("../../evil"));
        assert!(!valid_version("https://evil"));
    }

    /// Serves one HTTP request with the given body (tiny local test server).
    fn serve_once(body: Vec<u8>) -> (String, std::thread::JoinHandle<()>) {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let handle = std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut buffer = [0u8; 2048];
                let _ = stream.read(&mut buffer);
                let header = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = stream.write_all(header.as_bytes());
                let _ = stream.write_all(&body);
                let _ = stream.flush();
            }
        });
        (format!("http://{address}/version"), handle)
    }

    #[test]
    fn local_update_server_reports_a_newer_version() {
        let (url, server) = serve_once(b"1.2.0".to_vec());
        let client = reqwest::Client::new();
        let remote =
            tauri::async_runtime::block_on(fetch_remote_version(&client, &url)).unwrap();
        let check = evaluate_update("1.1.0", &remote);
        assert!(check.has_update);
        assert_eq!(check.remote, "1.2.0");
        assert!(check.error.is_none());
        server.join().unwrap();
    }

    #[test]
    fn local_update_server_writes_exact_bytes() {
        let payload: Vec<u8> = (0..4096u32).map(|value| (value % 251) as u8).collect();
        let (url, server) = serve_once(payload.clone());
        let destination =
            std::env::temp_dir().join(format!("ncduwin-update-test-{}.exe", std::process::id()));
        let client = reqwest::Client::new();
        let written = tauri::async_runtime::block_on(async {
            let response = client.get(&url).send().await.unwrap();
            download_to(response, &destination, |_, _| {}).await
        })
        .unwrap();
        assert_eq!(written, payload.len() as u64);
        assert_eq!(std::fs::read(&destination).unwrap(), payload);
        let _ = std::fs::remove_file(&destination);
        server.join().unwrap();
    }
}
