const SIZE_UNITS = ["KiB", "MiB", "GiB", "TiB", "PiB"];

/** Mirrors humanSize() in src/core/FormatHelpers.cpp. */
export function formatSize(bytes: number): string {
  if (bytes < 0) return `-${formatSize(-bytes)}`;
  if (bytes < 1024) return `${bytes} B`;
  let value = bytes;
  for (let i = 0; i < SIZE_UNITS.length; i += 1) {
    value /= 1024;
    if (value < 1024) return `${value.toFixed(1)} ${SIZE_UNITS[i]}`;
  }
  return `${value.toFixed(1)} EiB`;
}

/** Mirrors humanCount() in src/core/FormatHelpers.cpp. */
export function formatCount(value: number): string {
  if (value < 1000) return String(value);
  if (value < 1_000_000) return `${(value / 1000).toFixed(1)}k`;
  return `${(value / 1_000_000).toFixed(1)}M`;
}

export function formatDuration(ms: number, t: (key: string, params?: Record<string, string | number>) => string): string {
  if (ms < 0) ms = 0;
  if (ms < 1000) return t("time.ms", { ms: Math.round(ms) });
  if (ms < 60000) {
    const tenths = Math.round(ms / 100);
    if (tenths >= 600) return t("time.min_sec", { min: 1, sec: "00" });
    return t("time.sec", { sec: (tenths / 10).toFixed(1) });
  }
  if (ms < 3600000) {
    const min = Math.floor(ms / 60000);
    const sec = String(Math.floor((ms % 60000) / 1000)).padStart(2, "0");
    return t("time.min_sec", { min, sec });
  }
  const hour = Math.floor(ms / 3600000);
  const min = String(Math.floor((ms % 3600000) / 60000)).padStart(2, "0");
  return t("time.hour_min", { hour, min });
}

/** Splits "C:\Users\name\docs" into clickable breadcrumb pieces. */
export function pathSegments(path: string): { label: string; path: string }[] {
  const normalized = path.replace(/\//g, "\\");
  const driveMatch = normalized.match(/^([A-Za-z]:)(\\)?/);
  const segments: { label: string; path: string }[] = [];
  let rest = normalized;

  if (driveMatch) {
    segments.push({ label: driveMatch[1], path: `${driveMatch[1]}\\` });
    rest = normalized.slice(driveMatch[0].length);
  } else if (normalized.startsWith("\\\\")) {
    segments.push({ label: "\\\\", path: normalized });
  }

  let current = segments.length ? segments[0].path : "";
  for (const part of rest.split("\\").filter(Boolean)) {
    current = current.endsWith("\\") ? `${current}${part}` : `${current}\\${part}`;
    segments.push({ label: part, path: current });
  }
  return segments;
}
