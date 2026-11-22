// Cross-layer contract with src-tauri/src/misc.rs.

export interface UpdateCheck {
  hasUpdate: boolean;
  remote: string;
  local: string;
  error: string | null;
}

export interface FeedbackInput {
  title: string;
  content: string;
  contact: string;
  level: "debug" | "info" | "warn" | "error" | "fatal";
}

export interface FeedbackResult {
  ok: boolean;
  error: string | null;
}
