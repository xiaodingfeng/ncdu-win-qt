import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import { submitFeedback } from "../lib/misc";
import type { FeedbackInput } from "../types/misc";

interface Props {
  open: boolean;
  onClose: () => void;
}

const LEVELS: FeedbackInput["level"][] = ["debug", "info", "warn", "error", "fatal"];

export default function FeedbackDialog({ open, onClose }: Props) {
  const { t } = useI18n();
  const [title, setTitle] = useState("");
  const [content, setContent] = useState("");
  const [contact, setContact] = useState("");
  const [level, setLevel] = useState<FeedbackInput["level"]>("info");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState(false);

  useEffect(() => {
    if (!open) return;
    setTitle("");
    setContent("");
    setContact("");
    setLevel("info");
    setError(null);
    setDone(false);
    setBusy(false);
  }, [open]);

  const submit = async () => {
    if (!content.trim()) {
      setError(t("feedback.empty_content"));
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const result = await submitFeedback({ title, content, contact, level });
      if (result.ok) {
        setDone(true);
      } else {
        setError(t("feedback.failed", { error: result.error ?? "" }));
      }
    } catch (err) {
      setError(t("feedback.failed", { error: String(err) }));
    } finally {
      setBusy(false);
    }
  };

  if (!open) return null;

  return (
    <Modal
      title={t("feedback.title")}
      width={560}
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose} disabled={busy}>
            {t("button.cancel")}
          </button>
          <div className="toolbar-spacer" />
          <button className="btn primary" onClick={() => void submit()} disabled={busy || done}>
            {busy ? t("feedback.sending") : t("feedback.submit")}
          </button>
        </>
      }
    >
      {done ? (
        <p className="dialog-text">{t("feedback.success_body")}</p>
      ) : (
        <div className="feedback-form">
          <label className="field">
            <span>{t("feedback.field.title")}</span>
            <input
              className="input"
              value={title}
              placeholder={t("feedback.placeholder.title")}
              onChange={(event) => setTitle(event.target.value)}
            />
          </label>
          <label className="field">
            <span>{t("feedback.field.content")}</span>
            <textarea
              className="input textarea"
              value={content}
              placeholder={t("feedback.placeholder.content")}
              onChange={(event) => setContent(event.target.value)}
            />
          </label>
          <label className="field">
            <span>{t("feedback.field.contact")}</span>
            <input
              className="input"
              value={contact}
              placeholder={t("feedback.placeholder.contact")}
              onChange={(event) => setContact(event.target.value)}
            />
          </label>
          <label className="field">
            <span>{t("feedback.field.level")}</span>
            <select
              className="select"
              value={level}
              onChange={(event) => setLevel(event.target.value as FeedbackInput["level"])}
            >
              {LEVELS.map((item) => (
                <option key={item} value={item}>
                  {t(`feedback.level.${item}`)}
                </option>
              ))}
            </select>
          </label>
          <p className="muted small">{t("feedback.hint")}</p>
          {error && <div className="ai-error">{error}</div>}
        </div>
      )}
    </Modal>
  );
}
