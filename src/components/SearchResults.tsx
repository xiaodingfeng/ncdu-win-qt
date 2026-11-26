import { useEffect, useRef, useState, type MouseEvent, type ReactNode } from "react";
import { useI18n } from "../context/I18nContext";
import { FileTypeIcon, IconCaret, IconFolderOpen, IconStar, type FileKind } from "./icons";
import { indexTypeKey, indexTypeVar } from "../lib/fileType";
import { formatSize } from "../lib/format";
import type { IndexHit, IndexSort } from "../types/index";

const ROW_HEIGHT = 30;
const OVERSCAN = 8;

interface Props {
  items: IndexHit[];
  query: string;
  sort: IndexSort;
  asc: boolean;
  selected: number;
  onSort: (key: IndexSort) => void;
  onSelect: (index: number) => void;
  onActivate: (hit: IndexHit) => void;
  onContextMenu?: (hit: IndexHit, event: MouseEvent) => void;
  favorites: Set<string>;
  onToggleFavorite: (hit: IndexHit) => void;
  onReveal: (hit: IndexHit) => void;
}

function escapePattern(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function highlight(text: string, query: string): ReactNode {
  const tokens = query
    .split(/\s+/)
    .map((token) => token.replace(/^!/, ""))
    .filter((token) => token && !token.includes(":") && !token.includes("|") && !/[*?"]/.test(token));
  if (!tokens.length) return text;
  const pattern = new RegExp(`(${tokens.map(escapePattern).join("|")})`, "gi");
  const parts = text.split(pattern);
  const lowered = tokens.map((token) => token.toLowerCase());
  return parts.map((part, index) =>
    lowered.includes(part.toLowerCase()) ? (
      <mark className="hl" key={index}>
        {part}
      </mark>
    ) : (
      part
    ),
  );
}

function formatDate(seconds: number): string {
  if (!seconds) return "";
  const date = new Date(seconds * 1000);
  const pad = (value: number) => String(value).padStart(2, "0");
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

export default function SearchResults({
  items,
  query,
  sort,
  asc,
  selected,
  onSort,
  onSelect,
  onActivate,
  onContextMenu,
  favorites,
  onToggleFavorite,
  onReveal,
}: Props) {
  const { t } = useI18n();
  const bodyRef = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewport, setViewport] = useState(400);

  useEffect(() => {
    const element = bodyRef.current;
    if (!element) return;
    const update = () => setViewport(element.clientHeight);
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    setScrollTop(0);
    if (bodyRef.current) bodyRef.current.scrollTop = 0;
  }, [items]);

  useEffect(() => {
    const element = bodyRef.current;
    if (!element || selected < 0) return;
    const top = selected * ROW_HEIGHT;
    const bottom = top + ROW_HEIGHT;
    if (top < element.scrollTop) element.scrollTop = top;
    else if (bottom > element.scrollTop + element.clientHeight) {
      element.scrollTop = bottom - element.clientHeight;
    }
  }, [selected]);

  const start = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - OVERSCAN);
  const end = Math.min(items.length, Math.ceil((scrollTop + viewport) / ROW_HEIGHT) + OVERSCAN);
  const visible = items.slice(start, end);

  const header = (key: IndexSort, label: string, className: string) => (
    <button
      className={`gs-th ${className} ${sort === key ? "active" : ""}`}
      onClick={() => onSort(key)}
    >
      {label}
      {sort === key && (
        <span className="sort-mark">
          <IconCaret up={asc} />
        </span>
      )}
    </button>
  );

  return (
    <div className="gs-results">
      <div className="gs-row gs-row-head">
        {header("name", t("column.name"), "gs-col-name")}
        {header("size", t("column.size"), "gs-col-size")}
        {header("mtime", t("search_global.column_mtime"), "gs-col-time")}
        <span className="gs-th gs-col-kind">{t("column.type")}</span>
        <span className="gs-th gs-col-actions" />
      </div>
      <div
        className="gs-results-body"
        ref={bodyRef}
        onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}
        role="listbox"
      >
        <div className="gs-results-space" style={{ height: items.length * ROW_HEIGHT }}>
          <div className="gs-results-window" style={{ transform: `translateY(${start * ROW_HEIGHT}px)` }}>
            {visible.map((hit, offset) => {
              const index = start + offset;
              const kind = indexTypeKey(hit) as FileKind;
              const color = `var(${indexTypeVar(hit)})`;
              return (
                <div
                  key={`${hit.path}#${index}`}
                  className={`gs-row gs-data-row ${index === selected ? "selected" : ""} ${
                    hit.hidden ? "muted" : ""
                  }`}
                  style={{ height: ROW_HEIGHT }}
                  role="option"
                  aria-selected={index === selected}
                  tabIndex={-1}
                  title={hit.path}
                  onMouseDown={(event) => {
                    event.preventDefault();
                    onSelect(index);
                  }}
                  onDoubleClick={() => onActivate(hit)}
                  onContextMenu={(event) => {
                    event.preventDefault();
                    onContextMenu?.(hit, event);
                  }}
                >
                  <div className="gs-col-name gs-name-cell">
                    <span className="gs-kind-icon" style={{ color }}>
                      <FileTypeIcon kind={kind} size={16} />
                    </span>
                    <span className="gs-name">{highlight(hit.name, query)}</span>
                    <span className="gs-path">{hit.path}</span>
                  </div>
                  <div className="gs-col-size mono">{hit.isDir ? "" : formatSize(hit.size)}</div>
                  <div className="gs-col-time mono">{formatDate(hit.mtime)}</div>
                  <div className="gs-col-kind" style={{ color }}>
                    {t(`type.${kind}`)}
                  </div>
                  <div className="gs-col-actions">
                    <button
                      className={`gs-row-act ${favorites.has(hit.path) ? "active" : ""}`}
                      title={t("search_global.action_favorite")}
                      onMouseDown={(event) => event.stopPropagation()}
                      onDoubleClick={(event) => event.stopPropagation()}
                      onClick={(event) => {
                        event.stopPropagation();
                        onToggleFavorite(hit);
                      }}
                    >
                      <IconStar size={14} />
                    </button>
                    <button
                      className="gs-row-act"
                      title={t("search_global.action_reveal")}
                      onMouseDown={(event) => event.stopPropagation()}
                      onDoubleClick={(event) => event.stopPropagation()}
                      onClick={(event) => {
                        event.stopPropagation();
                        onReveal(hit);
                      }}
                    >
                      <IconFolderOpen size={14} />
                    </button>
                  </div>
                </div>
              );
            })}
          </div>
        </div>
      </div>
    </div>
  );
}
