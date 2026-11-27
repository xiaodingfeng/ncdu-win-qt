import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type {
  DirectoryView,
  DiskInfo,
  NodeEntry,
  ScanFailure,
  ScanFinished,
  ScanProgress,
  TreemapNode,
} from "../types/scan";

export function startScan(root: string, skipHeavy: boolean): Promise<void> {
  return invoke("start_scan", { root, skipHeavy });
}

export function cancelScan(): Promise<void> {
  return invoke("cancel_scan");
}

export function getDirectory(
  path: string,
  offset?: number,
  limit?: number,
): Promise<DirectoryView> {
  return invoke<DirectoryView>("get_directory", { path, offset, limit });
}

export function releaseScan(): Promise<void> {
  return invoke("release_scan");
}

export function subscribeScanProgress(
  onProgress: (payload: ScanProgress) => void,
): Promise<UnlistenFn> {
  return listen<ScanProgress>("scan://progress", (event) => onProgress(event.payload));
}

export function getTreemap(
  path: string,
  depth = 1,
  showFiles = true,
): Promise<TreemapNode> {
  return invoke<TreemapNode>("get_treemap", { path, depth, showFiles });
}

export function searchTree(path: string, query: string, limit = 500): Promise<NodeEntry[]> {
  return invoke<NodeEntry[]>("search_tree", { path, query, limit });
}

export function getScanRoot(): Promise<NodeEntry | null> {
  return invoke<NodeEntry | null>("get_scan_root");
}

export function diskInfo(path: string): Promise<DiskInfo> {
  return invoke<DiskInfo>("disk_info", { path });
}

export function defaultPath(): Promise<string> {
  return invoke<string>("default_path");
}

export interface ScanEvents {
  onProgress: (payload: ScanProgress) => void;
  onFinished: (payload: ScanFinished) => void;
  onFailed: (payload: ScanFailure) => void;
  onLowMemory: () => void;
}

export async function subscribeScan(events: ScanEvents): Promise<UnlistenFn> {
  const unlisteners = await Promise.all([
    listen<ScanProgress>("scan://progress", (event) => events.onProgress(event.payload)),
    listen<ScanFinished>("scan://finished", (event) => events.onFinished(event.payload)),
    listen<ScanFailure>("scan://failed", (event) => events.onFailed(event.payload)),
    listen<ScanFailure>("scan://low-memory", () => events.onLowMemory()),
  ]);
  return () => unlisteners.forEach((unlisten) => unlisten());
}
