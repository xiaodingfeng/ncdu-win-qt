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
