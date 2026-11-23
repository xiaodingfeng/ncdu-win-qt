import { useCallback, useEffect, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import Fade from "./Fade";
import LockerDialog from "./LockerDialog";
import AppMoveDialog from "./AppMoveDialog";
import { formatSize } from "../lib/format";
import { openPath } from "../lib/ops";
import { closeLockingProcesses, lockedProcesses } from "../lib/locker";
import {
  cancelSaveMove,
  moveSaveContents,
  saveFolders,
  setSaveFolder,
  subscribeAppSync,
} from "../lib/appsync";
import type { SaveFolder, SavePair } from "../types/appsync";
import type { LockingProcess } from "../types/locker";

interface Props {
  open: boolean;
  onClose: () => void;
}

type Tab = "paths" | "apps";

interface MoveChoice {
  folder: SaveFolder;
  oldPath: string;
  newPath: string;
}

interface BatchConfirm {
  root: string;
  count: number;
}

interface Migration {
  pairs: SavePair[];
  total: number;
  copied: number;
  path: string;
}

interface LockPrompt {
  procs: LockingProcess[];
  intro: string;
  retry: () => void;
  round: number;
  doneText: string;
}

export default function PathSyncDialog({ open: visible, onClose }: Props) {
  const { t } = useI18n();

  const [tab, setTab] = useState<Tab>("paths");
  const [appsMounted, setAppsMounted] = useState(false);
  const [folders, setFolders] = useState<SaveFolder[]>([]);
  const [loading, setLoading] = useState(false);
  const [detectedAt, setDetectedAt] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [info, setInfo] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [migration, setMigration] = useState<Migration | null>(null);
  const [choice, setChoice] = useState<MoveChoice | null>(null);
  const [batch, setBatch] = useState<BatchConfirm | null>(null);
  const [lockPrompt, setLockPrompt] = useState<LockPrompt | null>(null);
  const cancelRef = useRef(false);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const list = await saveFolders();
      setFolders(list);
      setDetectedAt(new Date().toLocaleTimeString());
    } catch (err) {
      setError(String(err));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    if (!visible) return;
    setTab("paths");
    setAppsMounted(false);
    setInfo(null);
    setError(null);
    setChoice(null);
    setBatch(null);
    setLockPrompt(null);
    setMigration(null);
    void load();
  }, [visible, load]);

  useEffect(() => {
    if (!visible) return;
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeAppSync((payload) => {
      if (disposed) return;
      setMigration((previous) =>
        previous
          ? { ...previous, total: payload.total, copied: payload.copied, path: payload.path }
          : previous,
      );
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [visible]);

  const applyChange = async (folder: SaveFolder, newPath: string, moveFiles: boolean) => {
    setBusy(true);
    setError(null);
    setInfo(null);
    const oldPath = folder.path;
    try {
      await setSaveFolder(folder.id, newPath);
      setFolders((previous) =>
        previous.map((item) => (item.id === folder.id ? { ...item, path: newPath } : item)),
      );
      setInfo(t("app_sync.success"));
      if (moveFiles && oldPath) {
        await runMigration([{ from: oldPath, to: newPath }], t("app_sync.success"), 0);
      }
    } catch (err) {
      setError(String(err) === "app_sync.fail" ? t("app_sync.fail") : String(err));
    } finally {
      setBusy(false);
    }
  };

  const changeOne = async (folder: SaveFolder) => {
    const picked = await open({
      directory: true,
      multiple: false,
      title: t("app_sync.change_title", { name: t(folder.nameKey) }),
    });
    if (typeof picked !== "string") return;
    const target = picked.replace(/\//g, "\\").replace(/\\+$/, "");
    const current = folder.path.replace(/\//g, "\\").replace(/\\+$/, "");
    if (current && current.toLowerCase() === target.toLowerCase()) return;
    setChoice({ folder, oldPath: current, newPath: target });
  };

  const runMigration = async (pairs: SavePair[], doneText: string, round: number) => {
    if (!pairs.length) return;
    setBusy(true);
    setError(null);
    setInfo(null);
    cancelRef.current = false;
    setMigration({ pairs, total: 0, copied: 0, path: "" });
    try {
      const outcome = await moveSaveContents(pairs);
      setMigration(null);
      if (cancelRef.current) return;
      if (!outcome.leftovers.length) {
        setInfo(doneText);
        return;
      }
      const leftovers = outcome.leftovers;
      if (round < 2) {
        const procs = await lockedProcesses(
          leftovers.map((item) => item.from).slice(0, 8),
          400,
        );
        if (procs.some((process) => process.safeToClose)) {
          setLockPrompt({
            procs,
            intro:
              leftovers.length === 1
                ? t("app_sync.locked_intro", { name: leftovers[0].from })
                : t("app_sync.locked_intro_many", { count: leftovers.length }),
            retry: () =>
              void runMigration(
                leftovers.map((item) => ({ from: item.from, to: item.to })),
                doneText,
                round + 1,
              ),
            round,
            doneText,
          });
          return;
        }
      }
      setError(t(round === 0 ? "app_sync.move_partial" : "app_sync.retry_give_up"));
    } catch (err) {
      setMigration(null);
      setError(String(err));
    } finally {
      setBusy(false);
    }
  };

  const closeAndRetry = async () => {
    if (!lockPrompt) return;
    const closable = lockPrompt.procs.filter((process) => process.safeToClose);
    const retry = lockPrompt.retry;
    try {
      const outcomes = await closeLockingProcesses(closable.map((process) => process.pid));
      const closed = outcomes.some(
        (outcome) => outcome.result === "exited" || outcome.result === "killed",
      );
      setLockPrompt(null);
      if (closed) retry();
      else setError(t("app_sync.retry_give_up"));
    } catch (err) {
      setLockPrompt(null);
      setError(String(err));
    }
  };

  const startBatch = async () => {
    const picked = await open({
      directory: true,
      multiple: false,
      title: t("app_sync.select_target_root"),
    });
    if (typeof picked !== "string") return;
    const root = picked.replace(/\//g, "\\").replace(/\\+$/, "");
    setBatch({ root, count: folders.length });
  };

  const runBatch = async (root: string, moveFiles: boolean) => {
    setBatch(null);
    setBusy(true);
    setError(null);
    setInfo(null);
    const pairs: SavePair[] = [];
    let count = 0;
    const next = [...folders];
    for (let index = 0; index < next.length; index += 1) {
      const folder = next[index];
      const target = `${root}\\${folder.folder}`;
      try {
        await setSaveFolder(folder.id, target);
        const old = folder.path;
        next[index] = { ...folder, path: target };
        count += 1;
        if (
          moveFiles &&
          old &&
          old.toLowerCase() !== target.toLowerCase()
        ) {
          pairs.push({ from: old, to: target });
        }
      } catch {
        // Keep the original location; the summary below counts what changed.
      }
    }
    setFolders(next);
    try {
      if (count === 0) {
        setError(t("app_sync.batch_none"));
        return;
      }
      const doneText = t("app_sync.batch_done", { count });
      if (pairs.length) await runMigration(pairs, doneText, 0);
      else setInfo(doneText);
    } finally {
      setBusy(false);
    }
  };

  const percent =
    migration && migration.total > 0
      ? Math.min(100, Math.round((migration.copied / migration.total) * 100))
      : null;

  const dialog = (
    <Modal
      title={t("app_sync.title")}
      width={900}
      resizable
      className="modal-fill"
      onClose={() => !busy && onClose()}
      footer={
        <>
          <button className="btn" disabled={busy} onClick={onClose}>
            {t("button.close")}
          </button>
        </>
      }
    >
      {/* 两个 TAB 共用同一行描述，保证切换时下方内容不跳动 */}
      <p className="dialog-text muted pathsync-desc">
        {tab === "paths" ? t("app_sync.desc") : t("app_move.tip")}
      </p>

      <div className="tab-strip pathtab-strip">
        <button
          className={`tab ${tab === "paths" ? "active" : ""}`}
          onClick={() => setTab("paths")}
        >
          {t("app_sync.tab_paths")}
        </button>
        <button className={`tab ${tab === "apps" ? "active" : ""}`} onClick={() => { setTab("apps"); setAppsMounted(true); }}>
          {t("app_move.tab")}
        </button>
      </div>

      <div className="pathsync" style={tab === "paths" ? undefined : { display: "none" }}>
          <div className="pathsync-bar">
            <span className="pathsync-summary">
              {t("app_sync.summary", { total: folders.length, time: detectedAt })}
            </span>
            <div className="toolbar-spacer" />
            <button
              className="btn small primary"
              disabled={busy || loading || folders.length === 0}
              title={t("app_sync.batch_tip")}
              onClick={() => void startBatch()}
            >
              {t("app_sync.batch")}
            </button>
            <button className="btn small" disabled={busy || loading} onClick={() => void load()}>
              {t("app_sync.rescan")}
            </button>
          </div>

          {loading && <div className="appmove-progress"><span>{t("app_sync.rescan")}…</span></div>}
          {error && <div className="appmove-error">{error}</div>}
          {info && <div className="appmove-info">{info}</div>}

          {migration && (
            <div className="appmove-progress">
              {percent === null ? (
                <span>{t("app_sync.move_moving", { done: "0 B", total: "…" })}</span>
              ) : (
                <span>
                  {t("app_sync.move_moving", {
                    done: formatSize(migration.copied),
                    total: formatSize(migration.total),
                  })}
                </span>
              )}
              {migration.path && <span className="mono appmove-progress-path">{migration.path}</span>}
              <div className="progress-track">
                <span style={{ width: `${percent ?? 0}%`, ...(percent === null ? { flex: 1 } : {}) }} />
              </div>
              <button
                className="btn small"
                onClick={() => {
                  cancelRef.current = true;
                  void cancelSaveMove();
                }}
              >
                {t("button.cancel")}
              </button>
            </div>
          )}

          <div className="pathsync-table">
            <div className="pathsync-row head">
              <span>{t("app_sync.col_app")}</span>
              <span>{t("app_sync.col_path")}</span>
              <span>{t("app_sync.col_action")}</span>
            </div>
            {folders.map((folder) => (
              <div className="pathsync-row" key={folder.id}>
                <span className="pathsync-name">{t(folder.nameKey)}</span>
                {folder.path ? (
                  <button
                    className="pathsync-path"
                    title={t("app_sync.open_tip", { path: folder.path })}
                    onClick={() => void openPath(folder.path)}
                  >
                    {folder.path}
                  </button>
                ) : (
                  <span className="pathsync-path muted" title={t("app_sync.path_unknown")}>
                    {t("app_sync.path_unknown")}
                  </span>
                )}
                <span className="pathsync-actions">
                  <button
                    className="btn tiny"
                    disabled={busy}
                    title={t("app_sync.change_tip")}
                    onClick={() => void changeOne(folder)}
                  >
                    {t("app_sync.change")}
                  </button>
                  <button
                    className="btn tiny"
                    disabled={
                      busy ||
                      !folder.defaultPath ||
                      folder.path.replace(/\\+$/, "").toLowerCase() ===
                        folder.defaultPath.replace(/\\+$/, "").toLowerCase()
                    }
                    title={t("app_sync.reset_tip", { path: folder.defaultPath })}
                    onClick={() =>
                      setChoice({
                        folder,
                        oldPath: folder.path,
                        newPath: folder.defaultPath,
                      })
                    }
                  >
                    {t("app_sync.reset_default")}
                  </button>
                </span>
              </div>
            ))}
          </div>
        </div>

      {appsMounted && (
        <div
          className="pathsync-apps"
          style={tab === "apps" ? undefined : { display: "none" }}
        >
          <AppMoveDialog open embedded onClose={() => {}} />
        </div>
      )}

      <Fade show={Boolean(choice)}>
        {choice && (
          <Modal
            title={t("app_sync.move_dlg_title")}
            width={560}
            onClose={() => setChoice(null)}
            footer={
              <>
                <button className="btn" onClick={() => setChoice(null)}>
                  {t("button.cancel")}
                </button>
                <button
                  className="btn"
                  onClick={() => {
                    const pending = choice;
                    setChoice(null);
                    if (pending) void applyChange(pending.folder, pending.newPath, false);
                  }}
                >
                  {t("app_sync.move_dlg_no")}
                </button>
                <button
                  className="btn primary"
                  onClick={() => {
                    const pending = choice;
                    setChoice(null);
                    if (pending) void applyChange(pending.folder, pending.newPath, true);
                  }}
                >
                  {t("app_sync.move_dlg_yes")}
                </button>
              </>
            }
          >
            <p className="dialog-text">{t("app_sync.move_dlg_question")}</p>
            <p className="dialog-text">{t("app_sync.move_dlg_new", { new: choice.newPath })}</p>
            <p className="dialog-text muted">
              {t("app_sync.move_dlg_old", { old: choice.oldPath })}
            </p>
            <p className="dialog-text muted">{t("app_sync.move_dlg_hint")}</p>
          </Modal>
        )}
      </Fade>

      <Fade show={Boolean(batch)}>
        {batch && (
          <Modal
            title={t("app_sync.title")}
            width={600}
            onClose={() => setBatch(null)}
            footer={
              <>
                <button className="btn" onClick={() => setBatch(null)}>
                  {t("button.cancel")}
                </button>
                <button className="btn" onClick={() => void runBatch(batch.root, false)}>
                  {t("app_sync.move_dlg_no")}
                </button>
                <button className="btn primary" onClick={() => void runBatch(batch.root, true)}>
                  {t("app_sync.move_dlg_yes")}
                </button>
              </>
            }
          >
            <p className="dialog-text pre">{t("app_sync.batch_move_ask")}</p>
            <p className="dialog-text">{batch.root}</p>
          </Modal>
        )}
      </Fade>

      <Fade show={Boolean(lockPrompt)}>
        {lockPrompt && (
          <LockerDialog
            title={t("app_sync.title")}
            intro={lockPrompt.intro}
            procs={lockPrompt.procs}
            busy={busy}
            allowSkip={false}
            closeText={t("app_sync.btn_close_retry")}
            onCancel={() => {
              setLockPrompt(null);
              setError(t(lockPrompt.round === 0 ? "app_sync.move_partial" : "app_sync.retry_give_up"));
            }}
            onCloseAndContinue={() => void closeAndRetry()}
          />
        )}
      </Fade>
    </Modal>
  );

  return <Fade show={visible}>{dialog}</Fade>;
}
