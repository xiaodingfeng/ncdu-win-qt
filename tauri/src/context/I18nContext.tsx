import { createContext, useCallback, useContext, useMemo, type ReactNode } from "react";
import en from "../i18n/locales/en.json";
import zh from "../i18n/locales/zh.json";
import type { Language } from "../types/settings";
import { useSettings } from "./SettingsContext";

type Table = Record<string, string>;

const TABLES: Record<Language, Table> = { zh, en };

export type TranslateParams = Record<string, string | number>;

interface I18nContextValue {
  language: Language;
  t: (key: string, params?: TranslateParams) => string;
}

const I18nContext = createContext<I18nContextValue | null>(null);

/** Same `{name}` interpolation semantics as the Qt implementation. */
function format(template: string, params?: TranslateParams): string {
  if (!params) return template;
  return template.replace(/\{(\w+)\}/g, (match, name: string) => {
    const value = params[name];
    return value === undefined ? match : String(value);
  });
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const { settings } = useSettings();
  const language = settings?.language ?? "zh";

  const t = useCallback(
    (key: string, params?: TranslateParams) => {
      const template = TABLES[language][key] ?? TABLES.en[key] ?? key;
      return format(template, params);
    },
    [language],
  );

  const value = useMemo(() => ({ language, t }), [language, t]);
  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>;
}

export function useI18n(): I18nContextValue {
  const ctx = useContext(I18nContext);
  if (!ctx) throw new Error("useI18n must be used inside I18nProvider");
  return ctx;
}
