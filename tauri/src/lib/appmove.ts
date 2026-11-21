import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { AppDir, MoveFinished, MoveProgress } from "../types/appmove";

export function appmoveDefaultRoots(): Promise<string[]> {
  return invoke<string[]>("appmove_default_roots");
}

export function scanAppDirs(customRoots?: string[]): Promise<AppDir[]> {
  return invoke<AppDir[]>("scan_app_dirs", { customRoots: customRoots ?? null });
}

export function moveAppDirs(paths: string[], targetRoot: string): Promise<void> {
  return invoke("move_app_dirs", { paths, targetRoot });
}

export function movePath(paths: string[], targetRoot: string): Promise<MoveFinished> {
  return invoke<MoveFinished>("move_path", { paths, targetRoot });
}

export function restoreAppDir(path: string): Promise<void> {
  return invoke("restore_app_dir", { path });
}

export function cleanAppResidue(paths: string[]): Promise<void> {
  return invoke("clean_app_residue", { paths });
}

export interface AppMoveEvents {
  onProgress: (payload: MoveProgress) => void;
  onFinished: (payload: MoveFinished) => void;
}

export async function subscribeAppMove(events: Partial<AppMoveEvents>): Promise<UnlistenFn> {
  const unlisteners = await Promise.all([
    listen<MoveProgress>("appmove://progress", (event) => events.onProgress?.(event.payload)),
    listen<MoveFinished>("appmove://finished", (event) => events.onFinished?.(event.payload)),
  ]);
  return () => unlisteners.forEach((unlisten) => unlisten());
}
