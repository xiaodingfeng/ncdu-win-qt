use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter};

use crate::{config, settings};

static BUSY: AtomicBool = AtomicBool::new(false);
static CANCEL: AtomicBool = AtomicBool::new(false);

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AiConfigInfo {
    pub base_url: String,
    pub model: String,
    pub has_api_key: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AiDefaultReport {
    pub base_url: String,
    pub api_key_set: bool,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatMessage {
    pub role: String,
    pub content: String,
}

#[derive(Debug, Clone, Serialize)]
struct AiChunk {
    delta: String,
}

#[derive(Debug, Clone, Serialize)]
struct AiError {
    message: String,
}

fn config_info(app: &AppHandle) -> Result<AiConfigInfo, String> {
    let settings = settings::load_or_init(app)?;
    let cfg = config::load_or_init(app);
    Ok(AiConfigInfo {
        base_url: if settings.ai.base_url.trim().is_empty() {
            cfg.ai.base_url
        } else {
            settings.ai.base_url
        },
        model: if settings.ai.model.trim().is_empty() {
            cfg.ai.model
        } else {
            settings.ai.model
        },
        has_api_key: settings::stored_api_key().is_some(),
    })
}

#[tauri::command]
pub fn ai_get_config(app: AppHandle) -> Result<AiConfigInfo, String> {
    config_info(&app)
}

#[tauri::command]
pub fn ai_save_config(
    app: AppHandle,
    base_url: String,
    model: String,
    api_key: Option<String>,
    clear_key: bool,
) -> Result<AiConfigInfo, String> {
    let mut settings = settings::load_or_init(&app)?;
    let cfg = config::load_or_init(&app);
    settings.ai.base_url = base_url.trim().trim_end_matches('/').to_string();
    settings.ai.model = if model.trim().is_empty() {
        cfg.ai.model
    } else {
        model.trim().to_string()
    };
    settings.ai.has_api_key = settings::stored_api_key().is_some();
    settings::save(&app, &settings)?;

    if clear_key {
        settings::clear_api_key(&app)?;
    } else if let Some(key) = api_key {
        if !key.trim().is_empty() {
            settings::set_api_key(&app, &key)?;
        }
    }
    config_info(&app)
}

#[tauri::command]
pub async fn ai_fetch_defaults(app: AppHandle) -> Result<AiDefaultReport, String> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(|err| err.to_string())?;
    let cfg = config::load_or_init(&app);
    let base = cfg.ai_base_endpoint();
    let key = cfg.ai_key_endpoint();
    let (base_result, key_result) = tokio::join!(client.get(base).send(), client.get(key).send());

    let mut report = AiDefaultReport {
        base_url: String::new(),
        api_key_set: false,
        error: None,
    };
    let mut settings = settings::load_or_init(&app)?;

    match base_result {
        Ok(response) if response.status().is_success() => {
            let text = response.text().await.unwrap_or_default().trim().to_string();
            if !text.is_empty() {
                report.base_url = text.clone();
                if settings.ai.base_url.trim().is_empty() {
                    settings.ai.base_url = text.trim_end_matches('/').to_string();
                }
            }
        }
        Ok(response) => {
            report.error = Some(format!("HTTP {}", response.status()));
        }
        Err(err) => {
            report.error = Some(err.to_string());
        }
    }

    if settings::stored_api_key().is_none() {
        if let Ok(response) = key_result {
            if response.status().is_success() {
                let secret = response.text().await.unwrap_or_default().trim().to_string();
                if !secret.is_empty() {
                    settings::set_api_key(&app, &secret)?;
                    report.api_key_set = true;
                    settings = settings::load_or_init(&app)?;
                }
            }
        }
    }

    settings.ai.has_api_key = settings::stored_api_key().is_some();
    settings::save(&app, &settings)?;
    Ok(report)
}

async fn fetch_model_ids(base: &str, key: &str) -> Result<Vec<String>, String> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(30))
        .build()
        .map_err(|err| err.to_string())?;
    let response = client
        .get(format!("{base}/models"))
        .bearer_auth(key)
        .send()
        .await
        .map_err(|err| err.to_string())?;
    if !response.status().is_success() {
        return Err(format!("HTTP {}", response.status()));
    }
    let value: serde_json::Value = response.json().await.map_err(|err| err.to_string())?;
    // Different providers wrap the list differently and some return plain
    // strings instead of objects, so accept every common shape and dedupe.
    let mut ids: Vec<String> = Vec::new();
    for key in ["data", "models", "result"] {
        let Some(items) = value.get(key).and_then(|list| list.as_array()) else {
            continue;
        };
        for item in items {
            let id = item
                .as_str()
                .map(|text| text.to_string())
                .or_else(|| item.get("id").and_then(|id| id.as_str()).map(|id| id.to_string()))
                .or_else(|| {
                    item.get("name")
                        .and_then(|name| name.as_str())
                        .map(|name| name.to_string())
                })
                .unwrap_or_default();
            let id = id.trim().to_string();
            if !id.is_empty() && !ids.iter().any(|existing| existing == &id) {
                ids.push(id);
            }
        }
    }
    Ok(ids)
}

#[tauri::command]
pub async fn ai_fetch_models(
    app: AppHandle,
    base_url: Option<String>,
    api_key: Option<String>,
) -> Result<Vec<String>, String> {
    let settings = settings::load_or_init(&app)?;
    // Use the values typed in the settings dialog so a new endpoint can be
    // tested before it is saved.
    let base = base_url
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| settings.ai.base_url.trim().to_string())
        .trim_end_matches('/')
        .to_string();
    if base.is_empty() {
        return Err("empty base URL".to_string());
    }
    let key = api_key
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
        .or_else(settings::stored_api_key)
        .unwrap_or_default();
    fetch_model_ids(&base, &key).await
}

#[tauri::command]
pub fn ai_chat(
    app: AppHandle,
    messages: Vec<ChatMessage>,
    model: Option<String>,
) -> Result<(), String> {
    if BUSY.swap(true, Ordering::SeqCst) {
        return Err("another AI request is already running".to_string());
    }
    CANCEL.store(false, Ordering::SeqCst);

    tauri::async_runtime::spawn(async move {
        let result = run_chat(app.clone(), messages, model).await;
        BUSY.store(false, Ordering::SeqCst);
        match result {
            Ok(()) => {
                let _ = app.emit("ai://done", ());
            }
            Err(err) => {
                let _ = app.emit("ai://error", AiError { message: err });
            }
        }
    });
    Ok(())
}

async fn run_chat(
    app: AppHandle,
    messages: Vec<ChatMessage>,
    override_model: Option<String>,
) -> Result<(), String> {
    let settings = settings::load_or_init(&app)?;
    let base = settings.ai.base_url.trim().trim_end_matches('/').to_string();
    if base.is_empty() {
        return Err("empty base URL".to_string());
    }
    let key = settings::stored_api_key().unwrap_or_default();
    if key.is_empty() {
        return Err("empty API key".to_string());
    }

    let cfg = config::load_or_init(&app);
    let mut model = override_model
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty() && *value != cfg.ai.model)
        .unwrap_or_else(|| settings.ai.model.trim().to_string());
    if model.is_empty() || model == cfg.ai.model {
        let ids = fetch_model_ids(&base, &key).await?;
        model = ids
            .into_iter()
            .next()
            .ok_or_else(|| "no models available".to_string())?;
    }

    let body = serde_json::json!({
        "model": model,
        "messages": messages,
        "stream": true,
    });
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(300))
        .build()
        .map_err(|err| err.to_string())?;
    let response = client
        .post(format!("{base}/chat/completions"))
        .bearer_auth(key)
        .json(&body)
        .send()
        .await
        .map_err(|err| err.to_string())?;

    if !response.status().is_success() {
        let status = response.status();
        let text = response.text().await.unwrap_or_default();
        return Err(format!("HTTP {status}: {text}"));
    }

    let mut stream = response.bytes_stream();
    let mut buffer: Vec<u8> = Vec::new();
    let mut done = false;

    use futures_util::StreamExt;
    while let Some(chunk) = stream.next().await {
        if CANCEL.load(Ordering::SeqCst) {
            break;
        }
        let bytes = chunk.map_err(|err| err.to_string())?;
        buffer.extend_from_slice(&bytes);
        while let Some((position, separator)) = find_event_end(&buffer) {
            let event = buffer.drain(..position + separator).collect::<Vec<u8>>();
            let text = String::from_utf8_lossy(&event).replace("\r\n", "\n");
            for line in text.lines() {
                let Some(data) = line.strip_prefix("data:") else {
                    continue;
                };
                let data = data.trim();
                if data.is_empty() {
                    continue;
                }
                if data == "[DONE]" {
                    done = true;
                    break;
                }
                let Ok(value) = serde_json::from_str::<serde_json::Value>(data) else {
                    continue;
                };
                if let Some(delta) = value
                    .get("choices")
                    .and_then(|choices| choices.get(0))
                    .and_then(|choice| choice.get("delta"))
                    .and_then(|delta| delta.get("content"))
                    .and_then(|content| content.as_str())
                {
                    if !delta.is_empty() {
                        let _ = app.emit("ai://chunk", AiChunk { delta: delta.to_string() });
                    }
                }
            }
            if done {
                break;
            }
        }
        if done {
            break;
        }
    }
    Ok(())
}

/// Locates the next SSE event boundary, returning its offset and length.
/// Servers may terminate events with `\n\n` or `\r\n\r\n`.
fn find_event_end(buffer: &[u8]) -> Option<(usize, usize)> {
    let mut index = 0;
    while index < buffer.len() {
        if buffer[index] == b'\n'
            && buffer.get(index + 1) == Some(&b'\n')
        {
            return Some((index, 2));
        }
        if buffer[index] == b'\r'
            && buffer.get(index + 1) == Some(&b'\n')
            && buffer.get(index + 2) == Some(&b'\r')
            && buffer.get(index + 3) == Some(&b'\n')
        {
            return Some((index, 4));
        }
        index += 1;
    }
    None
}

#[tauri::command]
pub fn ai_cancel() -> Result<(), String> {
    CANCEL.store(true, Ordering::SeqCst);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn serve_json(body: &'static str) -> (String, std::thread::JoinHandle<()>) {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let handle = std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut buffer = [0u8; 2048];
                let _ = stream.read(&mut buffer);
                let header = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = stream.write_all(header.as_bytes());
                let _ = stream.write_all(body.as_bytes());
            }
        });
        (format!("http://{address}/v1"), handle)
    }

    #[test]
    fn model_list_accepts_models_array_and_dedupes() {
        let (base, server) = serve_json(r#"{"models":[{"id":"a"},{"id":"b"},{"id":"a"}]}"#);
        let ids = tauri::async_runtime::block_on(fetch_model_ids(&base, "test")).unwrap();
        assert_eq!(ids, vec!["a".to_string(), "b".to_string()]);
        server.join().unwrap();
    }

    #[test]
    fn model_list_accepts_result_strings() {
        let (base, server) = serve_json(r#"{"result":["x","y"]}"#);
        let ids = tauri::async_runtime::block_on(fetch_model_ids(&base, "test")).unwrap();
        assert_eq!(ids, vec!["x".to_string(), "y".to_string()]);
        server.join().unwrap();
    }

    #[test]
    fn sse_boundaries_accept_lf_and_crlf() {
        assert_eq!(find_event_end(b"data: a\n\n"), Some((7, 2)));
        assert_eq!(find_event_end(b"data: a\r\n\r\n"), Some((7, 4)));
        assert_eq!(find_event_end(b"data: a\r\n"), None);
        assert_eq!(find_event_end(b"data: a\n"), None);
    }
}
