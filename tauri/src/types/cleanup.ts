// Cross-layer contract with src-tauri/src/cleanup.rs.
// Rust serializes with #[serde(rename_all = "camelCase")]; DangerLevel is an
// upper-case letter.

export type DangerLevel = "S" | "A" | "B" | "C" | "D";

export interface CleanupTarget {
  key: string;
  path: string;
  size: number;
  fileCount: number;
  danger: DangerLevel;
  isDir: boolean;
  remark: string;
  checked: boolean;
  enabled: boolean;
}

export interface LargeFile {
  path: string;
  name: string;
  size: number;
  danger: DangerLevel;
  remark: string;
}

export interface DuplicateFile {
  path: string;
  name: string;
  size: number;
  danger: DangerLevel;
}

export interface DuplicateGroup {
  size: number;
  wasted: number;
  files: DuplicateFile[];
}

export interface CleanupFinished {
  targets: CleanupTarget[];
  largeFiles: LargeFile[];
  totalSize: number;
  totalCount: number;
  largeTotal: number;
  free: number;
  total: number;
}

export interface CleanupFailure {
  code: string;
  message: string;
}

export interface DupProgress {
  phase: number;
  processed: number;
  total: number;
}

export interface DupFinished {
  groups: DuplicateGroup[];
  totalWasted: number;
  totalFiles: number;
}

export interface CleanItem {
  type: "target" | "file";
  key: string;
  path: string;
}

export interface CleanItemOutcome {
  key: string;
  path: string;
  deleted: number;
  skipped: number;
  freed: number;
  failed: boolean;
  message: string;
}

export interface CleanProgress {
  index: number;
  total: number;
  label: string;
}

export interface CleanFinished {
  totalDeleted: number;
  totalSkipped: number;
  totalFreed: number;
  items: CleanItemOutcome[];
  failed: string[];
  needsElevation: boolean;
}
