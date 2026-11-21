// Cross-layer contract with src-tauri/src/locker.rs.

export interface LockingProcess {
  pid: number;
  name: string;
  exePath: string;
  safeToClose: boolean;
  blockKey: string;
}

export interface CloseOutcome {
  pid: number;
  name: string;
  result: "exited" | "killed" | "survived" | "refused" | string;
  blockKey: string;
}
