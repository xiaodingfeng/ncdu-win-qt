import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { AiChunk, AiConfigInfo, AiDefaultReport, AiError, ChatMessage } from "../types/ai";

export function aiGetConfig(): Promise<AiConfigInfo> {
  return invoke<AiConfigInfo>("ai_get_config");
}

export function aiSaveConfig(
  baseUrl: string,
  model: string,
  apiKey: string | null,
  clearKey: boolean,
): Promise<AiConfigInfo> {
  return invoke<AiConfigInfo>("ai_save_config", { baseUrl, model, apiKey, clearKey });
}

export function aiFetchDefaults(): Promise<AiDefaultReport> {
  return invoke<AiDefaultReport>("ai_fetch_defaults");
}

export function aiFetchModels(baseUrl?: string, apiKey?: string): Promise<string[]> {
  return invoke<string[]>("ai_fetch_models", {
    baseUrl: baseUrl ? baseUrl.trim() : null,
    apiKey: apiKey ? apiKey.trim() : null,
  });
}

export function aiChat(messages: ChatMessage[], model?: string | null): Promise<void> {
  return invoke("ai_chat", { messages, model: model ?? null });
}

export function aiCancel(): Promise<void> {
  return invoke("ai_cancel");
}

export interface AiEvents {
  onChunk: (payload: AiChunk) => void;
  onDone: () => void;
  onError: (payload: AiError) => void;
}

export async function subscribeAi(events: Partial<AiEvents>): Promise<UnlistenFn> {
  const unlisteners = await Promise.all([
    listen<AiChunk>("ai://chunk", (event) => events.onChunk?.(event.payload)),
    listen("ai://done", () => events.onDone?.()),
    listen<AiError>("ai://error", (event) => events.onError?.(event.payload)),
  ]);
  return () => unlisteners.forEach((unlisten) => unlisten());
}
