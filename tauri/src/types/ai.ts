// Cross-layer contract with src-tauri/src/ai.rs.

export interface AiConfigInfo {
  baseUrl: string;
  model: string;
  hasApiKey: boolean;
}

export interface AiDefaultReport {
  baseUrl: string;
  apiKeySet: boolean;
  error: string | null;
}

export type ChatRole = "system" | "user" | "assistant";

export interface ChatMessage {
  role: ChatRole;
  content: string;
}

export interface AiChunk {
  delta: string;
}

export interface AiError {
  message: string;
}
