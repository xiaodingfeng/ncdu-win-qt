import { useCallback, useEffect, useMemo, useRef, useState, type MouseEvent } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import FileTable, { type SortKey } from "../components/FileTable";
import Treemap from "../components/Treemap";
import LegendBar from "../components/LegendBar";
import CleanupPanel from "../components/CleanupPanel";
import SystemOptPanel from "../components/SystemOptPanel";
import PathSyncDialog from "../components/PathSyncDialog";
import MovePathDialog from "../components/MovePathDialog";
import AiDialog from "../components/AiDialog";
import Breadcrumb from "../components/Breadcrumb";
import ContextMenu, { type MenuItem } from "../components/ContextMenu";
import LockerDialog from "../components/LockerDialog";
import Modal from "../components/Modal";
import BusyOverlay from "../components/BusyOverlay";
import PropertiesDialog from "../components/PropertiesDialog";
import {
  cancelScan,
  defaultPath,
  diskInfo,
  getDirectory,
  getScanRoot,
  getTreemap,
  searchTree,
  startScan,
  subscribeScan,
} from "../lib/scan";
import { deletePaths, openPath, revealPath } from "../lib/ops";
import { closeLockingProcesses } from "../lib/locker";
import { formatDuration, formatSize, formatCount, pathSegments } from "../lib/format";
import { typeKey } from "../lib/fileType";
import type { DirectoryView, DiskInfo, NodeEntry, ScanProgress, TreemapNode } from "../types/scan";
import type { LockingProcess } from "../types/locker";
import { isDir } from "../types/scan";

type PanelTab = "treemap" | "cleanup" | "sysopt";

interface ScanSummary {
  path: string;
  size: number;
  files: number;
  folders: number;
  elapsedMs: number;
  engine: string;
}

interface DeleteConfirm {
  permanent: boolean;
  paths: string[];
  names: string[];
}

interface RecycleFallback {
  paths: string[];
  lines: string[];
}

interface LockPrompt {
  paths: string[];
  permanent: boolean;
  procs: LockingProcess[];
}

function compareEntries(a: NodeEntry, b: NodeEntry, key: SortKey, parentSize: number): number {
  switch (key) {
    case "name":
      return a.name.localeCompare(b.name, undefined, { sensitivity: "base" });
    case "items":
      return a.fileCount + a.dirCount - (b.fileCount + b.dirCount);
    case "type":
      return typeKey(a).localeCompare(typeKey(b));
    case "percent":
      return parentSize > 0 ? a.size / parentSize - b.size / parentSize : a.size - b.size;
    case "size":
    default:
      return a.size - b.size;
  }
}

export default function ScanPage() {
  const { t, language } = useI18n();
  const { settings, update } = useSettings();

  const [path, setPath] = useState("");
  const [current, setCurrent] = useState<NodeEntry | null>(null);
  const [entries, setEntries] = useState<NodeEntry[]>([]);
  const [treemap, setTreemap] = useState<TreemapNode | null>(null);
  const [disk, setDisk] = useState<DiskInfo | null>(null);
  const [scanning, setScanning] = useState(false);
  const [cancelling, setCancelling] = useState(false);
  const [progress, setProgress] = useState<ScanProgress | null>(null);
  const [summary, setSummary] = useState<ScanSummary | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [query, setQuery] = useState("");
  const [results, setResults] = useState<NodeEntry[] | null>(null);
  const [sortKey, setSortKey] = useState<SortKey>("size");
  const [sortAsc, setSortAsc] = useState(false);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [tab, setTab] = useState<PanelTab>("treemap");
  const [cleanupMounted, setCleanupMounted] = useState(false);
  const [sysoptMounted, setSysoptMounted] = useState(false);
  const [scanSeq, setScanSeq] = useState(0);
  const [confirm, setConfirm] = useState<DeleteConfirm | null>(null);
  const [fallback, setFallback] = useState<RecycleFallback | null>(null);
  const [lockPrompt, setLockPrompt] = useState<LockPrompt | null>(null);
  const [busy, setBusy] = useState(false);
  const [busyText, setBusyText] = useState<string | null>(null);
  const [menu, setMenu] = useState<{ x: number; y: number; entry: NodeEntry } | null>(null);
  const [treeMenu, setTreeMenu] = useState<{ x: number; y: number; entry: NodeEntry } | null>(null);
  const [hiddenTypes, setHiddenTypes] = useState<Set<string>>(new Set());
  const [properties, setProperties] = useState<NodeEntry | null>(null);
  const [pathSyncOpen, setPathSyncOpen] = useState(false);
  const [moveTarget, setMoveTarget] = useState<string[] | null>(null);
  const [aiSession, setAiSession] = useState<{
    open: boolean;
    key: number;
    subject: string;
    prompt: string;
  }>({ open: false, key: 0, subject: "", prompt: "" });

  const skipHeavy = settings?.scan.skipHeavyDirs ?? true;
  const showFiles = settings?.scan.showFiles ?? true;
  const navigationId = useRef(0);
  const searchRef = useRef<HTMLInputElement>(null);

  const navigate = useCallback(async (target: string) => {
    const requestId = ++navigationId.current;
    try {
      const view: DirectoryView = await getDirectory(target);
      if (requestId !== navigationId.current) return;
      setCurrent(view.node);
      setEntries(view.children);
      setSelected(new Set());
      setQuery("");
      setResults(null);
      setError(null);
    } catch (err) {
      if (requestId === navigationId.current) setError(String(err));
    }
  }, []);

  const runScan = useCallback(
    async (target: string) => {
      const trimmed = target.trim();
      if (!trimmed) return;
      setError(null);
      setNotice(null);
      setProgress(null);
      setCancelling(false);
      setScanning(true);
      setResults(null);
      setQuery("");
      setSummary(null);
      setSelected(new Set());
      try {
        await startScan(trimmed, skipHeavy);
      } catch (err) {
        setScanning(false);
        setError(String(err));
      }
    },
    [skipHeavy],
  );

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeScan({
      onProgress: (payload) => {
        if (!disposed) setProgress(payload);
      },
      onFinished: (payload) => {
        if (disposed) return;
        setScanning(false);
        setCancelling(false);
        setProgress(null);
        setSummary({
          path: payload.root.path,
          size: payload.root.size,
          files: payload.root.fileCount,
          folders: payload.root.dirCount,
          elapsedMs: payload.elapsedMs,
          engine: payload.engine,
        });
        setPath(payload.root.path);
        setScanSeq((value) => value + 1);
        void navigate(payload.root.path);
      },
      onFailed: (payload) => {
        if (disposed) return;
        setScanning(false);
        setCancelling(false);
        setProgress(null);
        if (payload.code === "cancelled") {
          setNotice(t("status.cancelled"));
        } else {
          setError(t("status.scan_failed", { msg: payload.message }));
        }
      },
      onLowMemory: () => {
        if (!disposed) setNotice(t("status.low_memory"));
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [navigate, t]);

  useEffect(() => {
    let disposed = false;
    void defaultPath().then((home) => {
      if (disposed || !home) return;
      setPath(home);
    });
    return () => {
      disposed = true;
    };
  }, []);

  useEffect(() => {
    if (!current) return;
    let disposed = false;
    void getTreemap(current.path, 1, showFiles)
      .then((node) => {
        if (!disposed) setTreemap(node);
      })
      .catch(() => {
        if (!disposed) setTreemap(null);
      });
    void diskInfo(current.path)
      .then((info) => {
        if (!disposed) setDisk(info);
      })
      .catch(() => {
        if (!disposed) setDisk(null);
      });
    return () => {
      disposed = true;
    };
  }, [current, showFiles]);

  useEffect(() => {
    const trimmed = query.trim();
    if (!trimmed || !current) {
      setResults(null);
      return;
    }
    let disposed = false;
    const timer = window.setTimeout(() => {
      void searchTree(current.path, trimmed)
        .then((found) => {
          if (!disposed) setResults(found);
        })
        .catch(() => {
          if (!disposed) setResults([]);
        });
    }, 220);
    return () => {
      disposed = true;
      window.clearTimeout(timer);
    };
  }, [query, current]);

  const activeEntries = results ?? entries;
  const parentSize = current?.size ?? 0;

  const sortedEntries = useMemo(() => {
    const list = [...activeEntries];
    list.sort((a, b) => {
      const order = compareEntries(a, b, sortKey, parentSize);
      return sortAsc ? order : -order;
    });
    return list;
  }, [activeEntries, sortKey, sortAsc, parentSize]);

  const onSort = (key: SortKey) => {
    if (key === sortKey) {
      setSortAsc((value) => !value);
    } else {
      setSortKey(key);
      setSortAsc(key === "name" || key === "type");
    }
  };

  const activate = useCallback(
    (entry: NodeEntry) => {
      if (isDir(entry)) {
        void navigate(entry.path);
      } else {
        void openPath(entry.path).catch((err) => setError(String(err)));
      }
    },
    [navigate],
  );

  const select = (entry: NodeEntry, additive: boolean) => {
    setSelected((previous) => {
      const next = additive ? new Set(previous) : new Set<string>();
      if (additive && previous.has(entry.path)) next.delete(entry.path);
      else next.add(entry.path);
      return next;
    });
  };

  const browse = useCallback(async () => {
    const picked = await open({ directory: true, multiple: false, title: t("button.browse") });
    if (typeof picked === "string") {
      setPath(picked);
      await runScan(picked);
    }
  }, [runScan, t]);

  const buildAiPrompt = useCallback(
    (node: NodeEntry) => {
      const intro = t("ai.prompt.intro", { lang: language === "zh" ? "简体中文" : "English" });
      const kind = isDir(node) ? t("ai.prompt.kind_dir") : t("ai.prompt.kind_file");
      const children = [...entries]
        .sort((a, b) => b.size - a.size)
        .slice(0, 20)
        .map((entry) =>
          t("ai.prompt.children_line", { name: entry.name, size: formatSize(entry.size) }),
        )
        .join("\n");
      return t("ai.prompt.dir", {
        intro,
        path: node.path,
        kind,
        size: formatSize(node.size),
        files: formatCount(node.fileCount),
        folders: formatCount(node.dirCount),
        limit: 20,
        children,
      });
    },
    [entries, language, t],
  );

  useEffect(() => {
    const handler = (event: Event) => {
      const action = (event as CustomEvent<string>).detail;
      if (action === "open") {
        void browse();
      } else if (action === "rescan") {
        void runScan(current?.path ?? path);
      } else if (action === "refresh") {
        if (current) void navigate(current.path);
        else void runScan(path);
      } else if (action === "up") {
        const parent = current ? pathSegments(current.path).slice(0, -1).pop() : null;
        if (parent) void navigate(parent.path);
      } else if (action === "toggle-files" && settings) {
        update({ scan: { ...settings.scan, showFiles: !settings.scan.showFiles } });
      } else if (action === "app-sync") {
        setPathSyncOpen(true);
      } else if (action === "ai-analyze" && current) {
        const prompt = buildAiPrompt(current);
        setAiSession((previous) => ({
          open: true,
          key: previous.key + 1,
          subject: current.name,
          prompt,
        }));
      }
    };
    window.addEventListener("ncduwin:action", handler);
    return () => window.removeEventListener("ncduwin:action", handler);
  }, [browse, buildAiPrompt, current, navigate, path, runScan, settings, update]);

  const requestDelete = useCallback(
    (permanent: boolean, target?: NodeEntry) => {
      const source = target
        ? selected.has(target.path)
          ? activeEntries.filter((entry) => selected.has(entry.path))
          : [target]
        : activeEntries.filter((entry) => selected.has(entry.path));
      if (!source.length) return;
      setConfirm({
        permanent,
        paths: source.map((entry) => entry.path),
        names: source.map((entry) => entry.name),
      });
    },
    [activeEntries, selected],
  );

  const performDelete = async (paths: string[], permanent: boolean) => {
    setBusy(true);
    setBusyText(
      t(permanent ? "dialog.delete.progress" : "dialog.recycle.progress", { n: paths.length }),
    );
    setError(null);
    try {
      const result = await deletePaths(paths, permanent);
      const locked = result.failed.filter((item) => (item.lockedBy?.length ?? 0) > 0);
      const otherFailures = result.failed.filter((item) => !(item.lockedBy?.length ?? 0));
      if (locked.length > 0) {
        const procs: LockingProcess[] = [];
        for (const item of locked) {
          for (const process of item.lockedBy ?? []) {
            if (!procs.some((existing) => existing.pid === process.pid)) procs.push(process);
          }
        }
        setLockPrompt({ paths: locked.map((item) => item.path), permanent, procs });
      }
      if (!permanent && result.deleted === 0 && otherFailures.length > 0 && locked.length === 0) {
        setFallback({
          paths,
          lines: otherFailures.map((item) => `${item.path}: ${item.message}`),
        });
        return;
      }
      if (result.deleted > 0) {
        setNotice(
          permanent
            ? t("status.deleted", { n: result.deleted })
            : t("status.moved_recycle", { n: result.deleted }),
        );
      }
      if (otherFailures.length > 0) {
        setError(otherFailures.map((item) => `${item.path}: ${item.message}`).join("\n"));
      }
      if (locked.length === 0) {
        setConfirm(null);
        setFallback(null);
        setSelected(new Set());
        if (current) {
          await navigate(current.path);
          const root = await getScanRoot();
          if (root && summary?.path === root.path) {
            setSummary((previous) =>
              previous
                ? {
                    ...previous,
                    size: root.size,
                    files: root.fileCount,
                    folders: root.dirCount,
                  }
                : previous,
            );
          }
        }
      }
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
      setBusyText(null);
    }
  };

  const retryLockedDelete = async () => {
    if (!lockPrompt) return;
    const names = lockPrompt.procs.map((process) => process.name).join("、");
    const closable = lockPrompt.procs.filter((process) => process.safeToClose);
    if (!closable.length) {
      setError(t("op_guard.blocked_noclose", { procs: names }));
      setLockPrompt(null);
      return;
    }
    setBusy(true);
    setBusyText(
      t(lockPrompt.permanent ? "dialog.delete.progress" : "dialog.recycle.progress", {
        n: lockPrompt.paths.length,
      }),
    );
    try {
      const outcomes = await closeLockingProcesses(closable.map((process) => process.pid));
      const closed = outcomes.some(
        (outcome) => outcome.result === "exited" || outcome.result === "killed",
      );
      if (!closed) {
        setError(t("op_guard.blocked_body", { procs: names }));
        setLockPrompt(null);
        return;
      }
      const paths = lockPrompt.paths;
      const permanent = lockPrompt.permanent;
      setLockPrompt(null);
      await performDelete(paths, permanent);
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
      setBusyText(null);
    }
  };

  const openContextMenu = (entry: NodeEntry, event: MouseEvent) => {
    if (!selected.has(entry.path)) setSelected(new Set([entry.path]));
    setMenu({ x: event.clientX, y: event.clientY, entry });
  };

  const refreshAfterClean = useCallback(async () => {
    if (current) await navigate(current.path);
    const root = await getScanRoot();
    if (root && summary?.path === root.path) {
      setSummary((previous) =>
        previous
          ? {
              ...previous,
              size: root.size,
              files: root.fileCount,
              folders: root.dirCount,
            }
          : previous,
      );
    }
  }, [current, navigate, summary?.path]);

  const copyPath = async (entry: NodeEntry) => {
    try {
      await navigator.clipboard.writeText(entry.path);
      setNotice(t("status.copied", { path: entry.path }));
    } catch (err) {
      setError(String(err));
    }
  };

  const menuItems = (entry: NodeEntry): MenuItem[] => {
    const sources =
      selected.has(entry.path) && selected.size > 1
        ? activeEntries.filter((item) => selected.has(item.path)).map((item) => item.path)
        : [entry.path];
    const analyze = () => {
      const prompt = buildAiPrompt(entry);
      setAiSession((previous) => ({
        open: true,
        key: previous.key + 1,
        subject: entry.name,
        prompt,
      }));
    };
    const items: MenuItem[] = [];
    if (isDir(entry)) {
      items.push({ label: t("ctx.open_folder"), onClick: () => void openPath(entry.path) });
    } else {
      items.push({ label: t("ctx.open_file"), onClick: () => void openPath(entry.path) });
    }
    items.push(
      { label: t("ctx.reveal"), onClick: () => void revealPath(entry.path) },
      { label: t("ctx.copy_path"), onClick: () => void copyPath(entry) },
      { label: t("ctx.move_to"), onClick: () => setMoveTarget(sources) },
      { label: "", separator: true, onClick: () => undefined },
      { label: t("dialog.recycle.title"), onClick: () => requestDelete(false, entry) },
      {
        label: t("dialog.delete.title"),
        danger: true,
        onClick: () => requestDelete(true, entry),
      },
      { label: "", separator: true, onClick: () => undefined },
      { label: t("properties.title"), onClick: () => setProperties(entry) },
      { label: "", separator: true, onClick: () => undefined },
      { label: t("ctx.ai_analyze"), onClick: analyze },
    );
    return items;
  };

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      const typing =
        !!target &&
        (target.tagName === "INPUT" ||
          target.tagName === "TEXTAREA" ||
          target.tagName === "SELECT" ||
          target.isContentEditable);

      if (typing) {
        // Global shortcuts must keep working after the search box (or any
        // input) is focused; only text-editing keys are left to the field.
        const mod = event.ctrlKey || event.metaKey;
        const key = event.key.toLowerCase();
        if (event.key === "Escape") {
          setQuery("");
          target?.blur();
        } else if (event.key === "F5") {
          event.preventDefault();
          target?.blur();
          void runScan(current?.path ?? path);
        } else if (mod && key === "f") {
          event.preventDefault();
          searchRef.current?.focus();
        } else if (mod && key === "o") {
          event.preventDefault();
          target?.blur();
          void browse();
        }
        return;
      }

      if (event.key === "F5") {
        event.preventDefault();
        void runScan(current?.path ?? path);
      } else if (event.key === "Backspace") {
        event.preventDefault();
        const parent = current ? pathSegments(current.path).slice(0, -1).pop() : null;
        if (parent) void navigate(parent.path);
      } else if (event.key === "Delete") {
        event.preventDefault();
        requestDelete(event.shiftKey);
      } else if (event.key === "Enter" && selected.size === 1) {
        const only = activeEntries.find((entry) => selected.has(entry.path));
        if (only) activate(only);
      } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "f") {
        event.preventDefault();
        searchRef.current?.focus();
      } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "o") {
        event.preventDefault();
        void browse();
      } else if (event.key === "Escape") {
        setQuery("");
        setSelected(new Set());
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [activeEntries, browse, current, path, requestDelete, runScan, selected, activate, navigate]);

  const statusText = (() => {
    if (error) return error;
    if (scanning) {
      if (progress?.engine === "mft") {
        const phase =
          progress.phase === "enumerating"
            ? t("scan.phase.enumerating")
            : progress.phase === "readingMft"
              ? t("scan.phase.reading_mft")
              : progress.phase === "buildingTree"
                ? t("scan.phase.building_tree")
                : t("scan.engine_mft");
        const count = progress.count ? ` (${formatCount(progress.count)})` : "";
        return `${phase}${count}`;
      }
      const target = progress?.path || path;
      return cancelling ? t("status.cancelling") : t("status.scanning", { path: target });
    }
    if (notice) return notice;
    if (results) return t("status.search_results", { query: query.trim(), n: results.length });
    if (current) {
      const isRoot = summary?.path === current.path;
      if (isRoot && summary) {
        return t("status.scanned", {
          path: current.path,
          size: formatSize(summary.size),
          files: formatCount(summary.files),
          folders: formatCount(summary.folders),
          time: formatDuration(summary.elapsedMs, t),
        });
      }
      return t("status.current", {
        path: current.path,
        size: formatSize(current.size),
        files: formatCount(current.fileCount),
        folders: formatCount(current.dirCount),
      });
    }
    return t("status.ready");
  })();

  const engineText = (() => {
    const engine = scanning ? progress?.engine : summary?.engine;
    if (engine === "mft") return t("scan.engine_mft");
    if (engine === "walk") return t("scan.engine_walk");
    return "";
  })();

  const diskText = disk
    ? t("status.disk_free", {
        free: formatSize(disk.free),
        total: formatSize(disk.total),
        pct: disk.total > 0 ? Math.round((disk.free / disk.total) * 100) : 0,
      })
    : "";

  const confirmBody = confirm
    ? t(confirm.permanent ? "dialog.delete.body" : "dialog.recycle.body", {
        n: confirm.paths.length,
        names: confirm.names.slice(0, 5).join("\n"),
        more:
          confirm.names.length > 5
            ? t("dialog.recycle.more", { n: confirm.names.length - 5 })
            : "",
      })
    : "";

  return (
    <div className="scan-page">
      <div className="scan-toolbar">
        <input
          className="path-input"
          value={path}
          spellCheck={false}
          onChange={(event) => setPath(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "Enter") void runScan(path);
          }}
        />
        <button className="btn" onClick={() => void browse()}>
          {t("button.browse")}
        </button>
        {scanning ? (
          <button
            className="btn"
            disabled={cancelling}
            onClick={() => {
              setCancelling(true);
              void cancelScan();
            }}
          >
            {t("button.cancel")}
          </button>
        ) : (
          <button className="btn primary" onClick={() => void runScan(path)}>
            {t("button.scan")}
          </button>
        )}
        <button
          className="btn icon"
          title={t("button.refresh")}
          disabled={!current || scanning}
          onClick={() => current && void runScan(current.path)}
        >
          {t("button.refresh")}
        </button>
        <label className="check" title={t("tooltip.skip_heavy")}>
          <input
            type="checkbox"
            checked={skipHeavy}
            onChange={(event) =>
              update({ scan: { ...settings!.scan, skipHeavyDirs: event.target.checked } })
            }
          />
          {t("button.skip_heavy")}
        </label>
        <div className="toolbar-spacer" />
        <input
          ref={searchRef}
          className="search-input"
          value={query}
          placeholder={t("search.placeholder")}
          onChange={(event) => setQuery(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "Escape") setQuery("");
          }}
        />
      </div>

      <Breadcrumb path={current?.path ?? path} onNavigate={(target) => void navigate(target)} />

      <div className="scan-body">
        <div className="table-pane">
          <FileTable
            entries={sortedEntries}
            parentSize={parentSize}
            sortKey={sortKey}
            sortAsc={sortAsc}
            showPath={results !== null}
            emptyText={!current && !scanning ? t("scan.empty_hint") : undefined}
            selected={selected}
            onSort={onSort}
            onActivate={activate}
            onSelect={select}
            onContextMenu={openContextMenu}
          />
        </div>

        <div className="right-pane">
          <div className="tab-strip">
            {(["treemap", "cleanup", "sysopt"] as PanelTab[]).map((id) => (
              <button
                key={id}
                className={`tab ${tab === id ? "active" : ""}`}
                onClick={() => {
                  if (id === "cleanup") setCleanupMounted(true);
                  if (id === "sysopt") setSysoptMounted(true);
                  setTab(id);
                }}
              >
                {t(`tab.${id}`)}
              </button>
            ))}
            {tab === "treemap" && (
              <label className="check show-files">
                <input
                  type="checkbox"
                  checked={showFiles}
                  onChange={(event) =>
                    update({ scan: { ...settings!.scan, showFiles: event.target.checked } })
                  }
                />
                {t("menu.view.show_files")}
              </label>
            )}
          </div>

          <div className="panel-content">
            <div className="panel-slot" style={{ display: tab === "treemap" ? "flex" : "none" }}>
              <div className="treemap-pane">
                <Treemap
                  node={treemap}
                  onActivate={activate}
                  hiddenTypes={hiddenTypes}
                  onContextMenu={(entry, event) => {
                    if (!selected.has(entry.path)) setSelected(new Set([entry.path]));
                    setTreeMenu({ x: event.clientX, y: event.clientY, entry });
                  }}
                  filesHidden={!showFiles && entries.some((entry) => !isDir(entry))}
                  onShowFiles={() =>
                    update({ scan: { ...settings!.scan, showFiles: true } })
                  }
                />
                <LegendBar
                  hiddenTypes={hiddenTypes}
                  onToggle={(key) =>
                    setHiddenTypes((previous) => {
                      const next = new Set(previous);
                      if (next.has(key)) next.delete(key);
                      else next.add(key);
                      return next;
                    })
                  }
                />
              </div>
            </div>
            {cleanupMounted && (
              <div
                className="panel-slot"
                style={{ display: tab === "cleanup" ? "flex" : "none" }}
              >
                <CleanupPanel
                  root={summary?.path ?? current?.path ?? null}
                  scanSeq={scanSeq}
                  disk={disk}
                  onCleaned={refreshAfterClean}
                  onAiAnalyze={(subject, prompt) =>
                    setAiSession((previous) => ({
                      open: true,
                      key: previous.key + 1,
                      subject,
                      prompt,
                    }))
                  }
                />
              </div>
            )}
            {sysoptMounted && (
              <div
                className="panel-slot"
                style={{ display: tab === "sysopt" ? "flex" : "none" }}
              >
                <SystemOptPanel onOpenSync={() => setPathSyncOpen(true)} />
              </div>
            )}
          </div>
        </div>
      </div>

      <footer className="status-bar">
        <span className="status-text" title={statusText}>
          {statusText}
        </span>
        {engineText && (
          <span className="status-engine" title={t("scan.engine_tip")}>
            {engineText}
          </span>
        )}
        <span className="status-disk">{diskText}</span>
      </footer>

      {menu && (
        <ContextMenu
          x={menu.x}
          y={menu.y}
          items={menuItems(menu.entry)}
          onClose={() => setMenu(null)}
        />
      )}

      {treeMenu && (
        <ContextMenu
          x={treeMenu.x}
          y={treeMenu.y}
          items={menuItems(treeMenu.entry)}
          onClose={() => setTreeMenu(null)}
        />
      )}

      {properties && (
        <PropertiesDialog
          entry={properties}
          parentSize={parentSize}
          onClose={() => setProperties(null)}
        />
      )}

      <PathSyncDialog open={pathSyncOpen} onClose={() => setPathSyncOpen(false)} />

      {moveTarget && (
        <MovePathDialog
          open
          sources={moveTarget}
          onClose={() => setMoveTarget(null)}
          onDone={() => {
            setMoveTarget(null);
            if (current) void navigate(current.path);
          }}
        />
      )}

      <AiDialog
        key={aiSession.key}
        open={aiSession.open}
        subject={aiSession.subject}
        initialPrompt={aiSession.prompt}
        onClose={() => setAiSession((previous) => ({ ...previous, open: false }))}
        onConfigure={() => {
          setAiSession((previous) => ({ ...previous, open: false }));
          window.dispatchEvent(new CustomEvent("ncduwin:open-ai-settings"));
        }}
      />

      {confirm && (
        <Modal
          title={t(confirm.permanent ? "dialog.delete.title" : "dialog.recycle.title")}
          width={520}
          onClose={() => !busy && setConfirm(null)}
          footer={
            <>
              <button className="btn" disabled={busy} onClick={() => setConfirm(null)}>
                {t("button.cancel")}
              </button>
              <button
                className={`btn ${confirm.permanent ? "danger" : "primary"}`}
                disabled={busy}
                onClick={() => void performDelete(confirm.paths, confirm.permanent)}
              >
                {busy
                  ? t(
                      confirm.permanent
                        ? "dialog.delete.progress"
                        : "dialog.recycle.progress",
                      { n: confirm.paths.length },
                    )
                  : t(confirm.permanent ? "dialog.delete.title" : "dialog.recycle.title")}
              </button>
            </>
          }
        >
          <p className="dialog-text">{confirmBody}</p>
          {!confirm.permanent && (
            <button
              className="btn danger-outline"
              disabled={busy}
              onClick={() => void performDelete(confirm.paths, true)}
            >
              {t("dialog.delete.title")}
            </button>
          )}
        </Modal>
      )}

      {fallback && (
        <Modal
          title={t("dialog.recycle.fallback_title")}
          width={560}
          onClose={() => !busy && setFallback(null)}
          footer={
            <>
              <button className="btn" disabled={busy} onClick={() => setFallback(null)}>
                {t("button.cancel")}
              </button>
              <button
                className="btn danger"
                disabled={busy}
                onClick={() => void performDelete(fallback.paths, true)}
              >
                {t("dialog.delete.title")}
              </button>
            </>
          }
        >
          <p className="dialog-text">
            {t("dialog.recycle.fallback_body", { lines: fallback.lines.join("\n") })}
          </p>
        </Modal>
      )}
      {lockPrompt && (
        <LockerDialog
          title={t("op_guard.title")}
          intro={t("op_guard.intro")}
          procs={lockPrompt.procs}
          busy={busy}
          onCancel={() => !busy && setLockPrompt(null)}
          onCloseAndContinue={() => void retryLockedDelete()}
        />
      )}

      <BusyOverlay open={busy} label={busyText ?? ""} />
    </div>
  );
}
