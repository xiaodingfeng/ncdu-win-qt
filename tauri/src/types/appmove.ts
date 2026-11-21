// Cross-layer contract with src-tauri/src/appmove.rs.

export interface AppDir {
  path: string;
  name: string;
  size: number;
  linked: boolean;
  linkTarget: string;
  moved: boolean;
  installed: boolean;
  residue: boolean;
  backup: string;
  state: "normal" | "moved" | "residue" | string;
  error: string | null;
}

export interface MoveProgress {
  index: number;
  total: number;
  path: string;
  phase: string;
  percent: number;
}

export interface MoveItemResult {
  path: string;
  ok: boolean;
  message: string;
}

export interface MoveFinished {
  moved: number;
  failed: number;
  items: MoveItemResult[];
}
