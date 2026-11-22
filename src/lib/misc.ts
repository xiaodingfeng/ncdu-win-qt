import { invoke } from "@tauri-apps/api/core";
import { getVersion } from "@tauri-apps/api/app";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { FeedbackInput, FeedbackResult, UpdateCheck } from "../types/misc";

export function checkUpdate(): Promise<UpdateCheck> {
  return invoke<UpdateCheck>("check_update");
}

export function isPortable(): Promise<boolean> {
  return invoke<boolean>("is_portable");
}

export function submitFeedback(input: FeedbackInput): Promise<FeedbackResult> {
  return invoke<FeedbackResult>("submit_feedback", { input });
}

export function downloadUpdate(version: string): Promise<void> {
  return invoke("download_update", { version });
}

export interface UpdateProgress {
  percent: number;
  received: number;
  total: number;
}

export interface UpdateEventHandlers {
  onProgress: (payload: UpdateProgress) => void;
  onError: (message: string) => void;
}

export async function subscribeUpdate(handlers: Partial<UpdateEventHandlers>): Promise<UnlistenFn> {
  const unlisteners = await Promise.all([
    listen<UpdateProgress>("update://progress", (event) => handlers.onProgress?.(event.payload)),
    listen<{ message: string }>("update://error", (event) => handlers.onError?.(event.payload.message)),
  ]);
  return () => unlisteners.forEach((unlisten) => unlisten());
}

export { getVersion };
