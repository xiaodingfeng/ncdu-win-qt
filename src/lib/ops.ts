import { invoke } from "@tauri-apps/api/core";
import type { LockingProcess } from "../types/locker";

export interface DeleteFailure {
  path: string;
  message: string;
  lockedBy?: LockingProcess[];
}

export interface DeleteResult {
  deleted: number;
  deletedPaths: string[];
  failed: DeleteFailure[];
}

export interface FileTimes {
  created: number | null;
  modified: number | null;
  accessed: number | null;
}

export function deletePaths(paths: string[], permanent: boolean): Promise<DeleteResult> {
  return invoke<DeleteResult>("delete_paths", { paths, permanent });
}

export function openPath(path: string): Promise<void> {
  return invoke("open_path", { path });
}

export function revealPath(path: string): Promise<void> {
  return invoke("reveal_path", { path });
}

export function fileTimes(path: string): Promise<FileTimes> {
  return invoke<FileTimes>("file_times", { path });
}
