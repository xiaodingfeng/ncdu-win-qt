import { useCallback, useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import Fade from "./Fade";
import { IconBolt, IconNetwork, IconStore, IconSync, IconUpdate } from "./icons";
import {
  flushDns,
  getUpdateStatus,
  resetStore,
  runNetworkFix,
  setUpdateEnabled,
} from "../lib/sysopt";
import type { UpdateStatus } from "../types/sysopt";

type JobKey = "update" | "netfix" | "dns" | "store";

interface ConfirmState {
  title: string;
  body: string;
  run: () => void;
}

interface Props {
  onOpenSync: () => void;
}

export default function SystemOptPanel({ onOpenSync }: Props) {
  const { t } = useI18n();

  const [status, setStatus] = useState<UpdateStatus | null>(null);
  const [busy, setBusy] = useState<JobKey | null>(null);
  const [messages, setMessages] = useState<Partial<Record<JobKey, string>>>({});
  const [errors, setErrors] = useState<Partial<Record<JobKey, string>>>({});
  const [confirm, setConfirm] = useState<ConfirmState | null>(null);

  const loadStatus = useCallback(async () => {
    try {
      setStatus(await getUpdateStatus());
    } catch (err) {
      setErrors((previous) => ({ ...previous, update: String(err) }));
    }
  }, []);

  useEffect(() => {
    void loadStatus();
  }, [loadStatus]);

  const setMessage = (key: JobKey, message: string | undefined) => {
    setMessages((previous) => ({ ...previous, [key]: message }));
    setErrors((previous) => ({ ...previous, [key]: undefined }));
  };

  const setError = (key: JobKey, message: string | undefined) => {
    setErrors((previous) => ({ ...previous, [key]: message }));
    setMessages((previous) => ({ ...previous, [key]: undefined }));
  };

  const runUpdate = useCallback(
    async (enable: boolean) => {
      setBusy("update");
      setMessage("update", t("sysopt.running"));
      try {
        const result = await setUpdateEnabled(enable);
        if (!result.ok) {
          setError("update", t("sysopt.op_failed"));
        } else if (result.needsReboot) {
          setMessage("update", t("sysopt.task_done_reboot", { total: result.total }));
        } else {
          setMessage("update", t(result.message || "sysopt.task_done", { total: result.total }));
        }
        // Service and policy changes settle a moment after the helper exits.
        await new Promise((resolve) => window.setTimeout(resolve, 400));
        await loadStatus();
      } catch (err) {
        setError("update", String(err));
      } finally {
        setBusy(null);
      }
    },
    [loadStatus, t],
  );

  const requestUpdate = (enable: boolean) => {
    setConfirm({
      title: t("sysopt.update.title"),
      body: t(enable ? "sysopt.update.confirm_enable" : "sysopt.update.confirm_disable"),
      run: () => void runUpdate(enable),
    });
  };

  const runJob = async (
    key: Exclude<JobKey, "update">,
    action: () => Promise<{ ok: boolean; needsReboot: boolean; message: string; total: number }>,
  ) => {
    setBusy(key);
    setMessage(key, t("sysopt.running"));
    try {
      const result = await action();
      if (!result.ok) {
        setError(key, t("sysopt.op_failed"));
      } else {
        const messageKey =
          result.message || (result.needsReboot ? "sysopt.task_done_reboot" : "sysopt.task_done");
        setMessage(key, t(messageKey, { total: result.total }));
      }
    } catch (err) {
      setError(key, String(err));
    } finally {
      setBusy(null);
    }
  };

  const statusLine = (key: JobKey) => {
    if (errors[key]) return <span className="sysopt-error">{errors[key]}</span>;
    if (messages[key]) return <span className="sysopt-message">{messages[key]}</span>;
    if (busy === key) return <span className="sysopt-running">{t("sysopt.running")}</span>;
    return null;
  };

  const updateEnabled = status ? !status.blocked : true;

  return (
    <div className="sysopt-panel">
      <div className="sysopt-header">
        <h3>{t("sysopt.title")}</h3>
        <p>{t("sysopt.desc")}</p>
      </div>

      <div className="sysopt-cards">
        <section className="sysopt-card">
          <div className="sysopt-card-head">
            <span className="sysopt-icon"><IconSync size={20} /></span>
            <div className="sysopt-card-title">
              <strong>
                {t("sysopt.sync.title")}
                <span className="sysopt-badge">{t("sysopt.sync.badge")}</span>
              </strong>
              <span>{t("sysopt.sync.desc")}</span>
            </div>
            <button className="btn small primary" disabled={busy !== null} onClick={onOpenSync}>
              {t("sysopt.sync.btn")}
            </button>
          </div>
        </section>

        <section className="sysopt-card">
          <div className="sysopt-card-head">
            <span className="sysopt-icon"><IconUpdate size={20} /></span>
            <div className="sysopt-card-title">
              <strong>{t("sysopt.update.title")}</strong>
              <span>{t("sysopt.update.desc")}</span>
            </div>
            <button
              className="btn tiny"
              disabled={busy !== null}
              title={t("button.refresh")}
              onClick={() => void loadStatus()}
            >
              {t("button.refresh")}
            </button>
            <label className="switch" title={t(updateEnabled ? "sysopt.update.tooltip_on" : "sysopt.update.tooltip_off")}>
              <input
                type="checkbox"
                checked={updateEnabled}
                disabled={busy !== null}
                onChange={(event) => requestUpdate(event.target.checked)}
              />
              <span className="switch-track" />
            </label>
          </div>
          <div className="sysopt-status">
            {status ? (
              status.blocked ? (
                <span className="sysopt-blocked">
                  {t("sysopt.update.status_blocked", {
                    reason: status.reasons.map((key) => t(key)).join(" · "),
                  })}
                </span>
              ) : (
                <span className="sysopt-ok">{t("sysopt.update.status_enabled")}</span>
              )
            ) : (
              <span className="sysopt-running">{t("sysopt.update.status_checking")}</span>
            )}
            {statusLine("update")}
          </div>
        </section>

        <section className="sysopt-card">
          <div className="sysopt-card-head">
            <span className="sysopt-icon"><IconNetwork size={20} /></span>
            <div className="sysopt-card-title">
              <strong>{t("sysopt.netfix.title")}</strong>
              <span>{t("sysopt.netfix.desc")}</span>
            </div>
            <button
              className="btn small primary"
              disabled={busy !== null}
              onClick={() =>
                setConfirm({
                  title: t("sysopt.netfix.title"),
                  body: t("sysopt.netfix.confirm"),
                  run: () => void runJob("netfix", runNetworkFix),
                })
              }
            >
              {t("sysopt.netfix.btn")}
            </button>
          </div>
          <div className="sysopt-status">{statusLine("netfix")}</div>
        </section>

        <section className="sysopt-card">
          <div className="sysopt-card-head">
            <span className="sysopt-icon"><IconBolt size={20} /></span>
            <div className="sysopt-card-title">
              <strong>{t("sysopt.dns.title")}</strong>
              <span>{t("sysopt.dns.desc")}</span>
            </div>
            <button
              className="btn small primary"
              disabled={busy !== null}
              onClick={() =>
                setConfirm({
                  title: t("sysopt.dns.title"),
                  body: t("sysopt.dns.confirm"),
                  run: () => void runJob("dns", flushDns),
                })
              }
            >
              {t("sysopt.dns.btn")}
            </button>
          </div>
          <div className="sysopt-status">{statusLine("dns")}</div>
        </section>

        <section className="sysopt-card">
          <div className="sysopt-card-head">
            <span className="sysopt-icon"><IconStore size={20} /></span>
            <div className="sysopt-card-title">
              <strong>{t("sysopt.store.title")}</strong>
              <span>{t("sysopt.store.desc")}</span>
            </div>
            <button
              className="btn small primary"
              disabled={busy !== null}
              onClick={() =>
                setConfirm({
                  title: t("sysopt.store.title"),
                  body: t("sysopt.store.confirm"),
                  run: () => void runJob("store", resetStore),
                })
              }
            >
              {t("sysopt.store.btn")}
            </button>
          </div>
          <div className="sysopt-status">{statusLine("store")}</div>
        </section>
      </div>

      <Fade show={Boolean(confirm)}>
        {confirm && (
          <Modal
            title={confirm.title}
            width={520}
            onClose={() => setConfirm(null)}
            footer={
              <>
                <button className="btn" onClick={() => setConfirm(null)}>
                  {t("button.cancel")}
                </button>
                <button
                  className="btn primary"
                  onClick={() => {
                    const action = confirm.run;
                    setConfirm(null);
                    action();
                  }}
                >
                  {t("button.yes")}
                </button>
              </>
            }
          >
            <p className="dialog-text">{confirm.body}</p>
          </Modal>
        )}
      </Fade>
    </div>
  );
}
