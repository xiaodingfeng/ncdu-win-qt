import { useCallback, useEffect, useMemo, useRef, useState, type MouseEvent } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import FileTable, { type SortKey } from "../components/FileTable";
import StatusBar from "./StatusBar";
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
import Fade from "../components/Fade";
import BusyOverlay from "../components/BusyOverlay";
import PropertiesDialog from "../components/PropertiesDialog";
import { IconDrive } from "../components/icons";
import { indexStatus, subscribeIndex } from "../lib/index";
import type { IndexStatus } from "../types/index";
import {
  cancelScan,
  defaultPath,
  diskInfo,
  getDirectory,
  getScanRoot,
  getTreemap,
  releaseScan,
  searchTree,
  startScan,
  subscribeScan,
} from "../lib/scan";
import { deletePaths, openPath, revealPath } from "../lib/ops";
import { closeLockingProcesses } from "../lib/locker";
import { formatSize, formatCount, pathSegments } from "../lib/format";
import { typeKey } from "../lib/fileType";
import type {
  DirectoryView,
  DiskInfo,
  NodeEntry,
  ScanSummary,
  TreemapNode,
} from "../types/scan";
import type { LockingProcess } from "../types/locker";
import { isDir } from "../types/scan";

type PanelTab = "treemap" | "cleanup" | "sysopt";

const PAGE_SIZE = 200;

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
  const [entriesTotal, setEntriesTotal] = useState(0);
  const [treemap, setTreemap] = useState<TreemapNode | null>(null);
  const [disk, setDisk] = useState<DiskInfo | null>(null);
  const [scanning, setScanning] = useState(false);
  const [cancelling, setCancelling] = useState(false);
  const [summary, setSummary] = useState<ScanSummary | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [indexInfo, setIndexInfo] = useState<IndexStatus | null>(null);
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
  const filesHidden = useMemo(
    () => !showFiles && entries.some((entry) => !isDir(entry)),
    [entries, showFiles],
  );
  const navigationId = useRef(0);
  const loadingMore = useRef(false);
  const searchRef = useRef<HTMLInputElement>(null);
  // Mirrors whether the backend currently holds a scan tree. A rescan drops
  // the old tree immediately, so navigation/search must stay silent until the
  // new tree is installed.
  const hasTreeRef = useRef(false);

  const navigate = useCallback(async (target: string) => {
    const requestId = ++navigationId.current;
    try {
      const view: DirectoryView = await getDirectory(target, 0, PAGE_SIZE);
      if (requestId !== navigationId.current) return;
      setCurrent(view.node);
      setEntries(view.children);
      setEntriesTotal(view.total);
      setSelected(new Set());
      setQuery("");
      setResults(null);
      setError(null);
    } catch (err) {
      if (requestId === navigationId.current && hasTreeRef.current) setError(String(err));
    }
  }, []);

  const runScan = useCallback(
    async (target: string) => {
      const trimmed = target.trim();
      // Repeated triggers (F5, menu, path Enter) must not stack scans.
      if (!trimmed || scanning) return;
      const hadTree = hasTreeRef.current;
      hasTreeRef.current = false;
      setError(null);
      setNotice(null);
      setCancelling(false);
      setScanning(true);
      setResults(null);
      setQuery("");
      setSummary(null);
      setSelected(new Set());
      try {
        await startScan(trimmed, skipHeavy);
      } catch (err) {
        // An invalid path is rejected before the backend drops the old tree.
        hasTreeRef.current = hadTree;
        setScanning(false);
        setError(String(err));
      }
    },
    [skipHeavy, scanning],
  );

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeScan({
      onProgress: () => undefined,
      onFinished: (payload) => {
        if (disposed) return;
        hasTreeRef.current = true;
        setScanning(false);
        setCancelling(false);
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
    if (!trimmed || !current || !hasTreeRef.current) {
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
  const isDefaultSort = sortKey === "size" && !sortAsc;

  const sortedEntries = useMemo(() => {
    if (isDefaultSort && !results) return activeEntries;
    const list = [...activeEntries];
    list.sort((a, b) => {
      const order = compareEntries(a, b, sortKey, parentSize);
      return sortAsc ? order : -order;
    });
    return list;
  }, [activeEntries, isDefaultSort, results, sortKey, sortAsc, parentSize]);

  const loadMore = useCallback(async () => {
    if (!current || loadingMore.current || entries.length >= entriesTotal) return;
    loadingMore.current = true;
    const requestId = navigationId.current;
    try {
      const view = await getDirectory(current.path, entries.length, PAGE_SIZE);
      if (requestId !== navigationId.current) return;
      setEntries((prev) => [...prev, ...view.children]);
      setEntriesTotal(view.total);
    } catch {
      /* ignore */
    } finally {
      loadingMore.current = false;
    }
  }, [current, entries.length, entriesTotal]);

  useEffect(() => {
    if (results || !current) return;
    if (sortKey === "size" && !sortAsc) return;
    if (entries.length >= entriesTotal) return;
    let disposed = false;
    void getDirectory(current.path)
      .then((view) => {
        if (disposed) return;
        setEntries(view.children);
        setEntriesTotal(view.total);
      })
      .catch(() => undefined);
    return () => {
      disposed = true;
    };
  }, [results, current, sortKey, sortAsc, entries.length, entriesTotal]);

  const onSort = useCallback(
    (key: SortKey) => {
      if (key === sortKey) {
        setSortAsc((value) => !value);
      } else {
        setSortKey(key);
        setSortAsc(key === "name" || key === "type");
      }
    },
    [sortKey],
  );

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

  const select = useCallback((entry: NodeEntry, additive: boolean) => {
    setSelected((previous) => {
      const next = additive ? new Set(previous) : new Set<string>();
      if (additive && previous.has(entry.path)) next.delete(entry.path);
      else next.add(entry.path);
      return next;
    });
  }, []);

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

  useEffect(() => {
    const nameOf = (target: string) =>
      target.split(/[\\/]/).filter(Boolean).pop() ?? target;
    const handleDelete = (event: Event) => {
      const detail = (event as CustomEvent<{ paths?: string[]; permanent?: boolean }>).detail;
      if (!detail?.paths?.length) return;
      setConfirm({
        permanent: Boolean(detail.permanent),
        paths: detail.paths,
        names: detail.paths.map(nameOf),
      });
    };
    const handleAi = (event: Event) => {
      const detail = (
        event as CustomEvent<{ path?: string; name?: string; size?: number; isDir?: boolean }>
      ).detail;
      if (!detail?.path) return;
      const name = detail.name ?? nameOf(detail.path);
      const intro = t("ai.prompt.intro", { lang: language === "zh" ? "简体中文" : "English" });
      const prompt = t("ai.prompt.dir", {
        intro,
        path: detail.path,
        kind: detail.isDir ? t("ai.prompt.kind_dir") : t("ai.prompt.kind_file"),
        size: formatSize(detail.size ?? 0),
        files: formatCount(0),
        folders: formatCount(0),
        limit: 0,
        children: "",
      });
      setAiSession((previous) => ({
        open: true,
        key: previous.key + 1,
        subject: name,
        prompt,
      }));
    };
    const handleMove = (event: Event) => {
      const detail = (event as CustomEvent<{ paths?: string[] }>).detail;
      if (detail?.paths?.length) setMoveTarget(detail.paths);
    };
    const handleLocate = (event: Event) => {
      const detail = (event as CustomEvent<{ path?: string }>).detail;
      const target = detail?.path;
      if (!target) return;
      const parent = pathSegments(target).slice(0, -1).pop();
      if (!parent) return;
      // Locate may target any child, so load the full listing after the
      // first page before selecting it.
      void (async () => {
        await navigate(parent.path);
        const requestId = navigationId.current;
        try {
          const view = await getDirectory(parent.path);
          if (requestId !== navigationId.current) return;
          setEntries(view.children);
          setEntriesTotal(view.total);
        } catch {
          /* keep the first page */
        }
        setSelected(new Set([target]));
      })();
    };
    const handleProperties = (event: Event) => {
      const detail = (
        event as CustomEvent<{
          path?: string;
          name?: string;
          size?: number;
          isDir?: boolean;
          hidden?: boolean;
        }>
      ).detail;
      if (!detail?.path) return;
      setProperties({
        path: detail.path,
        name: detail.name ?? nameOf(detail.path),
        kind: detail.isDir ? "directory" : "file",
        size: detail.size ?? 0,
        fileCount: 0,
        dirCount: 0,
        isHidden: Boolean(detail.hidden),
        skipped: false,
        error: null,
      });
    };
    window.addEventListener("ncduwin:delete-paths", handleDelete);
    window.addEventListener("ncduwin:ai-analyze-path", handleAi);
    window.addEventListener("ncduwin:move-paths", handleMove);
    window.addEventListener("ncduwin:locate", handleLocate);
    window.addEventListener("ncduwin:properties-path", handleProperties);
    return () => {
      window.removeEventListener("ncduwin:delete-paths", handleDelete);
      window.removeEventListener("ncduwin:ai-analyze-path", handleAi);
      window.removeEventListener("ncduwin:move-paths", handleMove);
      window.removeEventListener("ncduwin:locate", handleLocate);
      window.removeEventListener("ncduwin:properties-path", handleProperties);
    };
  }, [language, navigate, t]);

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void indexStatus()
      .then((value) => {
        if (!disposed) setIndexInfo(value);
      })
      .catch(() => undefined);
    void subscribeIndex({
      onStatus: (value) => {
        if (!disposed) setIndexInfo(value);
      },
      onProgress: () => undefined,
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, []);

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

  const openContextMenu = useCallback(
    (entry: NodeEntry, event: MouseEvent) => {
      if (!selected.has(entry.path)) setSelected(new Set([entry.path]));
      setMenu({ x: event.clientX, y: event.clientY, entry });
    },
    [selected],
  );

  const treeContextMenu = useCallback(
    (entry: NodeEntry, event: MouseEvent) => {
      if (!selected.has(entry.path)) setSelected(new Set([entry.path]));
      setTreeMenu({ x: event.clientX, y: event.clientY, entry });
    },
    [selected],
  );

  const showFilesAll = useCallback(() => {
    update({ scan: { ...settings!.scan, showFiles: true } });
  }, [settings, update]);

  const release = useCallback(async () => {
    try {
      await releaseScan();
      hasTreeRef.current = false;
      setCurrent(null);
      setEntries([]);
      setEntriesTotal(0);
      setTreemap(null);
      setSummary(null);
      setResults(null);
      setQuery("");
      setSelected(new Set());
      setScanSeq((value) => value + 1);
      setNotice(t("status.released"));
    } catch (err) {
      setError(String(err));
    }
  }, [t]);

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
        } else if (mod && !event.shiftKey && key === "f") {
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
      } else if ((event.ctrlKey || event.metaKey) && !event.shiftKey && event.key.toLowerCase() === "f") {
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

  const diskText = disk
    ? t("status.disk_free", {
        free: formatSize(disk.free),
        total: formatSize(disk.total),
        pct: disk.total > 0 ? Math.round((disk.free / disk.total) * 100) : 0,
      })
    : "";

  const indexBadge = (() => {
    if (!indexInfo) return t("index.badge_idle");
    if (indexInfo.status === "building") return t("index.badge_building");
    if (indexInfo.status === "syncing") return t("index.badge_syncing");
    if (indexInfo.status === "loading") return t("index.badge_loading");
    if (indexInfo.status === "stored") return t("index.badge_stored");
    if (indexInfo.status === "ready") {
      return t("index.badge_ready", { n: formatCount(indexInfo.count) });
    }
    if (indexInfo.status === "disabled") return t("index.badge_off");
    return t("index.badge_idle");
  })();

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
        {summary && !scanning && (
          <button className="btn" onClick={() => void release()}>
            {t("button.release")}
          </button>
        )}
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
        <button
          className="btn small global-search-button"
          title={t("search_global.shortcut_hint")}
          onClick={() =>
            window.dispatchEvent(
              new CustomEvent("ncduwin:action", { detail: "global-search" }),
            )
          }
        >
          <IconDrive size={13} />
          {t("search_global.toolbar")}
        </button>
      </div>

      <Breadcrumb path={current?.path ?? path} onNavigate={(target) => void navigate(target)} />

      <div className="scan-body">
        <div className="table-pane">
          <FileTable
            key={current?.path ?? "empty"}
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
            total={isDefaultSort && !results ? entriesTotal : undefined}
            onLoadMore={isDefaultSort && !results ? loadMore : undefined}
            resetKey={current?.path ?? ""}
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
                  onContextMenu={treeContextMenu}
                  filesHidden={filesHidden}
                  onShowFiles={showFilesAll}
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
                  mainScanning={scanning}
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

      <StatusBar
        scanning={scanning}
        cancelling={cancelling}
        error={error}
        notice={notice}
        results={results}
        query={query}
        current={current}
        summary={summary}
        diskText={diskText}
        indexBadge={indexBadge}
        onGlobalSearch={() =>
          window.dispatchEvent(new CustomEvent("ncduwin:action", { detail: "global-search" }))
        }
      />

      <Fade show={Boolean(menu)}>
        {menu && (
          <ContextMenu
            x={menu.x}
            y={menu.y}
            items={menuItems(menu.entry)}
            onClose={() => setMenu(null)}
          />
        )}
      </Fade>

      <Fade show={Boolean(treeMenu)}>
        {treeMenu && (
          <ContextMenu
            x={treeMenu.x}
            y={treeMenu.y}
            items={menuItems(treeMenu.entry)}
            onClose={() => setTreeMenu(null)}
          />
        )}
      </Fade>

      <Fade show={Boolean(properties)}>
        {properties && (
          <PropertiesDialog
            entry={properties}
            parentSize={parentSize}
            onClose={() => setProperties(null)}
          />
        )}
      </Fade>

      <PathSyncDialog open={pathSyncOpen} onClose={() => setPathSyncOpen(false)} />

      <Fade show={Boolean(moveTarget)}>
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
      </Fade>

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

      <Fade show={Boolean(confirm)}>
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
      </Fade>

      <Fade show={Boolean(fallback)}>
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
      </Fade>
      <Fade show={Boolean(lockPrompt)}>
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
      </Fade>

      <BusyOverlay open={busy} label={busyText ?? ""} />
    </div>
  );
}
