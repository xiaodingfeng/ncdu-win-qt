import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import { useAppConfig } from "../context/AppConfigContext";
import { checkUpdate, downloadUpdate, isPortable, subscribeUpdate } from "../lib/misc";
import { logClient } from "../lib/log";
import { openPath } from "../lib/ops";
import type { UpdateCheck } from "../types/misc";

const STARTUP_DELAY = 2000;

/** Checks for updates shortly after startup and offers the download in place. */
export default function UpdateToast() {
  const { t } = useI18n();
  const config = useAppConfig();
  const [result, setResult] = useState<UpdateCheck | null>(null);
  const [visible, setVisible] = useState(false);
  const [portable, setPortable] = useState(false);
  const [active, setActive] = useState(false);
  const [percent, setPercent] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let disposed = false;
    // Let the first screen render before touching the network.
    const timer = window.setTimeout(() => {
      void isPortable()
        .then((value) => {
          if (!disposed) setPortable(value);
        })
        .catch(() => undefined);
      void checkUpdate()
        .then((value) => {
          if (disposed || !value.hasUpdate) return;
          setResult(value);
          setVisible(true);
          logClient("info", `update available: ${value.remote} (local ${value.local})`);
        })
        .catch((err) => logClient("warn", `update check failed: ${String(err)}`));
    }, STARTUP_DELAY);
    return () => {
      disposed = true;
      window.clearTimeout(timer);
    };
  }, []);

  useEffect(() => {
    if (!active) return;
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeUpdate({
      onProgress: (payload) => {
        if (!disposed) setPercent(payload.percent);
      },
      onError: (message) => {
        if (disposed) return;
        setPercent(null);
        setError(t("update.download_failed", { error: message }));
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [active, t]);

  if (!visible || !result) return null;

  const startDownload = () => {
    setError(null);
    setActive(true);
    setPercent(0);
    void downloadUpdate(result.remote).catch((err) => {
      setPercent(null);
      setError(t("update.download_failed", { error: String(err) }));
    });
  };

  return (
    <div className="update-toast" role="alert">
      <div className="update-toast-head">
        <strong>{t("update.new_title")}</strong>
        {percent === null && (
          <button
            className="update-toast-close"
            aria-label={t("button.close")}
            onClick={() => setVisible(false)}
          >
            ×
          </button>
        )}
      </div>
      {percent !== null ? (
        <div className="update-progress">
          <span>{percent >= 100 ? t("update.installing") : t("update.downloading", { percent })}</span>
          <div className="progress-track">
            <span style={{ width: `${percent}%` }} />
          </div>
        </div>
      ) : (
        <>
          <p>
            {error ??
              (portable
                ? t("update.toast_portable_body", {
                    version: result.remote,
                    local: result.local,
                  })
                : t("update.toast_body", { version: result.remote, local: result.local }))}
          </p>
          <div className="update-toast-actions">
            <button className="btn" onClick={() => setVisible(false)}>
              {t("button.close")}
            </button>
            {portable ? (
              <button
                className="btn primary"
                onClick={() => {
                  setVisible(false);
                  void openPath(config.downloadPage);
                }}
              >
                {t("update.go_download")}
              </button>
            ) : (
              <button className="btn primary" onClick={startDownload}>
                {t("update.download_install")}
              </button>
            )}
          </div>
        </>
      )}
    </div>
  );
}
