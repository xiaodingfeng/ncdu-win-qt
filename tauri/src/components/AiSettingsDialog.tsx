import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import { aiFetchDefaults, aiFetchModels, aiGetConfig, aiSaveConfig } from "../lib/ai";
import type { AiConfigInfo } from "../types/ai";

interface Props {
  open: boolean;
  onClose: () => void;
  onSaved?: (config: AiConfigInfo) => void;
}

export default function AiSettingsDialog({ open, onClose, onSaved }: Props) {
  const { t } = useI18n();
  const [baseUrl, setBaseUrl] = useState("");
  const [apiKey, setApiKey] = useState("");
  const [model, setModel] = useState("auto");
  const [models, setModels] = useState<string[]>([]);
  const [hasKey, setHasKey] = useState(false);
  const [busy, setBusy] = useState<"save" | "models" | "defaults" | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!open) return;
    setApiKey("");
    setMessage(null);
    setError(null);
    void aiGetConfig()
      .then((config) => {
        setBaseUrl(config.baseUrl);
        setModel(config.model || "auto");
        setHasKey(config.hasApiKey);
      })
      .catch((err) => setError(String(err)));
  }, [open]);

  const fetchModels = async () => {
    setBusy("models");
    setError(null);
    setMessage(null);
    try {
      const list = await aiFetchModels(baseUrl.trim(), apiKey.trim());
      setModels(list);
      if (list.length === 0) {
        setError(t("ai.settings.fetch_models_failed", { error: t("ai.settings.no_models") }));
      } else if (model === "auto" || !model) {
        setModel(list[0]);
      }
    } catch (err) {
      setError(
        t("ai.settings.fetch_models_failed", {
          error: baseUrl.trim() && (hasKey || apiKey.trim()) ? String(err) : t("ai.settings.fill_url_key"),
        }),
      );
    } finally {
      setBusy(null);
    }
  };

  const restoreDefaults = async () => {
    setBusy("defaults");
    setError(null);
    setMessage(null);
    try {
      const report = await aiFetchDefaults();
      if (report.baseUrl) setBaseUrl(report.baseUrl);
      const config = await aiGetConfig();
      setHasKey(config.hasApiKey);
      if (report.error && !report.apiKeySet) {
        setError(t("ai.settings.key_fetch_failed"));
      }
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
    }
  };

  const save = async () => {
    setBusy("save");
    setError(null);
    try {
      const config = await aiSaveConfig(
        baseUrl.trim(),
        model.trim() || "auto",
        apiKey.trim() ? apiKey.trim() : null,
        false,
      );
      setHasKey(config.hasApiKey);
      onSaved?.(config);
      setMessage(t("ai.settings.save"));
      onClose();
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
    }
  };

  if (!open) return null;

  return (
    <Modal
      title={t("ai.settings.title")}
      width={620}
      onClose={onClose}
      footer={
        <>
          <button className="btn" disabled={busy !== null} onClick={() => void restoreDefaults()}>
            {busy === "defaults" ? t("ai.settings.fetching") : t("ai.settings.restore_defaults")}
          </button>
          <div className="toolbar-spacer" />
          <button className="btn" onClick={onClose} disabled={busy !== null}>
            {t("button.cancel")}
          </button>
          <button className="btn primary" disabled={busy !== null} onClick={() => void save()}>
            {busy === "save" ? t("ai.settings.fetching") : t("ai.settings.save")}
          </button>
        </>
      }
    >
      <div className="ai-settings">
        <label className="field">
          <span>{t("ai.settings.base_url")}</span>
          <input
            className="input"
            value={baseUrl}
            placeholder="https://api.example.com/v1"
            onChange={(event) => setBaseUrl(event.target.value)}
          />
        </label>
        <label className="field">
          <span>{t("ai.settings.api_key")}</span>
          <input
            className="input"
            type="password"
            value={apiKey}
            placeholder={hasKey ? "••••••••" : ""}
            onChange={(event) => setApiKey(event.target.value)}
          />
        </label>
        <label className="field">
          <span>{t("ai.settings.model")}</span>
          <div className="ai-model-row">
            <input
              className="input"
              value={model}
              placeholder="auto"
              onChange={(event) => setModel(event.target.value)}
            />
            <select
              className="select ai-model-select"
              value={models.includes(model) ? model : ""}
              disabled={busy !== null || models.length === 0}
              onChange={(event) => {
                if (event.target.value) setModel(event.target.value);
              }}
            >
              {models.length === 0 ? (
                <option value="">{t("ai.settings.pick_model")}</option>
              ) : (
                <>
                  <option value="">{t("ai.settings.pick_model")}</option>
                  {models.map((id) => (
                    <option key={id} value={id}>
                      {id}
                    </option>
                  ))}
                </>
              )}
            </select>
            <button className="btn small" disabled={busy !== null} onClick={() => void fetchModels()}>
              {busy === "models" ? t("ai.settings.fetching") : t("ai.settings.fetch_models")}
            </button>
          </div>
        </label>
        <p className="muted small">{t("ai.settings.model_hint")}</p>
        {message && <div className="ai-settings-message">{message}</div>}
        {error && <div className="ai-error">{error}</div>}
      </div>
    </Modal>
  );
}
