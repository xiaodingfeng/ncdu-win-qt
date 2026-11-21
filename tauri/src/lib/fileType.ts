import type { NodeEntry } from "../types/scan";

const DOC = [".txt", ".pdf", ".doc", ".docx", ".rtf", ".odt", ".pages", ".md", ".tex", ".epub", ".mobi"];
const IMAGE = [".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tiff", ".tif", ".webp", ".svg", ".heic", ".raw", ".psd", ".ai", ".ico"];
const VIDEO = [".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v", ".mpg", ".mpeg", ".3gp"];
const AUDIO = [".mp3", ".wav", ".flac", ".aac", ".ogg", ".wma", ".m4a", ".alac", ".aiff"];
const ARCHIVE = [".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz", ".iso", ".dmg", ".tgz", ".lz"];
const CODE = [
  ".py", ".js", ".ts", ".jsx", ".tsx", ".java", ".c", ".cpp", ".h", ".hpp", ".cs", ".go", ".rs",
  ".rb", ".php", ".swift", ".kt", ".scala", ".sh", ".bash", ".ps1", ".bat", ".cmd", ".html", ".css",
  ".scss", ".less", ".vue", ".svelte", ".json", ".xml", ".yaml", ".yml", ".toml", ".ini", ".cfg",
];
const EXEC = [".exe", ".msi", ".app", ".appimage", ".deb", ".rpm", ".apk", ".dll", ".so", ".dylib"];
const DATA = [".csv", ".tsv", ".db", ".sqlite", ".sqlite3", ".mdb", ".accdb", ".parquet", ".feather", ".h5", ".hdf5"];

const GROUPS: { key: string; ext: string[] }[] = [
  { key: "document", ext: DOC },
  { key: "image", ext: IMAGE },
  { key: "video", ext: VIDEO },
  { key: "audio", ext: AUDIO },
  { key: "archive", ext: ARCHIVE },
  { key: "code", ext: CODE },
  { key: "executable", ext: EXEC },
  { key: "data", ext: DATA },
];

function extension(name: string): string {
  const index = name.lastIndexOf(".");
  return index < 0 ? "" : name.slice(index).toLowerCase();
}

export function typeKey(entry: NodeEntry): string {
  if (entry.kind === "directory" || entry.kind === "drive") return "folder";
  const ext = extension(entry.name);
  return GROUPS.find((group) => group.ext.includes(ext))?.key ?? "other";
}

/** CSS custom property holding the type colour. */
export function typeVar(entry: NodeEntry): string {
  if (entry.kind === "directory" || entry.kind === "drive") return "--type-dir";
  if (entry.kind === "symlink") return "--type-other";
  if (entry.isHidden) return "--type-hidden";
  const key = typeKey(entry);
  return `--type-${key === "executable" ? "exec" : key}`;
}
