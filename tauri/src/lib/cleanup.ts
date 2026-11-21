import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type {
  CleanFinished,
  CleanItem,
  CleanProgress,
  CleanupFailure,
  CleanupFinished,
  CleanupTarget,
  DuplicateGroup,
  DupFinished,
  DupProgress,
  LargeFile,
} from "../types/cleanup";

export function scanCleanup(
  root: string,
  scanCategories: boolean,
  scanLargeFiles: boolean,
): Promise<void> {
  return invoke("scan_cleanup", { root, scanCategories, scanLargeFiles });
}

export function scanDuplicates(): Promise<void> {
  return invoke("scan_duplicates");
}

export function cancelCleanup(): Promise<void> {
  return invoke("cancel_cleanup");
}

export function cleanItems(items: CleanItem[], permanent: boolean): Promise<void> {
  return invoke("clean_items", { items, permanent });
}

/** Retries failed deletes through the elevated helper. */
export function cleanItemsElevated(items: CleanItem[], permanent: boolean): Promise<void> {
  return invoke("clean_items_elevated", { items, permanent });
}

export interface CleanupEvents {
  onTarget: (target: CleanupTarget) => void;
  onLargeFile: (file: LargeFile) => void;
  onFinished: (payload: CleanupFinished) => void;
  onFailed: (payload: CleanupFailure) => void;
  onDupProgress: (payload: DupProgress) => void;
  onDupGroup: (group: DuplicateGroup) => void;
  onDupFinished: (payload: DupFinished) => void;
  onCleanProgress: (payload: CleanProgress) => void;
  onCleanFinished: (payload: CleanFinished) => void;
}

export async function subscribeCleanup(events: Partial<CleanupEvents>): Promise<UnlistenFn> {
  const unlisteners = await Promise.all([
    listen<CleanupTarget>("cleanup://target", (event) => events.onTarget?.(event.payload)),
    listen<LargeFile>("cleanup://large", (event) => events.onLargeFile?.(event.payload)),
    listen<CleanupFinished>("cleanup://finished", (event) => events.onFinished?.(event.payload)),
    listen<CleanupFailure>("cleanup://failed", (event) => events.onFailed?.(event.payload)),
    listen<DupProgress>("cleanup://dup-progress", (event) => events.onDupProgress?.(event.payload)),
    listen<DuplicateGroup>("cleanup://dup-group", (event) => events.onDupGroup?.(event.payload)),
    listen<DupFinished>("cleanup://dup-finished", (event) => events.onDupFinished?.(event.payload)),
    listen<CleanProgress>("cleanup://clean-progress", (event) =>
      events.onCleanProgress?.(event.payload),
    ),
    listen<CleanFinished>("cleanup://clean-finished", (event) =>
      events.onCleanFinished?.(event.payload),
    ),
  ]);
  return () => unlisteners.forEach((unlisten) => unlisten());
}
