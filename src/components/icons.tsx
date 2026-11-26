interface IconProps {
  size?: number;
}

const base = (size: number) => ({
  width: size,
  height: size,
  viewBox: "0 0 24 24",
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 1.8,
  strokeLinecap: "round" as const,
  strokeLinejoin: "round" as const,
  "aria-hidden": true,
});

export function IconArrowUp({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M12 19V5" />
      <path d="m5 12 7-7 7 7" />
    </svg>
  );
}

export function IconFolder({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
    </svg>
  );
}

export function IconClose({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)} strokeWidth={2.6}>
      <path d="M6 6l12 12M18 6 6 18" />
    </svg>
  );
}

export function IconFolderOpen({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M3 8a2 2 0 0 1 2-2h4l2 2h6a2 2 0 0 1 2 2v1" />
      <path d="M4.6 19h13.2a1.6 1.6 0 0 0 1.5-1.1l1.6-5.3A1 1 0 0 0 20 11.3H7.4a1.6 1.6 0 0 0-1.5 1.1L4 18a1 1 0 0 0 .6 1z" />
    </svg>
  );
}

export function IconDrive({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <rect x="3" y="5" width="18" height="14" rx="2" />
      <path d="M3 13h18" />
      <circle cx="17.5" cy="16.5" r="1" fill="currentColor" stroke="none" />
    </svg>
  );
}

export function IconSearch({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <circle cx="11" cy="11" r="7" />
      <path d="m20 20-3.6-3.6" />
    </svg>
  );
}

export function IconStar({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="m12 3.6 2.7 5.5 6.1.9-4.4 4.3 1 6.1-5.4-2.9-5.4 2.9 1-6.1L3.2 10l6.1-.9z" />
    </svg>
  );
}

export type FileKind =
  | "folder"
  | "document"
  | "image"
  | "video"
  | "audio"
  | "archive"
  | "code"
  | "executable"
  | "data"
  | "other";

/** Type glyph used by the search result rows, matching the scan list colours. */
export function FileTypeIcon({ kind, size = 16 }: { kind: FileKind; size?: number }) {
  switch (kind) {
    case "folder":
      return (
        <svg {...base(size)}>
          <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
        </svg>
      );
    case "document":
      return (
        <svg {...base(size)}>
          <path d="M6 3h8l4 4v14H6z" />
          <path d="M14 3v4h4" />
          <path d="M9 12h6M9 16h6" />
        </svg>
      );
    case "image":
      return (
        <svg {...base(size)}>
          <rect x="3" y="4" width="18" height="16" rx="2" />
          <circle cx="8.5" cy="9.5" r="1.5" />
          <path d="m4 17 5-5 4 4 3-3 4 4" />
        </svg>
      );
    case "video":
      return (
        <svg {...base(size)}>
          <rect x="3" y="5" width="18" height="14" rx="2" />
          <path d="m10 9 5 3-5 3z" />
        </svg>
      );
    case "audio":
      return (
        <svg {...base(size)}>
          <path d="M9 18V6l9-2v12" />
          <circle cx="6.5" cy="18" r="2.5" />
          <circle cx="15.5" cy="16" r="2.5" />
        </svg>
      );
    case "archive":
      return (
        <svg {...base(size)}>
          <rect x="4" y="3" width="16" height="18" rx="2" />
          <path d="M12 3v4M12 9v2M12 13v2" />
        </svg>
      );
    case "code":
      return (
        <svg {...base(size)}>
          <path d="m8 8-4 4 4 4M16 8l4 4-4 4M13 5l-2 14" />
        </svg>
      );
    case "executable":
      return (
        <svg {...base(size)}>
          <rect x="3" y="4" width="18" height="16" rx="2" />
          <path d="M3 9h18" />
          <path d="M7 6.5h.01M10 6.5h.01" />
        </svg>
      );
    case "data":
      return (
        <svg {...base(size)}>
          <ellipse cx="12" cy="6" rx="7" ry="3" />
          <path d="M5 6v12c0 1.7 3.1 3 7 3s7-1.3 7-3V6" />
          <path d="M5 12c0 1.7 3.1 3 7 3s7-1.3 7-3" />
        </svg>
      );
    default:
      return (
        <svg {...base(size)}>
          <path d="M6 3h8l4 4v14H6z" />
          <path d="M14 3v4h4" />
        </svg>
      );
  }
}

export function IconCaret({ size = 10, up = false }: IconProps & { up?: boolean }) {
  return (
    <svg {...base(size)} style={{ transform: up ? "none" : "rotate(180deg)" }}>
      <path d="m6 15 6-6 6 6" />
    </svg>
  );
}

export function IconUpdate({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M12 3v10" />
      <path d="m8 9 4 4 4-4" />
      <path d="M4 17v2a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2" />
    </svg>
  );
}

export function IconNetwork({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <circle cx="12" cy="12" r="9" />
      <path d="M3 12h18" />
      <path d="M12 3a15 15 0 0 1 0 18a15 15 0 0 1 0-18z" />
    </svg>
  );
}

export function IconBolt({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M13 2 4 14h7l-1 8 9-12h-7z" />
    </svg>
  );
}

export function IconStore({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M4 9h16l-1-4H5z" />
      <path d="M5 9v9a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V9" />
      <path d="M9 13h6" />
    </svg>
  );
}

export function IconSync({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M4 7a8 8 0 0 1 13-2l3 3" />
      <path d="M20 4v4h-4" />
      <path d="M20 17a8 8 0 0 1-13 2l-3-3" />
      <path d="M4 20v-4h4" />
    </svg>
  );
}

export function IconMove({ size = 18 }: IconProps) {
  return (
    <svg {...base(size)}>
      <path d="M5 9V5h4" />
      <path d="M19 15v4h-4" />
      <path d="M5 5l6 6" />
      <path d="M19 19l-6-6" />
    </svg>
  );
}
