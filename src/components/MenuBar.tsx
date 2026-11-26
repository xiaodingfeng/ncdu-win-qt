import { useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import { useAppConfig } from "../context/AppConfigContext";
import Fade from "./Fade";
import Select from "./ui/Select";
import { openPath } from "../lib/ops";

const THEME_OPTIONS = ["system", "light", "dark", "custom"] as const;
const LANGUAGE_OPTIONS = ["zh", "en"] as const;

interface Item {
  label?: string;
  action?: () => void;
  separator?: boolean;
}

interface Props {
  onThemeCustomize: () => void;
  onAbout: () => void;
  onUpdate: () => void;
  onFeedback: () => void;
  onAiSettings: () => void;
  onAiQa: () => void;
}

function emit(action: string) {
  window.dispatchEvent(new CustomEvent("ncduwin:action", { detail: action }));
}

export default function MenuBar({
  onThemeCustomize,
  onAbout,
  onUpdate,
  onFeedback,
  onAiSettings,
  onAiQa,
}: Props) {
  const { t } = useI18n();
  const { settings, update } = useSettings();
  const config = useAppConfig();
  const [active, setActive] = useState<string | null>(null);
  const [themeOpen, setThemeOpen] = useState(false);

  const menus: { id: string; label: string; items: Item[] }[] = [
    {
      id: "file",
      label: t("menu.file"),
      items: [
        { label: t("menu.file.open_folder"), action: () => emit("open") },
        { label: t("menu.file.rescan"), action: () => emit("rescan") },
        { label: t("menu.file.global_search"), action: () => emit("global-search") },
        { separator: true },
        { label: t("app_sync.menu"), action: () => emit("app-sync") },
        { separator: true },
        {
          label: t("menu.file.quit"),
          action: () => {
            void getCurrentWindow().close();
          },
        },
      ],
    },
    {
      id: "view",
      label: t("menu.view"),
      items: [
        { label: t("menu.view.refresh"), action: () => emit("refresh") },
        { label: t("menu.view.up"), action: () => emit("up") },
        { label: t("menu.view.show_files"), action: () => emit("toggle-files") },
        { separator: true },
        { label: t("menu.theme.customize"), action: onThemeCustomize },
      ],
    },
    {
      id: "ai",
      label: t("menu.ai"),
      items: [
        { label: t("menu.ai.analyze_current"), action: () => emit("ai-analyze") },
        { label: t("menu.ai.qa"), action: onAiQa },
        { separator: true },
        { label: t("menu.ai.settings"), action: onAiSettings },
      ],
    },
    {
      id: "help",
      label: t("menu.help"),
      items: [
        { label: t("menu.help.about", { name: config.name }), action: onAbout },
        { label: t("menu.help.documentation"), action: () => void openPath(config.homepage) },
        { label: t("menu.help.check_update"), action: onUpdate },
        { label: t("menu.help.publish_log"), action: () => void openPath(`${config.homepage}#publishlog`) },
        { separator: true },
        { label: t("menu.help.feedback"), action: onFeedback },
      ],
    },
  ];

  return (
    <nav className="menubar">
      {menus.map((menu) => (
        <div className="menubar-item" key={menu.id}>
          <button
            className={`menubar-button ${active === menu.id ? "active" : ""}`}
            onClick={() => setActive(active === menu.id ? null : menu.id)}
            onMouseEnter={() => active && setActive(menu.id)}
          >
            {menu.label}
          </button>
          <Fade show={active === menu.id}>
            {active === menu.id && (
              <>
                <div className="menubar-overlay" onMouseDown={() => setActive(null)} />
                <div className="menu-dropdown">
                  {menu.items.map((item, index) =>
                    item.separator ? (
                      <div className="menu-separator" key={`sep-${index}`} />
                    ) : (
                      <button
                        key={item.label}
                        className="menu-entry"
                        onClick={() => {
                          setActive(null);
                          item.action?.();
                        }}
                      >
                        {item.label}
                      </button>
                    ),
                  )}
                </div>
              </>
            )}
          </Fade>
        </div>
      ))}
      {settings && (
        <div className="menubar-controls">
          <div className="menubar-item">
            <button
              className={`menubar-select ${themeOpen ? "active" : ""}`}
              aria-label={t("menu.theme")}
              title={t("menu.theme")}
              onClick={() => setThemeOpen((value) => !value)}
            >
              {t(`theme.${settings.theme}`)}
              <span className="menubar-caret">▾</span>
            </button>
            <Fade show={themeOpen}>
              {themeOpen && (
                <>
                  <div className="menubar-overlay" onMouseDown={() => setThemeOpen(false)} />
                  <div className="menu-dropdown theme-dropdown">
                    {THEME_OPTIONS.map((mode) => (
                      <button
                        key={mode}
                        className={`menu-entry ${settings.theme === mode ? "active" : ""}`}
                        onClick={() => {
                          setThemeOpen(false);
                          update({ theme: mode });
                          if (mode === "custom") onThemeCustomize();
                        }}
                      >
                        {t(`theme.${mode}`)}
                      </button>
                    ))}
                  </div>
                </>
              )}
            </Fade>
          </div>
          <Select
            value={settings.language}
            ariaLabel={t("menu.language")}
            options={LANGUAGE_OPTIONS.map((code) => ({
              value: code,
              label: code === "zh" ? "简体中文" : "English",
            }))}
            onChange={(value) => update({ language: value as (typeof LANGUAGE_OPTIONS)[number] })}
          />
        </div>
      )}
    </nav>
  );
}
