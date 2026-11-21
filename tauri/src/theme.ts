import type { CustomTheme, ThemeMode } from "./types/settings";

// Palette values mirrored from src/ui/Style.h (LIGHT_THEME / DARK_THEME).
export const LIGHT_PALETTE: Record<string, string> = {
  BG: "#F5F8FB",
  SURFACE: "#FFFFFF",
  SURFACE_ALT: "#EEF3F8",
  SURFACE_DEEP: "#E4EBF3",
  TEXT: "#1E2A3A",
  TEXT_SEC: "#5B6B7E",
  TEXT_MUTED: "#94A3B8",
  PRIMARY: "#14B8A6",
  PRIMARY_HOVER: "#0D9488",
  PRIMARY_SOFT: "#E3F0FF",
  ACCENT: "#60A5FA",
  DANGER: "#EF4444",
  WARNING: "#F59E0B",
  SUCCESS: "#10B981",
  BORDER: "#E2E8F0",
  BORDER_LIGHT: "#EEF2F6",
  SCROLLBAR_THUMB: "#CBD5E1",
  SCROLLBAR_THUMB_HOVER: "#94A3B8",
  TYPE_DIR: "#2DD4BF",
  TYPE_DOC: "#60A5FA",
  TYPE_IMAGE: "#A78BFA",
  TYPE_VIDEO: "#F472B6",
  TYPE_AUDIO: "#34D399",
  TYPE_ARCHIVE: "#FBBF24",
  TYPE_CODE: "#22D3EE",
  TYPE_EXEC: "#FB923C",
  TYPE_DATA: "#818CF8",
  TYPE_OTHER: "#94A3B8",
  TYPE_HIDDEN: "#CBD5E1",
};

export const DARK_PALETTE: Record<string, string> = {
  BG: "#1E2330",
  SURFACE: "#252B3A",
  SURFACE_ALT: "#2D3445",
  SURFACE_DEEP: "#353D50",
  TEXT: "#F0F4F8",
  TEXT_SEC: "#A6B4C9",
  TEXT_MUTED: "#8595A8",
  PRIMARY: "#14B8A6",
  PRIMARY_HOVER: "#0D9488",
  PRIMARY_SOFT: "#1F3A3A",
  ACCENT: "#60A5FA",
  DANGER: "#EF4444",
  WARNING: "#F59E0B",
  SUCCESS: "#10B981",
  BORDER: "#353D50",
  BORDER_LIGHT: "#2D3445",
  SCROLLBAR_THUMB: "#4B5563",
  SCROLLBAR_THUMB_HOVER: "#718096",
  TYPE_DIR: "#2DD4BF",
  TYPE_DOC: "#60A5FA",
  TYPE_IMAGE: "#A78BFA",
  TYPE_VIDEO: "#F472B6",
  TYPE_AUDIO: "#34D399",
  TYPE_ARCHIVE: "#FBBF24",
  TYPE_CODE: "#22D3EE",
  TYPE_EXEC: "#FB923C",
  TYPE_DATA: "#818CF8",
  TYPE_OTHER: "#94A3B8",
  TYPE_HIDDEN: "#4A5366",
};

function varName(token: string): string {
  return `--${token.toLowerCase().replace(/_/g, "-")}`;
}

function resolveBase(mode: ThemeMode, custom: CustomTheme | null): "light" | "dark" {
  if (mode === "custom" && custom) return custom.base;
  if (mode === "light" || mode === "dark") return mode;
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

export function applyTheme(mode: ThemeMode, custom: CustomTheme | null): void {
  const base = resolveBase(mode, custom);
  const palette = base === "dark" ? DARK_PALETTE : LIGHT_PALETTE;
  const overrides = mode === "custom" && custom ? custom.colors : {};
  const root = document.documentElement;

  root.dataset.theme = base;
  root.style.colorScheme = base;
  for (const [token, value] of Object.entries(palette)) {
    root.style.setProperty(varName(token), overrides[token] ?? value);
  }
  for (const [token, value] of Object.entries(overrides)) {
    root.style.setProperty(varName(token), value);
  }
}

/** Subscribes to OS light/dark changes while `system` is active. */
export function watchSystemTheme(onChange: () => void): () => void {
  const media = window.matchMedia("(prefers-color-scheme: dark)");
  media.addEventListener("change", onChange);
  return () => media.removeEventListener("change", onChange);
}
