import { useEffect, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { useI18n } from "./context/I18nContext";
import { useSettings } from "./context/SettingsContext";
import { useAppConfig } from "./context/AppConfigContext";
import ScanPage from "./components/ScanPage";
import MenuBar from "./components/MenuBar";
import AboutDialog from "./components/AboutDialog";
import UpdateDialog from "./components/UpdateDialog";
import UpdateToast from "./components/UpdateToast";
import FeedbackDialog from "./components/FeedbackDialog";
import AiSettingsDialog from "./components/AiSettingsDialog";
import AiDialog from "./components/AiDialog";
import ThemeDialog from "./components/ThemeDialog";
import { applyTheme, watchSystemTheme } from "./theme";
import { SHELL_TEXT } from "./i18n/shell";

type DialogId = "theme" | "about" | "update" | "feedback" | "aiSettings" | "aiQa" | null;

export default function App() {
  const { settings, error, reload } = useSettings();
  const { t, language } = useI18n();
  const config = useAppConfig();
  const shell = SHELL_TEXT[language];
  const [dialog, setDialog] = useState<DialogId>(null);

  useEffect(() => {
    // Keep the native window title in sync with the selected language.
    void getCurrentWindow()
      .setTitle(t("app.title", { name: config.name }))
      .catch(() => undefined);
  }, [t, config.name]);

  useEffect(() => {
    if (!settings) return;
    const apply = () => applyTheme(settings.theme, settings.customTheme);
    apply();
    return settings.theme === "system" ? watchSystemTheme(apply) : undefined;
  }, [settings]);

  useEffect(() => {
    const openAiSettings = () => setDialog("aiSettings");
    window.addEventListener("ncduwin:open-ai-settings", openAiSettings);
    return () => window.removeEventListener("ncduwin:open-ai-settings", openAiSettings);
  }, []);

  useEffect(() => {
    // The WebView's default context menu has nothing useful; every actionable
    // menu is rendered by the component that owns the row or cell.
    const suppress = (event: Event) => event.preventDefault();
    document.addEventListener("contextmenu", suppress, true);
    return () => document.removeEventListener("contextmenu", suppress, true);
  }, []);

  if (!settings) {
    return (
      <div className="loading-screen">
        <span>{error ? `${shell.loadError}: ${error}` : shell.loading}</span>
        {error && (
          <button className="btn" onClick={() => void reload()}>
            {shell.retry}
          </button>
        )}
      </div>
    );
  }

  return (
    <div className="app">
      <MenuBar
        onThemeCustomize={() => setDialog("theme")}
        onAbout={() => setDialog("about")}
        onUpdate={() => setDialog("update")}
        onFeedback={() => setDialog("feedback")}
        onAiSettings={() => setDialog("aiSettings")}
        onAiQa={() => setDialog("aiQa")}
      />
      <ScanPage />
      <UpdateToast />

      <ThemeDialog open={dialog === "theme"} onClose={() => setDialog(null)} />
      <AboutDialog open={dialog === "about"} onClose={() => setDialog(null)} />
      <UpdateDialog open={dialog === "update"} onClose={() => setDialog(null)} />
      <FeedbackDialog open={dialog === "feedback"} onClose={() => setDialog(null)} />
      <AiSettingsDialog open={dialog === "aiSettings"} onClose={() => setDialog(null)} />
      <AiDialog
        open={dialog === "aiQa"}
        systemPrompt={t("ai.qa.system")}
        onClose={() => setDialog(null)}
        onConfigure={() => setDialog("aiSettings")}
      />

      {error && <div className="global-error">{error}</div>}
    </div>
  );
}
