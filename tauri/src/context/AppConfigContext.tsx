import { createContext, useContext, useEffect, useState, type ReactNode } from "react";
import { invoke } from "@tauri-apps/api/core";

export interface AppConfig {
  name: string;
  homepage: string;
  sourceUrl: string;
  downloadPage: string;
  version: string;
  versionInfo: string;
  copyright: string;
  ai?: {
    baseUrl: string;
    model: string;
    baseUrlEndpoint: string;
    apiKeyEndpoint: string;
  };
}

export const FALLBACK_CONFIG: AppConfig = {
  name: "NcduWin",
  homepage: "https://ncduwin.fengzhengx.cn/",
  sourceUrl: "https://github.com/xiaodingfeng/ncdu-win-qt",
  downloadPage: "https://ncduwin.fengzhengx.cn/",
  version: "",
  versionInfo: "",
  copyright: "",
};

const AppConfigContext = createContext<AppConfig>(FALLBACK_CONFIG);

export function AppConfigProvider({ children }: { children: ReactNode }) {
  const [config, setConfig] = useState<AppConfig>(FALLBACK_CONFIG);

  useEffect(() => {
    void invoke<AppConfig>("app_config")
      .then((value) => setConfig({ ...FALLBACK_CONFIG, ...value }))
      .catch(() => undefined);
  }, []);

  return <AppConfigContext.Provider value={config}>{children}</AppConfigContext.Provider>;
}

export function useAppConfig(): AppConfig {
  return useContext(AppConfigContext);
}
