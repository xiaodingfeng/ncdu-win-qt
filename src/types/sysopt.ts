// Cross-layer contract with src-tauri/src/sysopt.rs.

export interface UpdateStatus {
  blocked: boolean;
  reasons: string[];
  serviceDisabled: boolean;
  policyBlocked: boolean;
  hostsBlocked: boolean;
}

export interface SysJobResult {
  ok: boolean;
  needsReboot: boolean;
  message: string;
  total: number;
}
