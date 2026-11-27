import { memo, useEffect, useRef, useState, type MouseEvent } from "react";
import { useI18n } from "../context/I18nContext";
import { IconCaret } from "./icons";
import { formatCount, formatSize } from "../lib/format";
import { typeKey, typeVar } from "../lib/fileType";
import type { NodeEntry } from "../types/scan";

export type SortKey = "name" | "size" | "percent" | "items" | "type";

const ROW_HEIGHT = 30;
const OVERSCAN = 8;

interface FileTableProps {
  entries: NodeEntry[];
  parentSize: number;
  sortKey: SortKey;
  sortAsc: boolean;
  showPath?: boolean;
  emptyText?: string;
  selected: Set<string>;
  onSort: (key: SortKey) => void;
  onActivate: (entry: NodeEntry) => void;
  onSelect: (entry: NodeEntry, additive: boolean) => void;
  onContextMenu?: (entry: NodeEntry, event: MouseEvent) => void;
  total?: number;
  onLoadMore?: () => void;
  resetKey?: string;
}

function FileTable({
  entries,
  parentSize,
  sortKey,
  sortAsc,
  showPath = false,
  emptyText,
  selected,
  onSort,
  onActivate,
  onSelect,
  onContextMenu,
  total,
  onLoadMore,
  resetKey,
}: FileTableProps) {
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
  }, [resetKey, sortKey, sortAsc]);

  useEffect(() => {
    if (!onLoadMore) return;
    if (scrollTop + viewport >= entries.length * ROW_HEIGHT - ROW_HEIGHT * 4) {
      onLoadMore();
    }
  }, [scrollTop, viewport, entries.length, onLoadMore]);

  const count = Math.max(total ?? entries.length, entries.length);
  const start = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - OVERSCAN);
  const end = Math.min(
    entries.length,
    Math.ceil((scrollTop + viewport) / ROW_HEIGHT) + OVERSCAN,
  );
  const visible = entries.slice(start, end);

  const header = (key: SortKey, label: string, className: string) => (
    <button
      className={`th ${className} ${sortKey === key ? "active" : ""}`}
      onClick={() => onSort(key)}
    >
      {label}
      {sortKey === key && <span className="sort-mark"><IconCaret up={sortAsc} /></span>}
    </button>
  );

  return (
    <div className="file-table" role="table">
      <div className="table-head" role="row">
        {header("name", showPath ? `${t("column.name")} / ${t("column.path")}` : t("column.name"), "col-name")}
        <span className="th col-distribution">{t("column.distribution")}</span>
        {header("size", t("column.size"), "col-size")}
        {header("percent", t("column.percent"), "col-percent")}
        {header("items", t("column.items"), "col-items")}
        {header("type", t("column.type"), "col-type")}
      </div>

      <div
        className="table-body"
        role="rowgroup"
        ref={bodyRef}
        onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}
      >
        {entries.length === 0 && (
          <div className="table-empty">{emptyText ?? t("treemap.empty")}</div>
        )}
        <div style={{ height: count * ROW_HEIGHT, position: "relative" }}>
          <div style={{ transform: `translateY(${start * ROW_HEIGHT}px)` }}>
            {visible.map((entry) => {
              const percent = parentSize > 0 ? (entry.size / parentSize) * 100 : 0;
              const color = `var(${typeVar(entry)})`;
              const isSelected = selected.has(entry.path);
              return (
                <div
                  key={entry.path}
                  className={`table-row ${isSelected ? "selected" : ""} ${entry.error ? "has-error" : ""}`}
                  role="row"
                  tabIndex={0}
                  title={entry.path}
                  onClick={(event) => onSelect(entry, event.ctrlKey || event.metaKey)}
                  onContextMenu={(event) => {
                    event.preventDefault();
                    onContextMenu?.(entry, event);
                  }}
                  onDoubleClick={() => onActivate(entry)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter") onActivate(entry);
                  }}
                >
                  <div className="cell col-name">
                    <span className="type-dot" style={{ background: color }} />
                    <span className={`name ${entry.isHidden ? "muted" : ""}`}>{entry.name}</span>
                    {entry.skipped && <span className="tag">{t("status.skipped")}</span>}
                    {entry.error && <span className="tag error">{entry.error}</span>}
                    {showPath && <span className="sub-path">{entry.path}</span>}
                  </div>
                  <div className="cell col-distribution">
                    <div className="size-bar">
                      <span style={{ width: `${Math.min(100, percent)}%`, background: color }} />
                    </div>
                  </div>
                  <div className="cell col-size mono">{formatSize(entry.size)}</div>
                  <div className="cell col-percent mono">{percent.toFixed(1)}%</div>
                  <div className="cell col-items mono">
                    {entry.kind === "directory" || entry.kind === "drive"
                      ? formatCount(entry.fileCount + entry.dirCount)
                      : ""}
                  </div>
                  <div className="cell col-type">
                    <span style={{ color }}>{t(`type.${typeKey(entry)}`)}</span>
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

export default memo(FileTable);
