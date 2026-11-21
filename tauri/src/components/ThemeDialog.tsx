import { useEffect, useMemo, useState } from "react";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import { SHELL_TEXT } from "../i18n/shell";
import Modal from "./Modal";
import { DARK_PALETTE, LIGHT_PALETTE } from "../theme";

interface Props {
  open: boolean;
  onClose: () => void;
}

const GROUPS: { key: string; tokens: string[] }[] = [
  { key: "surface", tokens: ["BG", "SURFACE", "SURFACE_ALT", "SURFACE_DEEP"] },
  { key: "text", tokens: ["TEXT", "TEXT_SEC", "TEXT_MUTED"] },
  { key: "accent", tokens: ["PRIMARY", "PRIMARY_HOVER", "PRIMARY_SOFT", "ACCENT"] },
  { key: "status", tokens: ["DANGER", "WARNING", "SUCCESS"] },
  { key: "border", tokens: ["BORDER", "BORDER_LIGHT", "SCROLLBAR_THUMB", "SCROLLBAR_THUMB_HOVER"] },
  {
    key: "types",
    tokens: [
      "TYPE_DIR", "TYPE_DOC", "TYPE_IMAGE", "TYPE_VIDEO", "TYPE_AUDIO", "TYPE_ARCHIVE",
      "TYPE_CODE", "TYPE_EXEC", "TYPE_DATA", "TYPE_OTHER", "TYPE_HIDDEN",
    ],
  },
];

export default function ThemeDialog({ open, onClose }: Props) {
  const { t, language } = useI18n();
  const { settings, update } = useSettings();
  const shell = SHELL_TEXT[language];
  const [base, setBase] = useState<"light" | "dark">("light");
  const [colors, setColors] = useState<Record<string, string>>({});

  useEffect(() => {
    if (!open || !settings) return;
    const custom = settings.customTheme;
    setBase(custom?.base ?? (settings.theme === "dark" ? "dark" : "light"));
    setColors(custom?.colors ?? {});
  }, [open, settings]);

  const palette = useMemo(
    () => (base === "dark" ? DARK_PALETTE : LIGHT_PALETTE),
    [base],
  );

  const setToken = (token: string, value: string) => {
    setColors((previous) => ({ ...previous, [token]: value }));
  };

  const resetToken = (token: string) => {
    setColors((previous) => {
      const next = { ...previous };
      delete next[token];
      return next;
    });
  };

  const resetAll = () => setColors({});

  const apply = () => {
    update({ theme: "custom", customTheme: { base, colors } });
    onClose();
  };

  if (!open || !settings) return null;

  return (
    <Modal
      title={t("menu.theme.customize")}
      width={720}
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={resetAll}>
            {shell.resetColors}
          </button>
          <div className="toolbar-spacer" />
          <button className="btn" onClick={onClose}>
            {t("button.cancel")}
          </button>
          <button className="btn primary" onClick={apply}>
            {t("button.ok")}
          </button>
        </>
      }
    >
      <div className="theme-dialog">
        <div className="theme-base">
          <span>{t("menu.theme")}</span>
          <button
            className={`btn small ${base === "light" ? "primary" : ""}`}
            onClick={() => setBase("light")}
          >
            {t("theme.light")}
          </button>
          <button
            className={`btn small ${base === "dark" ? "primary" : ""}`}
            onClick={() => setBase("dark")}
          >
            {t("theme.dark")}
          </button>
        </div>
        <div className="theme-preview" style={{ background: colors.BG ?? palette.BG }}>
          {["SURFACE", "PRIMARY", "ACCENT", "SUCCESS", "WARNING", "DANGER", "TEXT"].map((token) => (
            <span
              key={token}
              title={token}
              style={{ background: colors[token] ?? palette[token] }}
            />
          ))}
        </div>
        <div className="theme-groups">
          {GROUPS.map((group) => (
            <div className="theme-group" key={group.key}>
              <div className="theme-group-title">{t(`theme.group.${group.key}`)}</div>
              {group.tokens.map((token) => {
                const value = colors[token] ?? palette[token];
                const changed = token in colors;
                return (
                  <div className="theme-token" key={token}>
                    <span className="theme-token-name" title={token}>
                      {t(`theme.token.${token}`)}
                    </span>
                    <input
                      type="color"
                      value={value}
                      onChange={(event) => setToken(token, event.target.value)}
                    />
                    <span className="theme-token-value mono">{value}</span>
                    <button
                      className="btn tiny"
                      disabled={!changed}
                      onClick={() => resetToken(token)}
                    >
                      {shell.resetColors}
                    </button>
                  </div>
                );
              })}
            </div>
          ))}
        </div>
      </div>
    </Modal>
  );
}
