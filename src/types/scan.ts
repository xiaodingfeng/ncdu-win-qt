export type NodeKind = "file" | "directory" | "drive" | "symlink" | "other";

export interface NodeEntry {
  path: string;
  name: string;
  kind: NodeKind;
  size: number;
  fileCount: number;
  dirCount: number;
  isHidden: boolean;
  skipped: boolean;
  error: string | null;
}

export interface ScanProgress {
  path: string;
  files: number;
  dirs: number;
  bytes: number;
  engine: "mft" | "walk" | string;
  phase: string;
  count: number | null;
  elapsedMs?: number | null;
  filesPerSec?: number | null;
}

export interface ScanSummary {
  path: string;
  size: number;
  files: number;
  folders: number;
  elapsedMs: number;
  engine: string;
}

export interface ScanFinished {
  root: NodeEntry;
  elapsedMs: number;
  engine: "mft" | "walk" | string;
}

export interface ScanFailure {
  code: string;
  message: string;
}

export interface DirectoryView {
  node: NodeEntry;
  children: NodeEntry[];
  total: number;
}

export interface TreemapNode {
  path: string;
  name: string;
  kind: NodeKind;
  size: number;
  children: TreemapNode[];
}

export interface DiskInfo {
  free: number;
  total: number;
}

export function isDir(entry: NodeEntry): boolean {
  return entry.kind === "directory" || entry.kind === "drive";
}
