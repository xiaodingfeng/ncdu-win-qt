import { useEffect, useMemo, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import Modal from "./Modal";
import Fade from "./Fade";
import LockerDialog from "./LockerDialog";
import { movePath, subscribeAppMove } from "../lib/appmove";
import { closeLockingProcesses, lockedProcesses } from "../lib/locker";
import type { MoveFinished, MoveProgress } from "../types/appmove";
import type { LockingProcess } from "../types/locker";

interface Props {
  open: boolean;
  sources: string[];
  onClose: () => void;
  onDone: () => void;
}

function guessTarget(source: string): string {
  const drive = source.slice(0, 1).toUpperCase();
  const candidate = drive === "D" ? "E" : "D";
  return `${candidate}:\\NcduWinMoved`;
}

export default function MovePathDialog({ open: visible, sources, onClose, onDone }: Props) {
  const { t } = useI18n();
  const { settings, update } = useSettings();
  const [target, setTarget] = useState("");
  const [makeDefault, setMakeDefault] = useState(false);
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState<MoveProgress | null>(null);
  const [result, setResult] = useState<MoveFinished | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [lockPrompt, setLockPrompt] = useState<LockingProcess[] | null>(null);

  useEffect(() => {
    if (!visible || !settings) return;
    setTarget(settings.defaultMovePath || guessTarget(sources[0] ?? ""));
    setMakeDefault(false);
    setResult(null);
    setError(null);
    setProgress(null);
  }, [visible, settings, sources]);

  useEffect(() => {
    if (!visible) return;
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeAppMove({
      onProgress: (payload) => {
        if (!disposed) setProgress(payload);
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [visible]);

  const describe = (message: string): string => {
    if (!message) return "";
    if (message.includes("|")) {
      const [key, ...rest] = message.split("|");
      return t(key, { value: rest.join(" | ") });
    }
    const known = t(message);
    return known === message ? message : known;
  };

  const failures = useMemo(
    () => (result ? result.items.filter((item) => !item.ok) : []),
    [result],
  );

  const browse = async () => {
    const picked = await open({
      directory: true,
      multiple: false,
      title: t("move.select_folder"),
    });
    if (typeof picked === "string") setTarget(picked);
  };

  const runMove = async () => {
    setBusy(true);
    setError(null);
    setResult(null);
    try {
      const outcome = await movePath(sources, target.trim());
      setResult(outcome);
      if (outcome.moved > 0 && makeDefault) update({ defaultMovePath: target.trim() });
      if (outcome.failed === 0 && outcome.moved > 0) {
        onDone();
      }
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
      setProgress(null);
    }
  };

  const start = async () => {
    if (!target.trim()) return;
    setBusy(true);
    setError(null);
    try {
      const procs = await lockedProcesses(sources.slice(0, 8), 200);
      if (procs.length) {
        setLockPrompt(procs);
        return;
      }
    } catch {
      // Lock probing is best effort.
    } finally {
      setBusy(false);
    }
    await runMove();
  };

  const continueAfterClose = async () => {
    if (!lockPrompt) return;
    const closable = lockPrompt.filter((process) => process.safeToClose);
    setLockPrompt(null);
    if (closable.length) {
      try {
        await closeLockingProcesses(closable.map((process) => process.pid));
      } catch {
        // Continue anyway; the move itself reports what is still locked.
      }
    }
    await runMove();
  };

  return (
    <Fade show={visible}>
      <Modal
        title={t("move.title")}
        width={580}
        onClose={() => !busy && onClose()}
        footer={
          <>
            <button className="btn" disabled={busy} onClick={onClose}>
              {t("button.cancel")}
            </button>
            <button
              className="btn primary"
              disabled={busy || !target.trim() || sources.length === 0}
              onClick={() => void start()}
            >
              {busy ? t("move.progress_title") : t("move.btn_start")}
            </button>
          </>
        }
      >
        <div className="move-dialog">
          <p className="dialog-text">{t("move.tip")}</p>
          <label className="field">
            <span>{t("move.source")}</span>
            <div className="move-sources">
              {sources.map((source) => (
                <span className="mono" key={source} title={source}>
                  {source}
                </span>
              ))}
            </div>
          </label>
          <label className="field">
            <span>{t("move.target")}</span>
            <div className="field-row">
              <input
                className="text-input"
                value={target}
                spellCheck={false}
                onChange={(event) => setTarget(event.target.value)}
              />
              <button className="btn" disabled={busy} onClick={() => void browse()}>
                {t("button.browse")}
              </button>
            </div>
          </label>
          <label className="check">
            <input
              type="checkbox"
              checked={makeDefault}
              onChange={(event) => setMakeDefault(event.target.checked)}
            />
            {t("move.set_default")}
          </label>
          {progress && (
            <div className="move-progress">
              <div className="progress-track">
                <span
                  style={{
                    width: `${Math.round(
                      (Math.max(1, progress.index) / Math.max(1, progress.total)) * 100,
                    )}%`,
                  }}
                />
              </div>
              <span className="muted">
                {progress.index} / {progress.total}
              </span>
            </div>
          )}
          {error && <p className="cleanup-error">{error}</p>}
          {result && result.moved > 0 && (
            <p className="dialog-text">
              {t("move.success", { path: target.trim() })}
            </p>
          )}
          {failures.length > 0 && (
            <div className="move-failures">
              {failures.map((item) => (
                <p className="cleanup-error" key={item.path}>
                  {item.path}: {describe(item.message)}
                </p>
              ))}
            </div>
          )}
        </div>
      </Modal>

      <Fade show={Boolean(lockPrompt)}>
        {lockPrompt && (
          <LockerDialog
            title={t("move.title")}
            intro={t("app_move.locked_intro", {
              name: sources[0] ?? "",
            })}
            procs={lockPrompt}
            allowSkip
            onCancel={() => setLockPrompt(null)}
            onCloseAndContinue={() => void continueAfterClose()}
            onSkip={() => {
              setLockPrompt(null);
              void runMove();
            }}
          />
        )}
      </Fade>
    </Fade>
  );
}
