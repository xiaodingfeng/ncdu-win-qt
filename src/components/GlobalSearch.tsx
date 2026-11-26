import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from "react";
import { useI18n } from "../context/I18nContext";
import { useSettings } from "../context/SettingsContext";
import SearchResults from "./SearchResults";
import ContextMenu, { type MenuItem } from "./ContextMenu";
import { IconClose, IconDrive, IconSearch } from "./icons";
import { formatCount, formatSize } from "../lib/format";
import {
  indexBuild,
  indexCancel,
  indexClear,
  indexSearch,
  indexStatus,
  indexVolumes,
  restartAsAdmin,
  subscribeIndex,
} from "../lib/index";
import { openPath, revealPath } from "../lib/ops";
import { saveSettings } from "../lib/settings";
import { getScanRoot } from "../lib/scan";
import type {
  IndexHit,
  IndexPrefs,
  IndexProgress,
  IndexSearchResponse,
  IndexSort,
  IndexStatus,
  VolumeInfo,
} from "../types/index";
import { DEFAULT_SETTINGS } from "../types/settings";

type ScopeMode = "all" | "dir" | "volume";

const RECENT_KEY = "ncduwin.index.recent";
const FAVORITES_KEY = "ncduwin.index.favorites";
const PAGE_LIMIT = 500;

interface FavoriteHit {
  path: string;
  name: string;
  size: number;
  mtime: number;
  isDir: boolean;
}

function loadFavorites(): FavoriteHit[] {
  try {
    const raw = localStorage.getItem(FAVORITES_KEY);
    const parsed = raw ? (JSON.parse(raw) as unknown) : [];
    if (!Array.isArray(parsed)) return [];
    return parsed.filter(
      (item): item is FavoriteHit =>
        typeof item === "object" &&
        item !== null &&
        typeof (item as FavoriteHit).path === "string" &&
        typeof (item as FavoriteHit).name === "string" &&
        typeof (item as FavoriteHit).isDir === "boolean",
    );
  } catch {
    return [];
  }
}

function loadRecent(): string[] {
  try {
    const raw = localStorage.getItem(RECENT_KEY);
    const parsed = raw ? (JSON.parse(raw) as unknown) : [];
    return Array.isArray(parsed) ? parsed.filter((item): item is string => typeof item === "string") : [];
  } catch {
    return [];
  }
}

function baseName(path: string): string {
  return path.split(/[\\/]/).filter(Boolean).pop() ?? path;
}

export default function GlobalSearch() {
  const { t } = useI18n();
  const { settings, update } = useSettings();
  const [open, setOpen] = useState(false);
  const [status, setStatus] = useState<IndexStatus | null>(null);
  const [progress, setProgress] = useState<IndexProgress | null>(null);
  const [query, setQuery] = useState("");
  const [scopeMode, setScopeMode] = useState<ScopeMode>("all");
  const [scopeVolume, setScopeVolume] = useState("");
  const [scanRoot, setScanRoot] = useState<string | null>(null);
  const [volumes, setVolumes] = useState<VolumeInfo[]>([]);
  const [caseSensitive, setCaseSensitive] = useState(false);
  const [sort, setSort] = useState<IndexSort>("size");
  const [asc, setAsc] = useState(false);
  const [response, setResponse] = useState<IndexSearchResponse | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [selected, setSelected] = useState(0);
  const [rowMenu, setRowMenu] = useState<{ x: number; y: number; hit: IndexHit } | null>(null);
  const [showSettings, setShowSettings] = useState(false);
  const [recent, setRecent] = useState<string[]>(loadRecent);
  const [favorites, setFavorites] = useState<FavoriteHit[]>(loadFavorites);
  const [form, setForm] = useState<IndexPrefs>(settings?.index ?? DEFAULT_SETTINGS.index);
  const inputRef = useRef<HTMLInputElement>(null);
  const requestRef = useRef(0);

  const openSearch = useCallback(() => {
    setOpen(true);
    setError(null);
    setNotice(null);
    setShowSettings(false);
    void indexStatus()
      .then(setStatus)
      .catch(() => undefined);
    void indexVolumes()
      .then(setVolumes)
      .catch(() => setVolumes([]));
    void getScanRoot()
      .then((root) => setScanRoot(root?.path ?? null))
      .catch(() => setScanRoot(null));
  }, []);

  const close = useCallback(() => setOpen(false), []);

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeIndex({
      onStatus: (next) => {
        if (disposed) return;
        setStatus(next);
        if (next.status !== "building") setProgress(null);
      },
      onProgress: (next) => {
        if (!disposed) setProgress(next);
      },
    }).then((stop) => {
      if (disposed) stop();
      else unlisten = stop;
    });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, []);

  useEffect(() => {
    const handler = (event: globalThis.KeyboardEvent) => {
      const mod = event.ctrlKey || event.metaKey;
      if (!mod) return;
      const key = event.key.toLowerCase();
      if ((key === "f" && event.shiftKey) || (key === "k" && !event.shiftKey)) {
        event.preventDefault();
        event.stopPropagation();
        openSearch();
      }
    };
    window.addEventListener("keydown", handler, true);
    return () => window.removeEventListener("keydown", handler, true);
  }, [openSearch]);

  useEffect(() => {
    const handler = (event: Event) => {
      if ((event as CustomEvent<string>).detail === "global-search") openSearch();
    };
    window.addEventListener("ncduwin:action", handler);
    return () => window.removeEventListener("ncduwin:action", handler);
  }, [openSearch]);

  useEffect(() => {
    if (!open) return;
    setForm(settings?.index ?? DEFAULT_SETTINGS.index);
    window.setTimeout(() => inputRef.current?.focus(), 10);
  }, [open, settings?.index]);

  useEffect(() => {
    if (!open) return;
    const handler = (event: globalThis.KeyboardEvent) => {
      if (event.key === "Escape") close();
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [open, close]);

  const scope = useMemo(() => {
    if (scopeMode === "all") return null;
    if (scopeMode === "volume") return scopeVolume ? `${scopeVolume}:\\` : null;
    return scanRoot;
  }, [scopeMode, scopeVolume, scanRoot]);

  // Pick a sensible default volume chip once the status arrives.
  useEffect(() => {
    if (scopeVolume || !status?.volumes.length) return;
    const first = status.volumes.find((item) => item.status === "ready") ?? status.volumes[0];
    setScopeVolume(first.volume);
  }, [status, scopeVolume]);

  // A rebuild keeps the previous snapshot queryable, so an index that is
  // still building with existing entries stays searchable.
  const searchable = Boolean(
    status && (status.status === "ready" || (status.status === "building" && status.count > 0)),
  );

  useEffect(() => {
    if (!open) return;
    const trimmed = query.trim();
    if (!trimmed || !searchable) {
      setResponse(null);
      setBusy(false);
      if (status && !searchable) setError(null);
      return;
    }
    if (scopeMode === "dir" && !scope) {
      setResponse(null);
      return;
    }
    const requestId = ++requestRef.current;
    const timer = window.setTimeout(() => {
      setBusy(true);
      void indexSearch({
        query: trimmed,
        scope,
        sort,
        asc,
        offset: 0,
        limit: PAGE_LIMIT,
        caseSensitive,
      })
        .then((result) => {
          if (requestId !== requestRef.current) return;
          if (result.error) {
            // Keep the previous valid result visible while reporting the parse
            // error of the newest query.
            setError(result.error);
            return;
          }
          setResponse(result);
          setSelected(0);
          setError(null);
        })
        .catch((err) => {
          if (requestId !== requestRef.current) return;
          setError(String(err));
        })
        .finally(() => {
          if (requestId === requestRef.current) setBusy(false);
        });
    }, 40);
    return () => window.clearTimeout(timer);
  }, [open, query, searchable, status, scope, scopeMode, sort, asc, caseSensitive]);

  const hits = response?.items ?? [];
  const active = hits[selected] ?? null;

  const remember = (value: string) => {
    const trimmed = value.trim();
    if (!trimmed) return;
    setRecent((previous) => {
      const next = [trimmed, ...previous.filter((item) => item !== trimmed)].slice(0, 8);
      try {
        localStorage.setItem(RECENT_KEY, JSON.stringify(next));
      } catch {
        // Recent searches are a convenience; storage failures are harmless.
      }
      return next;
    });
  };

  const persistFavorites = (next: FavoriteHit[]) => {
    setFavorites(next);
    try {
      localStorage.setItem(FAVORITES_KEY, JSON.stringify(next));
    } catch {
      // Favourites are a convenience; storage failures are harmless.
    }
  };

  const toggleFavorite = (hit: IndexHit) => {
    const exists = favorites.some((item) => item.path === hit.path);
    if (exists) {
      persistFavorites(favorites.filter((item) => item.path !== hit.path));
      setNotice(t("search_global.favorite_removed"));
      return;
    }
    persistFavorites(
      [
        {
          path: hit.path,
          name: hit.name,
          size: hit.size,
          mtime: hit.mtime,
          isDir: hit.isDir,
        },
        ...favorites,
      ].slice(0, 200),
    );
    setNotice(t("search_global.favorite_added"));
  };

  const removeFavorite = (path: string) => {
    persistFavorites(favorites.filter((item) => item.path !== path));
  };

  const copyPath = async (hit: IndexHit) => {
    try {
      await navigator.clipboard.writeText(hit.path);
      setNotice(t("status.copied", { path: hit.path }));
    } catch (err) {
      setError(String(err));
    }
  };

  const dispatch = (name: string, detail: unknown) => {
    window.dispatchEvent(new CustomEvent(name, { detail }));
  };

  const requestDelete = (hit: IndexHit, permanent: boolean) => {
    dispatch("ncduwin:delete-paths", { paths: [hit.path], permanent });
  };

  const analyze = (hit: IndexHit) => {
    dispatch("ncduwin:ai-analyze-path", {
      path: hit.path,
      name: hit.name,
      size: hit.size,
      isDir: hit.isDir,
    });
  };

  const moveTo = (hit: IndexHit) => {
    dispatch("ncduwin:move-paths", { paths: [hit.path] });
  };

  const showProperties = (hit: IndexHit) => {
    dispatch("ncduwin:properties-path", {
      path: hit.path,
      name: hit.name,
      size: hit.size,
      isDir: hit.isDir,
      hidden: hit.hidden,
    });
  };

  const locate = (hit: IndexHit) => {
    close();
    dispatch("ncduwin:locate", { path: hit.path });
  };

  const rowMenuItems = (hit: IndexHit): MenuItem[] => [
    {
      label: t("search_global.action_open"),
      onClick: () => void openPath(hit.path).catch((err) => setError(String(err))),
    },
    {
      label: t("search_global.action_reveal"),
      onClick: () => void revealPath(hit.path).catch((err) => setError(String(err))),
    },
    { label: t("search_global.action_copy"), onClick: () => void copyPath(hit) },
    { label: "", onClick: () => undefined, separator: true },
    { label: t("search_global.action_locate"), onClick: () => locate(hit) },
    { label: t("search_global.action_ai"), onClick: () => analyze(hit) },
    { label: t("search_global.action_move"), onClick: () => moveTo(hit) },
    { label: t("search_global.action_properties"), onClick: () => showProperties(hit) },
    { label: "", onClick: () => undefined, separator: true },
    {
      label: t("search_global.action_delete"),
      danger: true,
      onClick: () => requestDelete(hit, false),
    },
  ];

  const changeSort = (key: IndexSort) => {
    if (key === sort) setAsc((value) => !value);
    else {
      setSort(key);
      setAsc(key === "name" || key === "path");
    }
  };

  const onKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    const count = hits.length;
    if (event.key === "Escape") {
      event.preventDefault();
      close();
    } else if (event.key === "ArrowDown") {
      event.preventDefault();
      setSelected((value) => (count ? Math.min(count - 1, value + 1) : 0));
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      setSelected((value) => Math.max(0, value - 1));
    } else if (event.key === "PageDown") {
      event.preventDefault();
      setSelected((value) => (count ? Math.min(count - 1, value + 8) : 0));
    } else if (event.key === "PageUp") {
      event.preventDefault();
      setSelected((value) => Math.max(0, value - 8));
    } else if (event.key === "Enter" && active) {
      event.preventDefault();
      remember(query);
      if (event.ctrlKey || event.metaKey) void revealPath(active.path).catch((err) => setError(String(err)));
      else if (event.altKey) locate(active);
      else void openPath(active.path).catch((err) => setError(String(err)));
    } else if (event.key === "Delete" && active) {
      event.preventDefault();
      requestDelete(active, event.shiftKey);
    } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "c" && active) {
      event.preventDefault();
      void copyPath(active);
    }
  };

  const buildTargets = () => (form.volumes.length ? form.volumes : volumes.map((info) => info.letter));

  const runBuild = async () => {
    try {
      setError(null);
      setNotice(null);
      await indexBuild(buildTargets());
    } catch (err) {
      setError(String(err));
    }
  };

  const saveAndRebuild = async () => {
    try {
      setError(null);
      if (settings) {
        const next = { ...settings, index: form };
        update({ index: form });
        await saveSettings(next);
      }
      setShowSettings(false);
      await indexBuild(buildTargets());
    } catch (err) {
      setError(String(err));
    }
  };

  const clearIndex = async () => {
    try {
      setError(null);
      await indexClear();
      setResponse(null);
      setStatus(await indexStatus());
    } catch (err) {
      setError(String(err));
    }
  };

  const favoritePaths = useMemo(
    () => new Set(favorites.map((item) => item.path)),
    [favorites],
  );

  // An empty selection means "all local volumes"; the picker always shows at
  // least one checked volume so a build never targets nothing.
  const selectedVolumes = form.volumes.length
    ? form.volumes
    : volumes.map((info) => info.letter);

  const toggleVolume = (letter: string) => {
    const next = new Set(selectedVolumes);
    if (next.has(letter)) next.delete(letter);
    else next.add(letter);
    if (next.size === 0) return;
    setForm({ ...form, volumes: [...next].sort() });
  };

  if (!open) return null;

  const building = status?.status === "building";
  const statusText = (() => {
    if (!status) return t("search_global.status_loading");
    if (status.status === "building") {
      return t("search_global.status_building", {
        volume: progress?.volume ? `${progress.volume}:` : status.volume,
        n: formatCount(progress?.count ?? 0),
        phase: progress ? t(`search_global.phase_${progress.phase}`) : "",
      });
    }
    if (status.status === "ready") {
      const ready = status.volumes
        .filter((item) => item.status === "ready")
        .map((item) => `${item.volume}:`)
        .join(" ");
      return (
        t("search_global.status_ready", {
          volume: ready || status.volume,
          n: formatCount(status.count),
          memory: formatSize(status.memoryBytes),
        }) + (status.engine === "walk" ? t("search_global.engine_walk") : "")
      );
    }
    if (status.status === "error") return t("search_global.status_error", { msg: status.message });
    if (status.status === "disabled") return t("search_global.status_disabled");
    return t("search_global.status_idle", { volume: status.volume });
  })();

  return (
    <div className="gs-overlay" onMouseDown={(event) => event.target === event.currentTarget && close()}>
      <div className="gs-panel" role="dialog" aria-modal="true" aria-label={t("search_global.title")}>
        <div className="gs-head">
          <span className="gs-mark">
            <IconSearch size={18} />
          </span>
          <div className="gs-input-wrap">
            <input
              ref={inputRef}
              className="gs-input"
              value={query}
              spellCheck={false}
              placeholder={t("search_global.placeholder")}
              onChange={(event) => setQuery(event.target.value)}
              onKeyDown={onKeyDown}
            />
            {query && (
              <button
                className="gs-clear"
                title={t("search_global.clear_input")}
                onClick={() => {
                  setQuery("");
                  inputRef.current?.focus();
                }}
              >
                <IconClose size={12} />
              </button>
            )}
          </div>
          {busy && <span className="gs-spinner" aria-hidden />}
          <label className="check gs-case" title={t("search_global.case_sensitive")}>
            <input
              type="checkbox"
              checked={caseSensitive}
              onChange={(event) => setCaseSensitive(event.target.checked)}
            />
            Aa
          </label>
          <button
            className={`gs-icon ${showSettings ? "active" : ""}`}
            title={t("search_global.settings")}
            onClick={() => setShowSettings((value) => !value)}
          >
            ⚙
          </button>
          <button className="gs-icon" title={t("button.close")} onClick={close}>
            <IconClose size={16} />
          </button>
        </div>

        <div className="gs-meta">
          <div className="gs-scopes">
            <button
              className={`gs-scope ${scopeMode === "all" ? "active" : ""}`}
              onClick={() => setScopeMode("all")}
            >
              {t("search_global.scope_all")}
            </button>
            {(status?.volumes ?? []).map((item) => (
              <button
                key={item.volume}
                className={`gs-scope gs-scope-volume ${
                  scopeMode === "volume" && scopeVolume === item.volume ? "active" : ""
                } ${item.status === "building" ? "busy" : ""}`}
                title={item.message || item.volume}
                onClick={() => {
                  setScopeVolume(item.volume);
                  setScopeMode("volume");
                }}
              >
                <IconDrive size={13} />
                {item.volume}:
              </button>
            ))}
            <button
              className={`gs-scope ${scopeMode === "dir" ? "active" : ""}`}
              disabled={!scanRoot}
              title={scanRoot ?? t("search_global.scope_dir_hint")}
              onClick={() => setScopeMode("dir")}
            >
              {t("search_global.scope_dir")}
            </button>
          </div>
          <span className="gs-status" title={status?.message || statusText}>
            {statusText}
          </span>
          <div className="gs-meta-actions">
            {building ? (
              <button
                className="btn small"
                onClick={() => void indexCancel().catch((err) => setError(String(err)))}
              >
                {t("search_global.cancel_build")}
              </button>
            ) : (
              <button className="btn small" onClick={() => void runBuild()} disabled={!status?.writable}>
                {status?.status === "ready"
                  ? t("search_global.rebuild")
                  : t("search_global.build")}
              </button>
            )}
            {status && !status.writable && (
              <span className="gs-status gs-readonly">{t("search_global.read_only")}</span>
            )}
            {status && !status.canBuild && status.engine !== "walk" && (
              <button
                className="btn small"
                title={t("search_global.admin_hint")}
                onClick={() => void restartAsAdmin().catch((err) => setError(String(err)))}
              >
                {t("search_global.restart_admin")}
              </button>
            )}
          </div>
        </div>

        {error && <div className="gs-error-bar">{error}</div>}

        {showSettings ? (
          <div className="gs-settings">
            <div className="gs-volumes-head">
              <span className="gs-settings-title">{t("search_global.settings_volumes")}</span>
              <div className="gs-volumes-actions">
                <button
                  className="gs-link"
                  onClick={() => setForm({ ...form, volumes: volumes.map((info) => info.letter) })}
                >
                  {t("search_global.volumes_all")}
                </button>
                <button
                  className="gs-link"
                  disabled={!volumes.length}
                  onClick={() => setForm({ ...form, volumes: volumes.slice(0, 1).map((info) => info.letter) })}
                >
                  {t("search_global.volumes_system")}
                </button>
              </div>
            </div>
            <div className="gs-volumes">
              {volumes.map((info) => {
                const selected = selectedVolumes.includes(info.letter);
                return (
                  <label
                    key={info.letter}
                    className={`gs-volume-card ${selected ? "checked" : ""}`}
                  >
                    <input
                      type="checkbox"
                      className="gs-volume-check"
                      checked={selected}
                      onChange={() => toggleVolume(info.letter)}
                    />
                    <span className="gs-volume-icon">
                      <IconDrive size={22} />
                    </span>
                    <span className="gs-volume-main">
                      <span className="gs-volume-title">
                        <span className="gs-volume-letter">{info.letter}:</span>
                        {info.ntfs ? (
                          <span className="gs-badge ok">NTFS</span>
                        ) : (
                          <span className="gs-badge warn">
                            {t("search_global.volume_walk")}
                          </span>
                        )}
                        {info.removable && (
                          <span className="gs-badge">{t("search_global.volume_removable")}</span>
                        )}
                      </span>
                      <span className="gs-volume-label">
                        {info.label || t("search_global.volume_unlabeled")}
                      </span>
                      <span className="gs-volume-meta">
                        {t("search_global.volume_free", {
                          free: formatSize(info.freeBytes),
                          total: formatSize(info.totalBytes),
                        })}
                      </span>
                    </span>
                  </label>
                );
              })}
            </div>
            <p className="gs-hint">{t("search_global.volumes_hint")}</p>
            <div className="gs-settings-grid">
              <label className="check">
                <input
                  type="checkbox"
                  checked={form.enabled}
                  onChange={(event) => setForm({ ...form, enabled: event.target.checked })}
                />
                {t("search_global.settings_enabled")}
              </label>
              <label className="check">
                <input
                  type="checkbox"
                  checked={form.realtime}
                  onChange={(event) => setForm({ ...form, realtime: event.target.checked })}
                />
                {t("search_global.settings_realtime")}
              </label>
              <label className="gs-field">
                <span>{t("search_global.settings_budget")}</span>
                <input
                  className="search-input"
                  type="number"
                  min={64}
                  max={8192}
                  value={form.memoryBudgetMb}
                  onChange={(event) =>
                    setForm({ ...form, memoryBudgetMb: Number(event.target.value) || 0 })
                  }
                />
              </label>
              <label className="gs-field">
                <span>{t("search_global.settings_max_entries")}</span>
                <input
                  className="search-input"
                  type="number"
                  min={10_000}
                  step={10_000}
                  value={form.maxEntries}
                  onChange={(event) =>
                    setForm({ ...form, maxEntries: Number(event.target.value) || 0 })
                  }
                />
              </label>
            </div>
            <label className="gs-field gs-field-wide">
              <span>{t("search_global.settings_excludes")}</span>
              <textarea
                className="gs-textarea"
                value={form.excludes.join("\n")}
                onChange={(event) =>
                  setForm({
                    ...form,
                    excludes: event.target.value
                      .split("\n")
                      .map((line) => line.trim())
                      .filter(Boolean),
                  })
                }
              />
            </label>
            <div className="gs-settings-actions">
              <button className="btn" onClick={() => setShowSettings(false)}>
                {t("button.cancel")}
              </button>
              <button
                className="btn"
                onClick={() => {
                  if (settings) update({ index: form });
                  setShowSettings(false);
                }}
              >
                {t("button.ok")}
              </button>
              <button className="btn primary" onClick={() => void saveAndRebuild()} disabled={!status?.writable}>
                {t("search_global.save_and_rebuild")}
              </button>
              <span className="toolbar-spacer" />
              <button
                className="btn danger-outline"
                onClick={() => void clearIndex()}
                disabled={!status?.writable}
              >
                {t("search_global.clear")}
              </button>
            </div>
          </div>
        ) : query.trim() === "" ? (
          <div className="gs-empty gs-recent">
            <div className="gs-empty-title">{t("search_global.recent_title")}</div>
            {recent.length === 0 ? (
              <p className="dialog-text">{t("search_global.recent_empty")}</p>
            ) : (
              <div className="gs-recent-list">
                {recent.map((item) => (
                  <button
                    key={item}
                    className="gs-recent-item"
                    onClick={() => {
                      setQuery(item);
                      inputRef.current?.focus();
                    }}
                  >
                    {item}
                  </button>
                ))}
              </div>
            )}
            <div className="gs-empty-title gs-bookmark-title">{t("search_global.favorite_title")}</div>
            {favorites.length === 0 ? (
              <p className="dialog-text">{t("search_global.favorite_empty")}</p>
            ) : (
              <div className="gs-recent-list">
                {favorites.map((item) => (
                  <div key={item.path} className="gs-bookmark-item">
                    <button
                      className="gs-recent-item"
                      title={item.path}
                      onClick={() => void openPath(item.path).catch((err) => setError(String(err)))}
                    >
                      <span className="gs-fav-name">{item.name}</span>
                      <span className="gs-bookmark-scope">{item.path}</span>
                    </button>
                    <button
                      className="gs-bookmark-remove"
                      title={t("search_global.favorite_remove")}
                      onClick={() => removeFavorite(item.path)}
                    >
                      ×
                    </button>
                  </div>
                ))}
              </div>
            )}
          </div>
        ) : !searchable ? (
          <div className="gs-empty">
            {status?.status === "building"
              ? t("search_global.empty_building")
              : status?.status === "disabled"
                ? t("search_global.status_disabled")
                : t("search_global.empty_no_index")}
            {status && !status.canBuild && (
              <div className="gs-empty-hint">{t("search_global.admin_hint")}</div>
            )}
          </div>
        ) : response && response.items.length === 0 ? (
          <div className="gs-empty">{t("search_global.empty_none", { query: query.trim() })}</div>
        ) : (
          <SearchResults
            items={hits}
            query={query}
            sort={sort}
            asc={asc}
            selected={selected}
            onSort={changeSort}
            onSelect={setSelected}
            favorites={favoritePaths}
            onToggleFavorite={toggleFavorite}
            onReveal={(hit) => void revealPath(hit.path).catch((err) => setError(String(err)))}
            onActivate={(hit) => {
              remember(query);
              void openPath(hit.path).catch((err) => setError(String(err)));
            }}
            onContextMenu={(hit, event) => {
              event.preventDefault();
              const index = hits.findIndex((item) => item.path === hit.path);
              if (index >= 0) setSelected(index);
              setRowMenu({ x: event.clientX, y: event.clientY, hit });
            }}
          />
        )}

        <div className="gs-footer">
          <span className="gs-actions gs-actions-hint">{t("search_global.footer_context_hint")}</span>
          <span className="gs-foot-meta">
            {notice ?? (response
              ? t("search_global.footer_count", {
                  n: formatCount(response.total),
                  ms: response.tookMs,
                })
              : t("search_global.footer_hint"))}
            {active && <span className="gs-foot-name"> · {baseName(active.path)}</span>}
          </span>
        </div>
      </div>
      {rowMenu && (
        <ContextMenu
          x={rowMenu.x}
          y={rowMenu.y}
          items={rowMenuItems(rowMenu.hit)}
          onClose={() => setRowMenu(null)}
        />
      )}
    </div>
  );
}
