export type IndexEngineStatus =
  | "idle"
  | "loading"
  | "building"
  | "syncing"
  | "stored"
  | "ready"
  | "error"
  | "disabled";

export interface VolumeStatus {
  volume: string;
  status: IndexEngineStatus;
  count: number;
  builtAt: number;
  elapsedMs: number;
  message: string;
  memoryBytes: number;
  canBuild: boolean;
  snapshot: boolean;
  engine: string;
  truncated: number;
  loaded: boolean;
}

export interface IndexStatus {
  status: IndexEngineStatus;
  volume: string;
  count: number;
  builtAt: number;
  elapsedMs: number;
  message: string;
  memoryBytes: number;
  canBuild: boolean;
  realtime: boolean;
  generation: number;
  snapshot: boolean;
  engine: string;
  writable: boolean;
  truncated: number;
  volumes: VolumeStatus[];
  unindexedVolumes: string[];
}

export interface IndexDiagnosis {
  volume: string;
  volumeIndexed: boolean;
  volumeStatus: string;
  engine: string;
  entries: number;
  truncated: number;
  nameFound: boolean;
  indexedPath: string | null;
  matched: number;
  hitExclude: string | null;
  reparseTarget: string | null;
  hint: string;
}

/** A local drive shown in the volume picker. */
export interface VolumeInfo {
  letter: string;
  label: string;
  ntfs: boolean;
  fixed: boolean;
  removable: boolean;
  totalBytes: number;
  freeBytes: number;
  canBuild: boolean;
}

export interface IndexHit {
  path: string;
  name: string;
  size: number;
  mtime: number;
  isDir: boolean;
  hidden: boolean;
}

export interface IndexSearchResponse {
  items: IndexHit[];
  total: number;
  tookMs: number;
  version: number;
  hasMore: boolean;
  error: string | null;
}

export type IndexSort = "name" | "path" | "size" | "mtime";

export interface IndexSearchArgs {
  query: string;
  scope?: string | null;
  sort?: IndexSort;
  asc?: boolean;
  offset?: number;
  limit?: number;
  caseSensitive?: boolean;
}

export interface IndexProgress {
  volume: string;
  phase: string;
  count: number;
}

export interface IndexPrefs {
  enabled: boolean;
  /** Selected volume letters. Empty means all local volumes. */
  volumes: string[];
  excludes: string[];
  memoryBudgetMb: number;
  maxEntries: number;
  realtime: boolean;
}
