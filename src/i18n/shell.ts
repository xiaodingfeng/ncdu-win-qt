import type { Language } from "../types/settings";

// Strings that only exist in the new shell. Feature pages reuse the shared
// locales so every user-facing feature string keeps its original key.
export const SHELL_TEXT: Record<
  Language,
  { loading: string; loadError: string; retry: string; keepUpdatesOn: string; resetColors: string }
> = {
  zh: {
    loading: "正在加载设置…",
    loadError: "设置加载失败",
    retry: "重试",
    keepUpdatesOn: "保持更新开启",
    resetColors: "恢复默认",
  },
  en: {
    loading: "Loading settings…",
    loadError: "Failed to load settings",
    retry: "Retry",
    keepUpdatesOn: "Keep updates on",
    resetColors: "Reset",
  },
};
