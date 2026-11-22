import { invoke } from "@tauri-apps/api/core";
import type { CloseOutcome, LockingProcess } from "../types/locker";

export function lockedProcesses(paths: string[], maxFiles?: number): Promise<LockingProcess[]> {
  return invoke<LockingProcess[]>("locked_processes", {
    paths,
    maxFiles: maxFiles ?? null,
  });
}

export function closeLockingProcesses(pids: number[], graceMs?: number): Promise<CloseOutcome[]> {
  return invoke<CloseOutcome[]>("close_locking_processes", {
    pids,
    graceMs: graceMs ?? null,
  });
}
