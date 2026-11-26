import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type {
  IndexProgress,
  IndexSearchArgs,
  IndexSearchResponse,
  IndexStatus,
  VolumeInfo,
} from "../types/index";

export function indexStatus(): Promise<IndexStatus> {
  return invoke<IndexStatus>("index_status");
}

export function indexVolumes(): Promise<VolumeInfo[]> {
  return invoke<VolumeInfo[]>("index_volumes");
}

export function indexBuild(volumes?: string[] | null): Promise<void> {
  return invoke("index_build", { volumes: volumes && volumes.length ? volumes : null });
}

export function indexCancel(): Promise<void> {
  return invoke("index_cancel");
}

export function indexClear(): Promise<void> {
  return invoke("index_clear");
}

export function indexSearch(args: IndexSearchArgs): Promise<IndexSearchResponse> {
  return invoke<IndexSearchResponse>("index_search", {
    query: args.query,
    scope: args.scope ?? null,
    sort: args.sort ?? null,
    asc: args.asc ?? null,
    offset: args.offset ?? null,
    limit: args.limit ?? null,
    caseSensitive: args.caseSensitive ?? null,
  });
}

export function restartAsAdmin(): Promise<void> {
  return invoke("restart_as_admin");
}

export interface IndexEvents {
  onStatus: (status: IndexStatus) => void;
  onProgress: (progress: IndexProgress) => void;
}

export async function subscribeIndex(events: IndexEvents): Promise<UnlistenFn> {
  const unlisteners = await Promise.all([
    listen<IndexStatus>("index://status", (event) => events.onStatus(event.payload)),
    listen<IndexProgress>("index://progress", (event) => events.onProgress(event.payload)),
  ]);
  return () => unlisteners.forEach((unlisten) => unlisten());
}
