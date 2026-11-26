// Cross-layer contract with src-tauri/src/settings.rs.
// Rust serializes with #[serde(rename_all = "camelCase")].

export type Language = "zh" | "en";
export type ThemeMode = "light" | "dark" | "system" | "custom";

export interface CustomTheme {
  base: "light" | "dark";
  /** Token name (BG, TYPE_DIR, ...) to #RRGGBB. */
  colors: Record<string, string>;
}

export interface ScanPrefs {
  skipHeavyDirs: boolean;
  showFiles: boolean;
  cleanupCategories: boolean;
  cleanupLarge: boolean;
  cleanupDuplicates: boolean;
}

export interface MoveScanPrefs {
  customDirs: string[];
  scopeIndex: number;
  lastTargetRoot: string;
}

export interface AiSettings {
  baseUrl: string;
  model: string;
  hasApiKey: boolean;
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

export interface AppSettings {
  language: Language;
  theme: ThemeMode;
  customTheme: CustomTheme | null;
  scan: ScanPrefs;
  defaultMovePath: string;
  moveScan: MoveScanPrefs;
  autoUpdateGuard: boolean;
  ai: AiSettings;
  index: IndexPrefs;
}

export interface ImportReport {
  imported: boolean;
  migratedKeys: string[];
  warnings: string[];
}

export const DEFAULT_SETTINGS: AppSettings = {
  language: "zh",
  theme: "system",
  customTheme: null,
  scan: {
    skipHeavyDirs: true,
    showFiles: true,
    cleanupCategories: true,
    cleanupLarge: true,
    cleanupDuplicates: true,
  },
  defaultMovePath: "",
  moveScan: { customDirs: [], scopeIndex: 0, lastTargetRoot: "" },
  autoUpdateGuard: false,
  ai: { baseUrl: "", model: "", hasApiKey: false },
  index: {
    enabled: true,
    volumes: [],
    excludes: ["$Recycle.Bin", "System Volume Information"],
    memoryBudgetMb: 256,
    maxEntries: 3_000_000,
    realtime: true,
  },
};
