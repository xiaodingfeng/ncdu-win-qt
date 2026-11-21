import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { SaveFolder, SaveMoveProgress, SaveMoveResult, SavePair } from "../types/appsync";

export function saveFolders(): Promise<SaveFolder[]> {
  return invoke<SaveFolder[]>("save_folders");
}

export function setSaveFolder(id: string, newPath: string): Promise<void> {
  return invoke("set_save_folder", { id, newPath });
}

export function moveSaveContents(pairs: SavePair[]): Promise<SaveMoveResult> {
  return invoke<SaveMoveResult>("move_save_contents", { pairs });
}

export function cancelSaveMove(): Promise<void> {
  return invoke("cancel_save_move");
}

export function subscribeAppSync(
  onProgress: (payload: SaveMoveProgress) => void,
): Promise<UnlistenFn> {
  return listen<SaveMoveProgress>("appsync://progress", (event) => onProgress(event.payload));
}
