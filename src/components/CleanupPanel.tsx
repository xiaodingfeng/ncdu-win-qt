import { useCallback, useEffect, useMemo, useRef, useState, type MouseEvent } from "react";
import { invoke } from "@tauri-apps/api/core";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import Modal from "./Modal";
import ContextMenu, { type MenuItem } from "./ContextMenu";
import BusyOverlay from "./BusyOverlay";
import { IconCaret } from "./icons";
import { formatCount, formatSize } from "../lib/format";
import { openPath, revealPath } from "../lib/ops";
import { defaultPath } from "../lib/scan";
import {
  cancelCleanup,
  cleanItems,
  cleanItemsElevated,
  scanCleanup,
  scanDuplicates,
  subscribeCleanup,
} from "../lib/cleanup";
import type {
  CleanFinished,
  CleanItem,
  CleanItemOutcome,
  CleanupFinished,
  CleanupTarget,
  DangerLevel,
  DuplicateFile,
  DuplicateGroup,
  LargeFile,
} from "../types/cleanup";
import type { DiskInfo } from "../types/scan";

type CleanupTab = "categories" | "large" | "duplicates";

interface Props {
  root: string | null;
  /** Increments on every finished main scan so results reset only then. */
  scanSeq: number;
  disk: DiskInfo | null;
  onCleaned: () => void;
  onAiAnalyze: (subject: string, prompt: string) => void;
}

interface ConfirmState {
  items: CleanItem[];
  size: number;
  names: string[];
}

const LEVEL_ORDER: DangerLevel[] = ["S", "A", "B", "C", "D"];

function targetId(target: CleanupTarget): string {
  return `${target.key}\u0000${target.path}`;
}

function levelClass(level: DangerLevel): string {
  return `level level-${level.toLowerCase()}`;
}

function warningKey(level: DangerLevel): string {
  switch (level) {
    case "S":
    case "A":
      return "cleanup.warn_regeneratable";
    case "C":
      return "cleanup.warn_forbidden";
    default:
      return "cleanup.warn_maybe_useful";
  }
}

function extensionOf(name: string): string {
  const dot = name.lastIndexOf(".");
  if (dot <= 0) return "";
  return name.slice(dot + 1).toLowerCase();
}

/** Mirrors dupKeepPriority() in CleanupPanel.cpp: highest score is kept. */
function dupKeepPriority(file: DuplicateFile): number {
  let score = 0;
  switch (file.danger) {
    case "D":
      score += 1000;
      break;
    case "C":
      score += 500;
      break;
    case "B":
      score += 100;
      break;
    case "A":
      score += 10;
      break;
    default:
      break;
  }
  const path = file.path.toLowerCase().replace(/\\/g, "/");
  const disposable = [
    "temp", "tmp", ".cache", "cache", "__pycache__", "node_modules", "build", "dist",
    "target", "obj", "bin", "backup", "backups", ".gradle", ".idea", "vendor",
    ".next", ".nuxt", ".turbo", ".parcel-cache", "debug", "release",
  ];
  for (const segment of disposable) {
    if (path.includes(`/${segment}/`) || path.endsWith(`/${segment}`)) score -= 5;
  }
  if (path.includes("/documents/") || path.includes("/desktop/")) score += 10;
  return score;
}

type SortKey = "name" | "path" | "size" | "count";
type SortState = { key: SortKey; asc: boolean };

export default function CleanupPanel({ root, scanSeq, disk, onCleaned, onAiAnalyze }: Props) {
  const { t } = useI18n();
  const { settings, update } = useSettings();

  const [tab, setTab] = useState<CleanupTab>("categories");
  const [targets, setTargets] = useState<CleanupTarget[]>([]);
  const [largeFiles, setLargeFiles] = useState<LargeFile[]>([]);
  const [groups, setGroups] = useState<DuplicateGroup[]>([]);
  // Rendering every duplicate group at once freezes the list on large scans.
  const [dupLimit, setDupLimit] = useState(150);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [scanning, setScanning] = useState(false);
  const [dupScanning, setDupScanning] = useState(false);
  const [dupPhase, setDupPhase] = useState(0);
  const [dupPct, setDupPct] = useState(0);
  const [dupDone, setDupDone] = useState<{ groups: number; files: number } | null>(null);
  const [cleaning, setCleaning] = useState(false);
  const [cleanProgress, setCleanProgress] = useState<{ index: number; total: number; label: string } | null>(null);
  const [cleanResult, setCleanResult] = useState<CleanFinished | null>(null);
  const [failed, setFailed] = useState<string[] | null>(null);
  const [confirm, setConfirm] = useState<ConfirmState | null>(null);
  /** Remembers which button the user pressed, for the elevated retry. */
  const [lastPermanent, setLastPermanent] = useState(false);
  const [details, setDetailsState] = useState<CleanupTarget | null>(null);
  const [detailsItems, setDetailsItems] = useState<{ name: string; path: string; size: number; isDir: boolean }[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [lfFilter, setLfFilter] = useState("all");
  const [dupFilter, setDupFilter] = useState("all");
  const [lfSort, setLfSort] = useState<SortState>({ key: "size", asc: false });
  const [dupSort, setDupSort] = useState<SortState>({ key: "size", asc: false });
  const [menu, setMenu] = useState<{ x: number; y: number; items: MenuItem[] } | null>(null);

  const scanFlags = settings?.scan;
  const rootRef = useRef<string | null>(null);
  const scanWaiters = useRef<((payload: CleanupFinished | null) => void)[]>([]);
  const scanScopeRef = useRef<"all" | "categories" | "large">("all");
  const onCleanedRef = useRef(onCleaned);
  onCleanedRef.current = onCleaned;

  const waitForScan = useCallback(
    () =>
      new Promise<CleanupFinished | null>((resolve) => {
        scanWaiters.current.push(resolve);
        window.setTimeout(() => {
          const index = scanWaiters.current.indexOf(resolve);
          if (index >= 0) {
            scanWaiters.current.splice(index, 1);
            resolve(null);
          }
        }, 600000);
      }),
    [],
  );

  const resolveScanWaiters = (payload: CleanupFinished | null) => {
    const waiters = scanWaiters.current.splice(0);
    for (const resolve of waiters) resolve(payload);
  };

  const runCategoryScan = useCallback(
    async (targetRoot: string, categories: boolean, large: boolean) => {
      scanScopeRef.current =
        categories && large ? "all" : categories ? "categories" : "large";
      setScanning(true);
      setError(null);
      setNotice(null);
      const finished = waitForScan();
      try {
        await scanCleanup(targetRoot, categories, large);
        const payload = await finished;
        return payload !== null;
      } catch (err) {
        setError(String(err));
        return false;
      } finally {
        setScanning(false);
      }
    },
    [waitForScan],
  );

  const runDuplicateScan = useCallback(async () => {
    setDupScanning(true);
    setDupDone(null);
    setDupPhase(1);
    setDupPct(0);
    setGroups([]);
    try {
      await scanDuplicates();
    } catch (err) {
      setDupScanning(false);
      setError(String(err));
    }
  }, []);

  const resetLists = () => {
    setTargets([]);
    setLargeFiles([]);
    setGroups([]);
    setSelected(new Set());
    setDupDone(null);
  };

  /** Auto scan after the main folder scan, honouring each module's checkbox. */
  const runAuto = useCallback(async () => {
    const effective = root;
    if (!effective) return;
    const categories = scanFlags?.cleanupCategories ?? true;
    const large = scanFlags?.cleanupLarge ?? true;
    const duplicates = scanFlags?.cleanupDuplicates ?? true;
    resetLists();
    if (categories || large) {
      // Category/large scans share one cancellation flag with the duplicate
      // scan, so duplicates must only start after the first scan has emitted
      // its finished event.
      const completed = await runCategoryScan(effective, categories, large);
      if (!completed) return;
    }
    if (duplicates) await runDuplicateScan();
  }, [
    root,
    runCategoryScan,
    runDuplicateScan,
    scanFlags?.cleanupCategories,
    scanFlags?.cleanupDuplicates,
    scanFlags?.cleanupLarge,
  ]);

  /** Rescan only the module of the currently selected tab. */
  const rescanCurrent = useCallback(async () => {
    let target = root ?? rootRef.current;
    if (!target) {
      // Manual rescan is allowed before a main scan: fall back to the user
      // profile, but only when the user explicitly clicks the button.
      target = await defaultPath();
      if (!target) return;
      rootRef.current = target;
    }
    setError(null);
    setNotice(null);
    if (tab === "categories") {
      setTargets([]);
      await runCategoryScan(target, true, false);
    } else if (tab === "large") {
      setLargeFiles([]);
      await runCategoryScan(target, false, true);
    } else {
      await runDuplicateScan();
    }
  }, [tab, root, runCategoryScan, runDuplicateScan]);

  useEffect(() => {
    if (!root) return;
    rootRef.current = root;
    void runAuto();
    // Reset and rescan only when the main folder is scanned again.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [root, scanSeq]);

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeCleanup({
      onTarget: (target) => {
        if (disposed || scanScopeRef.current === "large") return;
        setTargets((previous) => [...previous, target]);
        if (target.checked && target.enabled) {
          setSelected((previous) => new Set(previous).add(targetId(target)));
        }
      },
      onLargeFile: (file) => {
        if (disposed || scanScopeRef.current === "categories") return;
        setLargeFiles((previous) => [...previous, file]);
      },
      onFinished: (payload) => {
        if (disposed) return;
        setScanning(false);
        if (scanScopeRef.current !== "large") {
          setTargets(payload.targets);
          setSelected((previous) => {
            const next = new Set(previous);
            for (const target of payload.targets) {
              if (target.checked && target.enabled) next.add(targetId(target));
            }
            return next;
          });
        }
        if (scanScopeRef.current !== "categories") {
          setLargeFiles(payload.largeFiles);
        }
        resolveScanWaiters(payload);
      },
      onFailed: (payload) => {
        if (disposed) return;
        setScanning(false);
        setDupScanning(false);
        setCleaning(false);
        setCleanProgress(null);
        if (payload.code !== "cancelled") setError(payload.message);
        resolveScanWaiters(null);
      },
      onDupProgress: (payload) => {
        if (disposed) return;
        setDupPhase(payload.phase);
        setDupPct(payload.total > 0 ? Math.round((payload.processed / payload.total) * 100) : 0);
      },
      onDupGroup: (group) => {
        if (disposed) return;
        setGroups((previous) => [...previous, group]);
      },
      onDupFinished: (payload) => {
        if (disposed) return;
        setDupScanning(false);
        setGroups(payload.groups);
        setDupLimit(150);
        setDupDone({ groups: payload.groups.length, files: payload.totalFiles });
      },
      onCleanProgress: (payload) => {
        if (!disposed) setCleanProgress(payload);
      },
      onCleanFinished: (payload) => {
        if (disposed) return;
        setCleaning(false);
        setCleanProgress(null);
        setConfirm(null);
        setCleanResult(payload);
        if (payload.failed.length > 0) setFailed(payload.failed);
        removeCleaned(payload.items);
        onCleanedRef.current();
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
    // Subscription is set up once; callbacks only use setters or refs.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const removeCleaned = (items: CleanItemOutcome[]) => {
    const gone = new Set(items.filter((item) => !item.failed).map((item) => item.path));
    if (!gone.size) return;
    setTargets((previous) => previous.filter((target) => !(gone.has(target.path) && target.key !== "cleanup.s_tmp_files" && target.key !== "cleanup.a_pyc" && target.key !== "cleanup.b_large_archives" && target.key !== "cleanup.b_downloads" && target.key !== "cleanup.b_recycle")));
    setLargeFiles((previous) => previous.filter((file) => !gone.has(file.path)));
    setGroups((previous) =>
      previous
        .map((group) => ({ ...group, files: group.files.filter((file) => !gone.has(file.path)) }))
        .filter((group) => group.files.length >= 2),
    );
    setSelected((previous) => {
      const next = new Set(previous);
      for (const item of items) {
        if (!item.failed) next.delete(item.path);
      }
      return next;
    });
  };

  const visibleTargets = useMemo(() => {
    return [...targets].sort((a, b) => {
      const level =
        LEVEL_ORDER.indexOf(a.danger) - LEVEL_ORDER.indexOf(b.danger);
      if (level !== 0) return level;
      return b.size - a.size;
    });
  }, [targets]);

  const lfTypes = useMemo(() => {
    const set = new Set<string>();
    for (const file of largeFiles) {
      const ext = extensionOf(file.name);
      if (ext) set.add(ext);
    }
    return [...set].sort();
  }, [largeFiles]);

  const visibleLargeFiles = useMemo(() => {
    const list = [...largeFiles].sort((a, b) => b.size - a.size);
    const filtered = lfFilter === "all" ? list : list.filter((file) => extensionOf(file.name) === lfFilter);
    const order = (a: LargeFile, b: LargeFile) => {
      switch (lfSort.key) {
        case "name":
          return a.name.localeCompare(b.name, undefined, { sensitivity: "base" });
        case "path":
          return a.path.localeCompare(b.path, undefined, { sensitivity: "base" });
        default:
          return a.size - b.size;
      }
    };
    filtered.sort((a, b) => (lfSort.asc ? order(a, b) : -order(a, b)));
    return filtered;
  }, [largeFiles, lfFilter, lfSort]);

  const dupTypes = useMemo(() => {
    const set = new Set<string>();
    for (const group of groups) {
      for (const file of group.files) {
        const ext = extensionOf(file.name);
        if (ext) set.add(ext);
      }
    }
    return [...set].sort();
  }, [groups]);

  const visibleGroups = useMemo(() => {
    const base =
      dupFilter === "all"
        ? groups
        : groups
            .map((group) => ({
              ...group,
              files: group.files.filter((file) => extensionOf(file.name) === dupFilter),
            }))
            .filter((group) => group.files.length >= 2);
    const order = (a: DuplicateGroup, b: DuplicateGroup) => {
      switch (dupSort.key) {
        case "name":
          return (a.files[0]?.name ?? "").localeCompare(b.files[0]?.name ?? "", undefined, {
            sensitivity: "base",
          });
        case "path":
          return (a.files[0]?.path ?? "").localeCompare(b.files[0]?.path ?? "", undefined, {
            sensitivity: "base",
          });
        case "count":
          return a.files.length - b.files.length;
        default:
          return a.size - b.size;
      }
    };
    return [...base].sort((a, b) => (dupSort.asc ? order(a, b) : -order(a, b)));
  }, [groups, dupFilter, dupSort]);

  const toggleSort = (
    setter: (update: (previous: SortState) => SortState) => void,
    key: SortKey,
  ) => {
    setter((previous) =>
      previous.key === key
        ? { key, asc: !previous.asc }
        : { key, asc: key === "name" || key === "path" },
    );
  };

  const sortButton = (current: SortState, key: SortKey, onSort: () => void, label: string) => (
    <button
      className={`sort-btn ${current.key === key ? "active" : ""}`}
      onClick={onSort}
    >
      {label}
      {current.key === key && <span className="sort-mark"><IconCaret up={current.asc} /></span>}
    </button>
  );

  const selectedSize = useMemo(() => {
    let size = 0;
    let count = 0;
    if (tab === "categories") {
      for (const target of targets) {
        if (selected.has(targetId(target))) {
          size += target.size;
          count += Math.max(1, target.fileCount);
        }
      }
    } else if (tab === "large") {
      for (const file of largeFiles) {
        if (selected.has(file.path)) {
          size += file.size;
          count += 1;
        }
      }
    } else {
      for (const group of groups) {
        for (const file of group.files) {
          if (selected.has(file.path)) {
            size += file.size;
            count += 1;
          }
        }
      }
    }
    return { size, count };
  }, [tab, targets, largeFiles, groups, selected]);

  const toggle = (id: string) => {
    setSelected((previous) => {
      const next = new Set(previous);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };

  const currentIds = (): string[] => {
    if (tab === "categories") {
      return visibleTargets
        .filter((target) => target.enabled)
        .map((target) => targetId(target));
    }
    if (tab === "large") {
      return visibleLargeFiles.map((file) => file.path);
    }
    return visibleGroups.flatMap((group) => group.files.map((file) => file.path));
  };

  const selectAll = (on: boolean) => {
    const ids = currentIds();
    setSelected((previous) => {
      const next = new Set(previous);
      for (const id of ids) {
        if (on) next.add(id);
        else next.delete(id);
      }
      return next;
    });
  };

  const smartSelect = () => {
    setSelected((previous) => {
      const next = new Set(previous);
      for (const group of visibleGroups) {
        let keep = 0;
        let best = dupKeepPriority(group.files[0]);
        for (let i = 1; i < group.files.length; i += 1) {
          const score = dupKeepPriority(group.files[i]);
          if (score > best) {
            best = score;
            keep = i;
          }
        }
        group.files.forEach((file, index) => {
          if (index === keep) next.delete(file.path);
          else next.add(file.path);
        });
      }
      return next;
    });
  };

  const requestClean = () => {
    const items: CleanItem[] = [];
    const names: string[] = [];
    // Only clean what is visible on the active tab, so a selection carried over
    // from another tab can never be deleted unseen.
    if (tab === "categories") {
      for (const target of targets) {
        if (selected.has(targetId(target))) {
          items.push({ type: "target", key: target.key, path: target.path });
          names.push(t(target.key));
        }
      }
    } else if (tab === "large") {
      for (const file of largeFiles) {
        if (selected.has(file.path)) {
          items.push({ type: "file", key: "", path: file.path });
          names.push(file.name);
        }
      }
    } else {
      for (const group of groups) {
        for (const file of group.files) {
          if (selected.has(file.path)) {
            items.push({ type: "file", key: "", path: file.path });
            names.push(file.name);
          }
        }
      }
    }
    if (!items.length) {
      setNotice(t("cleanup.none_selected"));
      return;
    }
    setConfirm({ items, size: selectedSize.size, names });
  };

  const runClean = async (permanent: boolean) => {
    if (!confirm) return;
    setLastPermanent(permanent);
    setCleaning(true);
    setError(null);
    setCleanProgress({ index: 0, total: confirm.items.length, label: "" });
    try {
      await cleanItems(confirm.items, permanent);
    } catch (err) {
      setCleaning(false);
      setCleanProgress(null);
      setError(String(err));
    }
  };

  /** Retries permission failures with administrator rights. */
  const retryElevated = async () => {
    if (!cleanResult) return;
    const retries: CleanItem[] = cleanResult.items
      .filter((item) => item.failed && item.key === "large")
      .map((item) => ({ type: "file", key: "", path: item.path }));
    if (!retries.length) return;
    setFailed(null);
    setCleanResult(null);
    setCleaning(true);
    setCleanProgress({ index: 0, total: retries.length, label: "" });
    try {
      await cleanItemsElevated(retries, lastPermanent);
    } catch (err) {
      setCleaning(false);
      setCleanProgress(null);
      setError(String(err));
    }
  };

  const openDetails = async (target: CleanupTarget) => {
    setDetailsState(target);
    setDetailsItems([]);
    if (!target.isDir) return;
    try {
      const items = await invoke<{ name: string; path: string; size: number; isDir: boolean }[]>(
        "list_dir",
        { path: target.path, limit: 50 },
      );
      setDetailsItems(items);
    } catch {
      setDetailsItems([]);
    }
  };

  const aiPrompt = (name: string, path: string, size: number, remark: string) =>
    t("ai.prompt.cleanup", {
      name,
      path,
      size: formatSize(size),
      remark,
    });

  const openMenu = (event: MouseEvent, items: MenuItem[]) => {
    event.preventDefault();
    event.stopPropagation();
    setMenu({ x: event.clientX, y: event.clientY, items });
  };

  const setDefaultScan = (value: boolean) => {
    if (!settings || !scanFlags) return;
    const key =
      tab === "categories"
        ? "cleanupCategories"
        : tab === "large"
          ? "cleanupLarge"
          : "cleanupDuplicates";
    update({ scan: { ...scanFlags, [key]: value } as typeof scanFlags });
  };

  const defaultScan =
    tab === "categories"
      ? scanFlags?.cleanupCategories ?? true
      : tab === "large"
        ? scanFlags?.cleanupLarge ?? true
        : scanFlags?.cleanupDuplicates ?? true;

  const phaseLabel = (phase: number) => {
    if (phase === 1) return t("cleanup.dup_phase_size");
    if (phase === 2) return t("cleanup.dup_phase_partial");
    return t("cleanup.dup_phase_full");
  };

  const cleanupTotal = targets.reduce((sum, target) => sum + target.size, 0);
  const largeTotal = largeFiles.reduce((sum, file) => sum + file.size, 0);

  return (
    <div className="cleanup-panel">
      <div className="cleanup-tabs">
        {(["categories", "large", "duplicates"] as CleanupTab[]).map((id) => (
          <button
            key={id}
            className={`tab ${tab === id ? "active" : ""}`}
            onClick={() => setTab(id)}
          >
            {t(
              id === "categories"
                ? "cleanup.tab_categories"
                : id === "large"
                  ? "cleanup.tab_large_files"
                  : "cleanup.tab_duplicates",
            )}
          </button>
        ))}
      </div>

      <div className="cleanup-toolbar">
        <label className="check" title={t("cleanup.default_scan_hint")}>
          <input type="checkbox" checked={defaultScan} onChange={(event) => setDefaultScan(event.target.checked)} />
          {t("cleanup.default_scan")}
        </label>
        <div className="toolbar-spacer" />
        {tab !== "duplicates" && (
          <button className="btn small" onClick={() => selectAll(true)}>
            {t("cleanup.select_all")}
          </button>
        )}
        {tab !== "duplicates" && (
          <button className="btn small" onClick={() => selectAll(false)}>
            {t("cleanup.deselect_all")}
          </button>
        )}
        {tab === "duplicates" && (
          <>
            <button className="btn small" onClick={() => selectAll(false)}>
              {t("cleanup.deselect_all")}
            </button>
            <button className="btn small" onClick={smartSelect} title={t("cleanup.dup_smart_hint")}>
              {t("cleanup.smart_select")}
            </button>
          </>
        )}
        {(tab === "large" || tab === "duplicates") && (
          <select
            className="select small"
            value={tab === "large" ? lfFilter : dupFilter}
            onChange={(event) =>
              tab === "large"
                ? setLfFilter(event.target.value)
                : setDupFilter(event.target.value)
            }
            title={t("cleanup.filter_type")}
          >
            <option value="all">{t("cleanup.filter_all_types")}</option>
            {(tab === "large" ? lfTypes : dupTypes).map((type) => (
              <option key={type} value={type}>
                {type.toUpperCase()}
              </option>
            ))}
          </select>
        )}
        <button
          className="btn small"
          disabled={scanning || dupScanning || cleaning}
          onClick={() => void rescanCurrent()}
        >
          {t("cleanup.rescan")}
        </button>
      </div>

      <div className="cleanup-body">
        {tab === "categories" && (
          <div className="cleanup-list">
            {visibleTargets.length === 0 && (
              <div className="cleanup-empty">
                {scanning
                  ? t("disk.progress.scanning", { path: root ?? rootRef.current ?? "" })
                  : t("cleanup.empty_categories")}
              </div>
            )}
            {LEVEL_ORDER.map((level) => {
              const list = visibleTargets.filter((target) => target.danger === level);
              if (list.length === 0) return null;
              return (
                <div key={level}>
                  <div className="cleanup-group-title">
                    <span className={levelClass(level)}>{level}</span>
                    <span>{t(`cleanup.group.level_${level.toLowerCase()}`)}</span>
                    <span className="muted">{list.length}</span>
                  </div>
                  {list.map((target) => {
                    const id = targetId(target);
                    return (
                      <div
                        key={id}
                        className={`cleanup-row ${selected.has(id) ? "selected" : ""} ${target.enabled ? "" : "disabled"}`}
                        onDoubleClick={() => void openDetails(target)}
                        onContextMenu={(event) =>
                          openMenu(event, [
                            {
                              label: t("ctx.reveal"),
                              onClick: () => void revealPath(target.path),
                            },
                            {
                              label: t("cleanup.view_details"),
                              onClick: () => void openDetails(target),
                            },
                            { label: "", separator: true, onClick: () => undefined },
                            {
                              label: t("ctx.ai_analyze"),
                              onClick: () =>
                                onAiAnalyze(
                                  t(target.key),
                                  aiPrompt(
                                    t(target.key),
                                    target.path,
                                    target.size,
                                    t(warningKey(target.danger)),
                                  ),
                                ),
                            },
                          ])
                        }
                        title={target.path}
                      >
                        <input
                          type="checkbox"
                          checked={selected.has(id)}
                          disabled={!target.enabled}
                          onChange={() => toggle(id)}
                        />
                        <span className={levelClass(target.danger)}>{target.danger}</span>
                        <div className="cleanup-main">
                          <span className="cleanup-name">{t(target.key)}</span>
                          <span className="cleanup-path">{target.path}</span>
                        </div>
                        <span className="cleanup-remark" title={t(warningKey(target.danger))}>
                          {t(warningKey(target.danger))}
                        </span>
                        <span className="cleanup-count mono">{formatCount(target.fileCount)}</span>
                        <span className="cleanup-size mono">{formatSize(target.size)}</span>
                        <button className="btn tiny" onClick={() => void openDetails(target)}>
                          {t("cleanup.view_details")}
                        </button>
                      </div>
                    );
                  })}
                </div>
              );
            })}
          </div>
        )}

        {tab === "large" && (
          <div className="cleanup-list">
            <div className="cleanup-sortbar">
              {sortButton(lfSort, "name", () => toggleSort(setLfSort, "name"), t("cleanup.col_name"))}
              {sortButton(lfSort, "path", () => toggleSort(setLfSort, "path"), t("cleanup.col_path"))}
              {sortButton(lfSort, "size", () => toggleSort(setLfSort, "size"), t("cleanup.col_size"))}
            </div>
            {visibleLargeFiles.length === 0 && (
              <div className="cleanup-empty">
                {scanning
                  ? t("disk.progress.scanning", { path: root ?? rootRef.current ?? "" })
                  : t("cleanup.empty_large")}
              </div>
            )}
            {visibleLargeFiles.map((file) => (
              <div
                key={file.path}
                className={`cleanup-row ${selected.has(file.path) ? "selected" : ""}`}
                title={file.path}
                onContextMenu={(event) =>
                  openMenu(event, [
                    { label: t("ctx.reveal"), onClick: () => void revealPath(file.path) },
                    { label: t("ctx.open_file"), onClick: () => void openPath(file.path) },
                    { label: "", separator: true, onClick: () => undefined },
                    {
                      label: t("ctx.ai_analyze"),
                      onClick: () =>
                        onAiAnalyze(
                          file.name,
                          aiPrompt(file.name, file.path, file.size, t(warningKey(file.danger))),
                        ),
                    },
                  ])
                }
              >
                <input
                  type="checkbox"
                  checked={selected.has(file.path)}
                  onChange={() => toggle(file.path)}
                />
                <span className={levelClass(file.danger)}>{file.danger}</span>
                <div className="cleanup-main" onClick={() => void revealPath(file.path)}>
                  <span className="cleanup-name">{file.name}</span>
                  <span className="cleanup-path">{file.path}</span>
                </div>
                <span className="cleanup-remark" title={t(warningKey(file.danger))}>
                  {t(warningKey(file.danger))}
                </span>
                <span className="cleanup-size mono">{formatSize(file.size)}</span>
                <button className="btn tiny" onClick={() => void openPath(file.path)}>
                  {t("ctx.open_file")}
                </button>
              </div>
            ))}
          </div>
        )}

        {tab === "duplicates" && (
          <div className="cleanup-list">
            <div className="cleanup-sortbar">
              {sortButton(dupSort, "name", () => toggleSort(setDupSort, "name"), t("cleanup.col_name"))}
              {sortButton(dupSort, "path", () => toggleSort(setDupSort, "path"), t("cleanup.col_path"))}
              {sortButton(dupSort, "size", () => toggleSort(setDupSort, "size"), t("cleanup.col_size"))}
              {sortButton(dupSort, "count", () => toggleSort(setDupSort, "count"), t("cleanup.col_items"))}
            </div>
            {dupScanning && (
              <div className="dup-progress">
                <div className="dup-progress-head">
                  <span>{t("cleanup.dup_scanning")}</span>
                  <span>
                    {t("cleanup.dup_progress", {
                      phase: phaseLabel(dupPhase),
                      processed: dupPct,
                      total: 100,
                      pct: dupPct,
                    })}
                  </span>
                </div>
                <div className="progress-track">
                  <span style={{ width: `${dupPct}%` }} />
                </div>
              </div>
            )}
            {!dupScanning && dupDone && (
              <div className="dup-done">
                {dupDone.groups > 0
                  ? t("cleanup.dup_scan_done", { groups: dupDone.groups, files: dupDone.files })
                  : t("cleanup.dup_scan_empty")}
              </div>
            )}
            {visibleGroups.slice(0, dupLimit).map((group, groupIndex) => (
              <div className="dup-group" key={`${group.size}-${groupIndex}-${group.files[0]?.path ?? ""}`}>
                <div className="dup-group-head">
                  <span className="dup-group-title">
                    {t("cleanup.dup_group_label", { count: group.files.length })}
                  </span>
                  <span className="dup-group-meta">
                    {formatSize(group.size)} × {group.files.length} · {t("cleanup.col_dup_wasted")}{" "}
                    {formatSize(group.wasted)}
                  </span>
                </div>
                {group.files.map((file) => (
                  <div
                    key={file.path}
                    className={`cleanup-row nested ${selected.has(file.path) ? "selected" : ""}`}
                    title={file.path}
                    onContextMenu={(event) =>
                      openMenu(event, [
                        { label: t("ctx.reveal"), onClick: () => void revealPath(file.path) },
                        { label: t("ctx.open_file"), onClick: () => void openPath(file.path) },
                        { label: "", separator: true, onClick: () => undefined },
                        {
                          label: t("ctx.ai_analyze"),
                          onClick: () =>
                            onAiAnalyze(
                              file.name,
                              aiPrompt(file.name, file.path, file.size, t(warningKey(file.danger))),
                            ),
                        },
                      ])
                    }
                  >
                    <input
                      type="checkbox"
                      checked={selected.has(file.path)}
                      onChange={() => toggle(file.path)}
                    />
                    <span className={levelClass(file.danger)}>{file.danger}</span>
                    <div className="cleanup-main" onClick={() => void revealPath(file.path)}>
                      <span className="cleanup-name">{file.name}</span>
                      <span className="cleanup-path">{file.path}</span>
                    </div>
                    <span className="cleanup-size mono">{formatSize(file.size)}</span>
                    <button className="btn tiny" onClick={() => void openPath(file.path)}>
                      {t("ctx.open_file")}
                    </button>
                  </div>
                ))}
              </div>
            ))}
            {visibleGroups.length > dupLimit && (
              <div className="table-more">
                <button className="btn small" onClick={() => setDupLimit((value) => value + 150)}>
                  {t("table.show_more", { n: formatCount(visibleGroups.length - dupLimit) })}
                </button>
              </div>
            )}
          </div>
        )}
      </div>

      <div className="cleanup-footer">
        <div className="cleanup-summary">
          <span>{t("cleanup.selected", { size: formatSize(selectedSize.size), count: selectedSize.count })}</span>
          <span className="muted">
            {t("cleanup.total", { size: formatSize(cleanupTotal + largeTotal) })}
            {disk ? ` · ${formatSize(disk.free)} / ${formatSize(disk.total)}` : ""}
          </span>
        </div>
        {cleanProgress && (
          <span className="cleanup-progress-text">
            {t("cleanup.cleaning")} {cleanProgress.index}/{cleanProgress.total}
          </span>
        )}
        {notice && <span className="cleanup-notice">{notice}</span>}
        {error && <span className="cleanup-error">{error}</span>}
        <div className="toolbar-spacer" />
        {dupScanning && (
          <button className="btn small" onClick={() => void cancelCleanup()}>
            {t("button.cancel")}
          </button>
        )}
        <button className="btn primary" disabled={cleaning || selectedSize.count === 0} onClick={requestClean}>
          {cleaning ? t("cleanup.cleaning") : t("cleanup.clean_selected")}
        </button>
      </div>

      {cleaning && cleanProgress && cleanProgress.total > 0 && (
        <div className="cleanup-progress-bar">
          <span
            style={{
              width: `${Math.round((cleanProgress.index / cleanProgress.total) * 100)}%`,
            }}
          />
        </div>
      )}

      {menu && (
        <ContextMenu
          x={menu.x}
          y={menu.y}
          items={menu.items}
          onClose={() => setMenu(null)}
        />
      )}

      {confirm && (
        <Modal
          title={t("cleanup.clean_title")}
          width={560}
          onClose={() => !cleaning && setConfirm(null)}
          footer={
            <>
              <button className="btn" disabled={cleaning} onClick={() => setConfirm(null)}>
                {t("button.cancel")}
              </button>
              {!confirm.items.some((item) => item.key === "cleanup.b_recycle") && (
                <button className="btn" disabled={cleaning} onClick={() => void runClean(false)}>
                  {t("cleanup.clean_recycle")}
                </button>
              )}
              <button className="btn danger" disabled={cleaning} onClick={() => void runClean(true)}>
                {confirm.items.some((item) => item.key === "cleanup.b_recycle")
                  ? t("cleanup.clean_empty_recycle")
                  : t("cleanup.clean_permanent")}
              </button>
            </>
          }
        >
          <p className="dialog-text">
            {t("cleanup.clean_body", {
              size: formatSize(confirm.size),
              names: confirm.names.slice(0, 6).join("\n"),
              more:
                confirm.names.length > 6
                  ? t("dialog.recycle.more", { n: confirm.names.length - 6 })
                  : "",
            })}
          </p>
          <p className="dialog-text muted">{t("cleanup.clean_hint")}</p>
        </Modal>
      )}

      {details && (
        <Modal
          title={t("cleanup.details_title")}
          width={620}
          onClose={() => setDetailsState(null)}
          footer={
            <>
              <button className="btn" onClick={() => void revealPath(details.path)}>
                {t("ctx.reveal")}
              </button>
              <button className="btn" onClick={() => setDetailsState(null)}>
                {t("button.close")}
              </button>
            </>
          }
        >
          <div className="details-head">
            <strong>{t(details.key)}</strong>
            <span className="muted">
              {formatSize(details.size)} · {formatCount(details.fileCount)}
            </span>
          </div>
          <p className="dialog-text">{details.path}</p>
          <p className="dialog-text">{t(details.remark)}</p>
          {details.key === "cleanup.b_recycle" ? (
            <p className="dialog-text">{t("cleanup.details_recycle_msg")}</p>
          ) : (
            <>
              <p className="details-subtitle">{t("cleanup.details_contents")}</p>
              <div className="details-list">
                {detailsItems.map((item) => (
                  <div className="details-row" key={item.path} title={item.path}>
                    <span className="name">{item.name}</span>
                    <span className="mono">{formatSize(item.size)}</span>
                  </div>
                ))}
              </div>
            </>
          )}
        </Modal>
      )}

      {(failed || cleanResult) && (
        <Modal
          title={failed ? t("cleanup.failed_title") : t("cleanup.clean_title")}
          width={560}
          onClose={() => {
            setFailed(null);
            setCleanResult(null);
          }}
          footer={
            <>
              {cleanResult?.needsElevation && (
                <button className="btn" onClick={() => void retryElevated()}>
                  {t("cleanup.retry_elevated")}
                </button>
              )}
              <button
                className="btn primary"
                onClick={() => {
                  setFailed(null);
                  setCleanResult(null);
                }}
              >
                {t("button.ok")}
              </button>
            </>
          }
        >
          {failed && (
            <>
              <p className="dialog-text">{t("cleanup.failed_body", { items: failed.slice(0, 10).join("\n") })}</p>
              {failed.length > 10 && (
                <p className="dialog-text">{t("cleanup.failed_more", { n: failed.length - 10 })}</p>
              )}
              {cleanResult?.needsElevation && (
                <p className="dialog-text muted">{t("cleanup.needs_admin")}</p>
              )}
            </>
          )}
          {cleanResult && (
            <>
              <p className="dialog-text">
                {t("cleanup.clean_done", {
                  freed: formatSize(cleanResult.totalFreed),
                  deleted: cleanResult.totalDeleted,
                  skipped: cleanResult.totalSkipped,
                })}
              </p>
              {cleanResult.totalSkipped > 0 && (
                <p className="dialog-text">
                  {t("cleanup.kept_status", { n: cleanResult.totalSkipped })}
                </p>
              )}
            </>
          )}
        </Modal>
      )}

      <BusyOverlay
        open={cleaning}
        label={t("cleanup.cleaning")}
        detail={
          cleanProgress
            ? `${cleanProgress.index}/${cleanProgress.total} ${cleanProgress.label}`
            : undefined
        }
        percent={
          cleanProgress && cleanProgress.total > 0
            ? Math.round((cleanProgress.index / cleanProgress.total) * 100)
            : null
        }
      />
    </div>
  );
}
