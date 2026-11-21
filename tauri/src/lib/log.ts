import { invoke } from "@tauri-apps/api/core";

type Level = "error" | "warn" | "info" | "debug";

/** Forwards UI diagnostics to the same log file the Rust side writes. */
export function logClient(level: Level, message: string) {
  void invoke("log_client", { level, message }).catch(() => undefined);
}

/** Attaches global handlers so uncaught errors are never silent. */
export function installClientErrorLogging() {
  window.addEventListener("error", (event) => {
    logClient(
      "error",
      `window.error: ${event.message} @ ${event.filename}:${event.lineno}:${event.colno}`,
    );
  });
  window.addEventListener("unhandledrejection", (event) => {
    const reason =
      event.reason instanceof Error
        ? `${event.reason.message}\n${event.reason.stack ?? ""}`
        : String(event.reason);
    logClient("error", `unhandledrejection: ${reason}`);
  });
}
