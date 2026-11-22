import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import LockerDialog from "./LockerDialog";
import { IconCaret } from "./icons";
import { formatSize } from "../lib/format";
import {
  cleanAppResidue,
  moveAppDirs,
  restoreAppDir,
  scanAppDirs,
  subscribeAppMove,
} from "../lib/appmove";
import { closeLockingProcesses, lockedProcesses } from "../lib/locker";
import type { AppDir, MoveFinished, MoveItemResult, MoveProgress } from "../types/appmove";
import type { LockingProcess } from "../types/locker";

type Filter = "all" | "normal" | "moved" | "residue";
type Busy = "move" | "restore" | "residue" | null;
type SortKey = "name" | "size" | "state" | "path";

interface Props {
  open: boolean;
  onClose: () => void;
  /** Renders the panel without its own modal, for use inside another dialog's tab. */
  embedded?: boolean;
}

interface ConfirmState {
  body: string;
  run: () => void;
}

interface LockPrompt {
  procs: LockingProcess[];
  intro: string;
  allowSkip: boolean;
  retry: (() => void) | null;
  retryInfo: string;
}

const ROOTS_KEY = "ncduwin.appmove.customRoots";

/** Last directory picked in this session; never persisted. */
let lastTargetDir = "";

function restoreWindowFocus() {
  void getCurrentWindow().setFocus().catch(() => undefined);
}

/** Mirrors move_one(): target_root/<parent folder>/<name>. */
function targetPathFor(source: string, root: string): string {
  const cleaned = source.replace(/[\\/]+$/, "");
  const parts = cleaned.split(/[\\/]/);
  const name = parts.pop() ?? "";
  const parent = parts.pop() || "AppData";
  return `${root.replace(/[\\/]+$/, "")}\\${parent}\\${name}`;
}

export default function AppMoveDialog({ open: visible, onClose, embedded = false }: Props) {
  const { t } = useI18n();

  const [dirs, setDirs] = useState<AppDir[]>([]);
  const [scanning, setScanning] = useState(false);
  const [filter, setFilter] = useState<Filter>("all");
  const [query, setQuery] = useState("");
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [customRoots, setCustomRoots] = useState<string[]>(() => {
    try {
      const raw = localStorage.getItem(ROOTS_KEY);
      return raw ? (JSON.parse(raw) as string[]) : [];
    } catch {
      return [];
    }
  });
  const [busy, setBusy] = useState<Busy>(null);
  const [progress, setProgress] = useState<MoveProgress | null>(null);
  const [results, setResults] = useState<MoveFinished | null>(null);
  const [confirm, setConfirm] = useState<ConfirmState | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [info, setInfo] = useState<string | null>(null);
  const [checking, setChecking] = useState(false);
  const [lockPrompt, setLockPrompt] = useState<LockPrompt | null>(null);
  const [sort, setSort] = useState<{ key: SortKey; asc: boolean }>({ key: "size", asc: false });

  const waiters = useRef<((payload: MoveFinished) => void)[]>([]);

  const waitFinish = () => new Promise<MoveFinished>((resolve) => waiters.current.push(resolve));

  const scan = useCallback(async () => {
    setScanning(true);
    setError(null);
    try {
      const list = await scanAppDirs(customRoots.length > 0 ? customRoots : undefined);
      setDirs(list);
      setSelected((previous) => {
        const next = new Set<string>();
        for (const dir of list) {
          if (previous.has(dir.path)) next.add(dir.path);
        }
        return next;
      });
    } catch (err) {
      setError(String(err));
    } finally {
      setScanning(false);
    }
  }, [customRoots]);

  useEffect(() => {
    if (!visible) return;
    void scan();
    // Scan when the dialog is opened.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible]);

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeAppMove({
      onProgress: (payload) => {
        if (!disposed) setProgress(payload);
      },
      onFinished: (payload) => {
        if (disposed) return;
        const waiter = waiters.current.shift();
        waiter?.(payload);
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [scan]);

  const counts = useMemo(() => {
    return {
      all: dirs.length,
      normal: dirs.filter((dir) => dir.state === "normal").length,
      moved: dirs.filter((dir) => dir.state === "moved").length,
      residue: dirs.filter((dir) => dir.state === "residue").length,
    };
  }, [dirs]);

  const visibleDirs = useMemo(() => {
    const needle = query.trim().toLowerCase();
    const list = dirs.filter((dir) => {
      if (filter !== "all" && dir.state !== filter) return false;
      if (!needle) return true;
      return dir.name.toLowerCase().includes(needle) || dir.path.toLowerCase().includes(needle);
    });
    const order = (a: AppDir, b: AppDir) => {
      switch (sort.key) {
        case "name":
          return a.name.localeCompare(b.name, undefined, { sensitivity: "base" });
        case "state":
          return a.state.localeCompare(b.state);
        case "path":
          return a.path.localeCompare(b.path, undefined, { sensitivity: "base" });
        default:
          return a.size - b.size;
      }
    };
    list.sort((a, b) => (sort.asc ? order(a, b) : -order(a, b)));
    return list;
  }, [dirs, filter, query, sort]);

  const selectedDirs = dirs.filter((dir) => selected.has(dir.path));
  const selectedSize = selectedDirs.reduce((sum, dir) => sum + dir.size, 0);
  const moveTargets = selectedDirs.filter((dir) => dir.state === "normal");
  const restoreTargets = selectedDirs.filter((dir) => dir.state === "moved");
  const residueTargets = selectedDirs.filter((dir) => dir.state === "residue");

  const toggle = (path: string) => {
    setSelected((previous) => {
      const next = new Set(previous);
      if (next.has(path)) next.delete(path);
      else next.add(path);
      return next;
    });
  };

  /** Unchecks entries that finished successfully. */
  const dropMoved = (items: MoveItemResult[]) => {
    const done = new Set(items.filter((item) => item.ok).map((item) => item.path));
    if (!done.size) return;
    setSelected((previous) => {
      const next = new Set(previous);
      for (const path of done) next.delete(path);
      return next;
    });
  };

  const selectVisible = (on: boolean) => {
    setSelected((previous) => {
      const next = new Set(previous);
      for (const dir of visibleDirs) {
        if (on) next.add(dir.path);
        else next.delete(dir.path);
      }
      return next;
    });
  };

  const saveRoots = (roots: string[]) => {
    setCustomRoots(roots);
    localStorage.setItem(ROOTS_KEY, JSON.stringify(roots));
  };

  const addRoot = async () => {
    const picked = await open({ directory: true, multiple: true, title: t("app_move.scope_add_title") });
    const list = typeof picked === "string" ? [picked] : Array.isArray(picked) ? picked : [];
    if (list.length) saveRoots([...new Set([...customRoots, ...list])]);
  };

  const pickTarget = async (): Promise<string | null> => {
    const picked = await open({
      directory: true,
      multiple: false,
      defaultPath: lastTargetDir || undefined,
      title: t("app_move.select_target"),
    });
    if (typeof picked !== "string") return null;
    lastTargetDir = picked;
    return picked;
  };

  const inspectLocks = async (
    items: MoveItemResult[],
    intro: string,
    retry: () => void,
    retryInfo: string,
  ) => {
    const failed = items.filter((item) => !item.ok).map((item) => item.path);
    if (!failed.length) return;
    try {
      const procs = await lockedProcesses(failed.slice(0, 8), 200);
      if (procs.length) {
        setLockPrompt({ procs, intro, allowSkip: false, retry, retryInfo });
      }
    } catch {
      // Lock probing is best effort; the move result already explains the failure.
    }
  };

  const checkLocksThen = async (
    paths: string[],
    intro: string,
    retryInfo: string,
    next: () => void,
  ) => {
    setChecking(true);
    setError(null);
    setInfo(null);
    try {
      const procs = await lockedProcesses(paths.slice(0, 8), 200);
      if (!procs.length) {
        next();
        return;
      }

      const blocked = procs.filter((process) => !process.safeToClose);
      if (blocked.length) {
        const self = blocked.some(
          (process) => process.blockKey === "proc_close.block_self",
        );
        setError(
          t(self ? "app_move.locked_self" : "app_move.locked_system", {
            names: blocked.map((process) => process.name).join("、"),
          }),
        );
        setLockPrompt({ procs, intro, allowSkip: true, retry: next, retryInfo });
        return;
      }

      // Other apps are closed silently in the background, then the move runs.
      setInfo(t("app_move.auto_closing"));
      const outcomes = await closeLockingProcesses(
        procs.map((process) => process.pid),
      );
      const survived = new Set(
        outcomes
          .filter(
            (outcome) =>
              outcome.result !== "exited" && outcome.result !== "killed",
          )
          .map((outcome) => outcome.pid),
      );
      if (survived.size > 0) {
        setLockPrompt({
          procs: procs.filter((process) => survived.has(process.pid)),
          intro,
          allowSkip: true,
          retry: next,
          retryInfo,
        });
        return;
      }
      setInfo(null);
      next();
    } catch (err) {
      setError(String(err));
    } finally {
      setChecking(false);
    }
  };

  const closeAndContinue = async () => {
    if (!lockPrompt) return;
    const closable = lockPrompt.procs.filter((process) => process.safeToClose);
    if (!closable.length) {
      setError(t("app_move.locked_noclose"));
      return;
    }
    const action = lockPrompt.retry;
    setLockPrompt(null);
    setInfo(lockPrompt.retryInfo);
    try {
      const outcomes = await closeLockingProcesses(closable.map((process) => process.pid));
      const closed = outcomes.some(
        (outcome) => outcome.result === "exited" || outcome.result === "killed",
      );
      if (!closed) {
        setError(t("app_move.locked_noclose"));
        return;
      }
      action?.();
    } catch (err) {
      setError(String(err));
    }
  };

  const runMove = async (root: string) => {
    setBusy("move");
    setResults(null);
    setError(null);
    setInfo(null);
    try {
      const wait = waitFinish();
      await moveAppDirs(moveTargets.map((dir) => dir.path), root);
      const outcome = await wait;
      setResults(outcome);
      dropMoved(outcome.items);
      await inspectLocks(
        outcome.items,
        t("app_move.locked_fail_intro", { name: moveTargets[0]?.name ?? "" }),
        () => void runMove(root),
        t("app_move.closed_retry"),
      );
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
      setProgress(null);
      restoreWindowFocus();
      void scan();
    }
  };

  const requestMove = async () => {
    if (!moveTargets.length) {
      setError(t("app_move.none_selected"));
      return;
    }
    const root = await pickTarget();
    if (!root) return;
    const inside = moveTargets.some((dir) => {
      const target = root.replace(/\\/g, "/").toLowerCase();
      return target.startsWith(`${dir.path.replace(/\\/g, "/").toLowerCase()}/`);
    });
    if (inside) {
      setError(t("app_move.target_invalid"));
      return;
    }
    const names = moveTargets.map((dir) => dir.name).slice(0, 3).join("、");
    const listed = moveTargets
      .slice(0, 8)
      .map((dir) => `${dir.path}  →  ${targetPathFor(dir.path, root)}`)
      .join("\n");
    const more =
      moveTargets.length > 8 ? `\n… +${moveTargets.length - 8}` : "";
    void checkLocksThen(
      moveTargets.map((dir) => dir.path),
      t("app_move.locked_intro", { name: names }),
      t("app_move.closed_retry"),
      () =>
        setConfirm({
          body: t("app_move.confirm", {
            count: moveTargets.length,
            list: `${listed}${more}`,
          }),
          run: () => void runMove(root),
        }),
    );
  };
  const requestRestore = () => {
    if (!restoreTargets.length) return;
    void checkLocksThen(
      restoreTargets.map((dir) => dir.path),
      t("app_move.restore_locked_intro", { name: restoreTargets[0].name }),
      t("app_move.closed_retry_back"),
      () =>
        setConfirm({
          body: t("app_move.restore_confirm", { name: restoreTargets[0].name }),
          run: () => void runRestore(),
        }),
    );
  };

  const runRestore = async () => {
    setBusy("restore");
    setResults(null);
    setError(null);
    setInfo(null);
    const collected: MoveFinished = { moved: 0, failed: 0, items: [] };
    try {
      for (const dir of restoreTargets) {
        const wait = waitFinish();
        await restoreAppDir(dir.path);
        const outcome = await wait;
        collected.moved += outcome.moved;
        collected.failed += outcome.failed;
        collected.items.push(...outcome.items);
      }
      setResults(collected);
      dropMoved(collected.items);
      await inspectLocks(
        collected.items,
        t("app_move.locked_fail_intro", { name: restoreTargets[0]?.name ?? "" }),
        () => void runRestore(),
        t("app_move.closed_retry_back"),
      );
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
      setProgress(null);
      restoreWindowFocus();
      void scan();
    }
  };

  const requestResidue = () => {
    if (!residueTargets.length) return;
    setConfirm({
      body: t("app_move.clean_residue_confirm", {
        name: residueTargets[0].name,
        path: residueTargets[0].path,
      }),
      run: () => void runResidue(),
    });
  };

  const runResidue = async () => {
    setBusy("residue");
    setResults(null);
    setError(null);
    try {
      const wait = waitFinish();
      await cleanAppResidue(residueTargets.map((dir) => dir.path));
      const outcome = await wait;
      setResults(outcome);
      dropMoved(outcome.items);
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
      setProgress(null);
      restoreWindowFocus();
      void scan();
    }
  };

  const runSingleRestore = async (dir: AppDir) => {
    setBusy("restore");
    setResults(null);
    setInfo(null);
    try {
      const wait = waitFinish();
      await restoreAppDir(dir.path);
      const outcome = await wait;
      setResults(outcome);
      dropMoved(outcome.items);
      await inspectLocks(
        outcome.items,
        t("app_move.locked_fail_intro", { name: dir.name }),
        () => void runSingleRestore(dir),
        t("app_move.closed_retry_back"),
      );
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
      setProgress(null);
      restoreWindowFocus();
      void scan();
    }
  };

  const runSingleResidue = async (dir: AppDir) => {
    setBusy("residue");
    setResults(null);
    try {
      const wait = waitFinish();
      await cleanAppResidue([dir.path]);
      const outcome = await wait;
      setResults(outcome);
      dropMoved(outcome.items);
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(null);
      setProgress(null);
      restoreWindowFocus();
      void scan();
    }
  };

  const renderMessage = (message: string): string => {
    if (message.startsWith("app_move.fail_verify|")) {
      const [, src, dst] = message.split("|");
      return t("app_move.fail_verify", { src: formatSize(Number(src) || 0), dst: formatSize(Number(dst) || 0) });
    }
    if (message.includes("|")) {
      const [key, ...rest] = message.split("|");
      if (key === "app_move.fail_link" || key === "app_move.fail_rename") {
        return t(key, { reason: rest.join("|") });
      }
      if (key === "move.err.size_mismatch") {
        return t(key, {
          value: `${formatSize(Number(rest[0]) || 0)} / ${formatSize(Number(rest[1]) || 0)}`,
        });
      }
      if (key === "move.err.count_mismatch") {
        return t(key, { value: `${rest[0] ?? 0} / ${rest[1] ?? 0}` });
      }
      if (key.startsWith("move.err.")) {
        return t(key, { value: rest.join(" / ") });
      }
    }
    const known = t(message);
    return known === message ? message : known;
  };

  const stateLabel = (dir: AppDir) => {
    if (dir.state === "moved") return t("app_move.state_moved");
    if (dir.state === "residue") return t("app_move.clean_residue");
    return t("app_move.state_idle");
  };

  const progressLabel = () => {
    if (!progress) return t("app_move.state_copying");
    if (progress.phase === "restoring") return t("app_move.state_restoring");
    if (progress.phase === "deleting") return t("app_move.state_deleting");
    return t("app_move.state_copying");
  };

  if (!visible) return null;

  const th = (key: SortKey, label: string) => (
    <button
      className={`appmove-th ${sort.key === key ? "active" : ""}`}
      onClick={() =>
        setSort((previous) =>
          previous.key === key
            ? { key, asc: !previous.asc }
            : { key, asc: key === "name" || key === "path" },
        )
      }
    >
      {label}
      {sort.key === key && <span className="sort-mark"><IconCaret up={sort.asc} /></span>}
    </button>
  );

  const footer = (
    <>
      <span className="appmove-summary">
        {t("app_move.summary", { count: selectedDirs.length, size: formatSize(selectedSize) })}
      </span>
      <div className="toolbar-spacer" />
      {residueTargets.length > 0 && (
        <button className="btn" disabled={busy !== null} onClick={requestResidue}>
          {t("app_move.clean_residue")}
        </button>
      )}
      {restoreTargets.length > 0 && (
        <button className="btn" disabled={busy !== null} onClick={requestRestore}>
          {t("app_move.restore")}
        </button>
      )}
      <button className="btn primary" disabled={busy !== null || checking || moveTargets.length === 0} onClick={() => void requestMove()}>
        {t("app_move.move")}
      </button>
      {!embedded && (
        <button className="btn" disabled={busy !== null} onClick={onClose}>
          {t("button.close")}
        </button>
      )}
    </>
  );

  const content = (
    <>
      <div className="appmove">
        <p className="dialog-text">{t("app_move.tip")}</p>
        <p className="appmove-warn">{t("app_move.warn")}</p>

        <div className="appmove-toolbar">
          <label className="check">
            {t("app_move.scope")}
            <span className="muted">{customRoots.length ? customRoots.join("; ") : t("app_move.scope_default")}</span>
          </label>
          <button className="btn small" onClick={() => void addRoot()} disabled={busy !== null}>
            {t("app_move.scope_add")}
          </button>
          {customRoots.length > 0 && (
            <button className="btn small" onClick={() => saveRoots([])} disabled={busy !== null}>
              {t("app_move.scope_remove")}
            </button>
          )}
          <div className="toolbar-spacer" />
          <input
            className="input small"
            placeholder={t("search.placeholder")}
            value={query}
            onChange={(event) => setQuery(event.target.value)}
          />
          <select
            className="select small"
            value={filter}
            onChange={(event) => setFilter(event.target.value as Filter)}
            title={t("app_move.filter_tip")}
          >
            <option value="all">{t("app_move.filter_all")} ({counts.all})</option>
            <option value="normal">{t("app_move.state_idle")} ({counts.normal})</option>
            <option value="moved">{t("app_move.state_moved")} ({counts.moved})</option>
            <option value="residue">{t("app_move.clean_residue")} ({counts.residue})</option>
          </select>
          <button className="btn small" onClick={() => selectVisible(true)} disabled={busy !== null}>
            {t("cleanup.select_all")}
          </button>
          <button className="btn small" onClick={() => selectVisible(false)} disabled={busy !== null}>
            {t("cleanup.deselect_all")}
          </button>
          <button className="btn small" onClick={() => void scan()} disabled={busy !== null || scanning}>
            {scanning ? t("app_move.scanning", { done: 0, total: 0 }) : t("app_move.scan")}
          </button>
        </div>

        {checking && (
          <div className="appmove-progress">
            <span>{t("app_move.state_checking")}</span>
            <div className="progress-track">
              <span className="indeterminate" />
            </div>
          </div>
        )}

        {busy && (
          <div className="appmove-progress">
            <span>{progressLabel()}</span>
            {progress && <span className="mono appmove-progress-path">{progress.path}</span>}
            <div className="progress-track">
              <span className="indeterminate" />
            </div>
          </div>
        )}

        {error && <div className="appmove-error">{error}</div>}
        {info && <div className="appmove-info">{info}</div>}

        <div className="appmove-table">
          <div className="appmove-row head">
            <span />
            {th("name", t("app_move.col_name"))}
            {th("size", t("app_move.col_size"))}
            {th("state", t("app_move.col_state"))}
            <span>{t("app_move.col_action")}</span>
          </div>
          {visibleDirs.length === 0 && (
            <div className="appmove-empty">
              {scanning ? t("app_move.scanning", { done: 0, total: 0 }) : t("app_move.filter_empty")}
            </div>
          )}
          {visibleDirs.map((dir) => (
            <div className={`appmove-row ${selected.has(dir.path) ? "selected" : ""}`} key={dir.path}>
              <input type="checkbox" checked={selected.has(dir.path)} onChange={() => toggle(dir.path)} />
              <div className="appmove-name">
                <span className="name">
                  {dir.name}
                  {dir.installed && (
                    <span className="appmove-badge" title={t("app_move.name_matched", { name: dir.name })}>
                      {t("app_move.name_matched", { name: dir.name })}
                    </span>
                  )}
                </span>
                <span className="path" title={dir.path}>
                  {dir.state === "moved" && dir.linkTarget ? `${dir.path} → ${dir.linkTarget}` : dir.path}
                </span>
              </div>
              <span className="mono">{formatSize(dir.size)}</span>
              <span>{stateLabel(dir)}</span>
              <span className="appmove-actions">
                {dir.state === "moved" && (
                  <button
                    className="btn tiny"
                    disabled={busy !== null}
                    onClick={() =>
                      setConfirm({
                        body: t("app_move.restore_confirm", { name: dir.name }),
                        run: () => void runSingleRestore(dir),
                      })
                    }
                  >
                    {t("app_move.restore")}
                  </button>
                )}
                {dir.state === "residue" && (
                  <button
                    className="btn tiny"
                    disabled={busy !== null}
                    onClick={() =>
                      setConfirm({
                        body: t("app_move.clean_residue_confirm", { name: dir.name, path: dir.path }),
                        run: () => void runSingleResidue(dir),
                      })
                    }
                  >
                    {t("app_move.clean_residue")}
                  </button>
                )}
              </span>
            </div>
          ))}
        </div>

        {results && (
          <div className="appmove-results">
            <div className="appmove-results-head">
              {t("app_move.done", { ok: results.moved, failed: results.failed })}
            </div>
            {results.items.map((item) => (
              <div className={`appmove-result ${item.ok ? "ok" : "bad"}`} key={`${item.path}-${item.message}`}>
                <span className="mono">{item.path}</span>
                <span>{renderMessage(item.message)}</span>
              </div>
            ))}
          </div>
        )}
      </div>

      {lockPrompt && (
        <LockerDialog
          title={t("app_move.title")}
          intro={lockPrompt.intro}
          procs={lockPrompt.procs}
          busy={busy !== null}
          allowSkip={lockPrompt.allowSkip}
          closeText={t("op_guard.btn_close")}
          skipText={t("app_move.btn_skip_dir")}
          onCancel={() => setLockPrompt(null)}
          onCloseAndContinue={() => void closeAndContinue()}
          onSkip={() => {
            setLockPrompt(null);
            setError(t("app_move.fail_locked_skip"));
          }}
        />
      )}

      {confirm && (
        <Modal
          title={t("app_move.title")}
          width={600}
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
          <p className="dialog-text pre">{confirm.body}</p>
        </Modal>
      )}
    </>
  );

  if (embedded) {
    return (
      <div className="appmove-embedded">
        {content}
        <div className="appmove-footer">{footer}</div>
      </div>
    );
  }

  return (
    <Modal
      title={t("app_move.title")}
      width={1000}
      resizable
      className="modal-fill"
      onClose={() => {
        if (!busy) onClose();
      }}
      footer={footer}
    >
      {content}
    </Modal>
  );
}
