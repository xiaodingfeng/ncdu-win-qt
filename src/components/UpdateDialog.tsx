import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import { useAppConfig } from "../context/AppConfigContext";
import Modal from "./Modal";
import { checkUpdate, downloadUpdate, isPortable, subscribeUpdate } from "../lib/misc";
import type { UpdateCheck } from "../types/misc";
import { openPath } from "../lib/ops";

interface Props {
  open: boolean;
  onClose: () => void;
}

export default function UpdateDialog({ open, onClose }: Props) {
  const { t } = useI18n();
  const config = useAppConfig();
  const [checking, setChecking] = useState(false);
  const [result, setResult] = useState<UpdateCheck | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [percent, setPercent] = useState<number | null>(null);
  const [installing, setInstalling] = useState(false);
  const [portable, setPortable] = useState(false);

  useEffect(() => {
    if (!open) return;
    setResult(null);
    setError(null);
    setPercent(null);
    setInstalling(false);
    void isPortable()
      .then(setPortable)
      .catch(() => setPortable(false));
    setChecking(true);
    void checkUpdate()
      .then((value) => {
        setResult(value);
        if (value.error && !value.hasUpdate) setError(value.error);
      })
      .catch((err) => setError(String(err)))
      .finally(() => setChecking(false));
  }, [open]);

  useEffect(() => {
    if (!open) return;
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeUpdate({
      onProgress: (payload) => {
        if (!disposed) setPercent(payload.percent);
      },
      onError: (message) => {
        if (disposed) return;
        setError(t("update.download_failed", { error: message }));
        setPercent(null);
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [open, t]);

  const startDownload = async () => {
    if (!result) return;
    setError(null);
    setPercent(0);
    try {
      await downloadUpdate(result.remote);
      setInstalling(true);
    } catch (err) {
      setPercent(null);
      setError(t("update.download_failed", { error: String(err) }));
    }
  };

  if (!open) return null;

  const title =
    result?.hasUpdate && !checking ? t("update.new_title") : t("update.checking");

  return (
    <Modal
      title={title}
      width={560}
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose} disabled={percent !== null && !installing}>
            {t("button.close")}
          </button>
          <div className="toolbar-spacer" />
          {result?.hasUpdate && percent === null && !installing && !portable && (
            <button className="btn primary" onClick={() => void startDownload()}>
              {t("update.download_install")}
            </button>
          )}
        </>
      }
    >
      <div className="update-dialog">
        {checking && <p className="dialog-text">{t("update.checking")}</p>}
        {!checking && error && (
          <>
            <p className="dialog-text">{t("update.check_failed")}</p>
            <p className="dialog-text">{t("update.check_failed_body", { error })}</p>
          </>
        )}
        {!checking && !error && result && !result.hasUpdate && (
          <p className="dialog-text">{t("update.current_body", { version: result.local, name: config.name })}</p>
        )}
        {!checking && result?.hasUpdate && (
          <>
            <p className="dialog-text">{t("update.new_body", { version: result.remote, local: result.local, name: config.name })}</p>
            {portable && <p className="dialog-text muted">{t("update.portable_hint")}</p>}
            <button className="about-link" onClick={() => void openPath(config.downloadPage)}>
              {t("update.download_url", { url: config.downloadPage })}
            </button>
          </>
        )}
        {percent !== null && (
          <div className="update-progress">
            <span>{installing ? t("update.installing") : t("update.downloading", { percent })}</span>
            <div className="progress-track">
              <span style={{ width: `${percent}%` }} />
            </div>
          </div>
        )}
      </div>
    </Modal>
  );
}
