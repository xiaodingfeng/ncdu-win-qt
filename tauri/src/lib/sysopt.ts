import { invoke } from "@tauri-apps/api/core";
import type { SysJobResult, UpdateStatus } from "../types/sysopt";

export function getUpdateStatus(): Promise<UpdateStatus> {
  return invoke<UpdateStatus>("get_update_status");
}

export function setUpdateEnabled(enable: boolean): Promise<SysJobResult> {
  return invoke<SysJobResult>("set_update_enabled", { enable });
}

export function runNetworkFix(): Promise<SysJobResult> {
  return invoke<SysJobResult>("run_network_fix");
}

export function flushDns(): Promise<SysJobResult> {
  return invoke<SysJobResult>("flush_dns");
}

export function resetStore(): Promise<SysJobResult> {
  return invoke<SysJobResult>("reset_store");
}
