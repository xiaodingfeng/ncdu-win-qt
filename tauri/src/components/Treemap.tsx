import { useEffect, useMemo, useRef, useState, type MouseEvent as ReactMouseEvent } from "react";
import { useI18n } from "../context/I18nContext";
import { formatSize } from "../lib/format";
import { typeKey, typeVar } from "../lib/fileType";
import type { NodeEntry, TreemapNode } from "../types/scan";

interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

interface Layout {
  node: TreemapNode;
  rect: Rect;
}

const asEntry = (node: TreemapNode): NodeEntry => ({
  path: node.path,
  name: node.name,
  kind: node.kind,
  size: node.size,
  fileCount: 0,
  dirCount: 0,
  isHidden: false,
  skipped: false,
  error: null,
});

function worstRatio(row: number[], side: number): number {
  if (side <= 0) return Number.POSITIVE_INFINITY;
  const sum = row.reduce((acc, value) => acc + value, 0);
  if (sum <= 0) return Number.POSITIVE_INFINITY;
  const max = Math.max(...row);
  const min = Math.min(...row);
  return Math.max((side * side * max) / (sum * sum), (sum * sum) / (side * side * min));
}

/** Squarified treemap layout (Bruls, Houtman, van Wijk). */
function squarify(children: TreemapNode[], width: number, height: number): Layout[] {
  const items = children
    .map((child) => ({ node: child, value: Math.max(child.size, 1) }))
    .sort((a, b) => b.value - a.value);
  if (!items.length || width <= 0 || height <= 0) return [];

  const total = items.reduce((acc, item) => acc + item.value, 0);
  const scale = (width * height) / total;
  const values = items.map((item) => item.value * scale);

  const layouts: Layout[] = [];
  let index = 0;
  let { x, y, w, h } = { x: 0, y: 0, w: width, h: height };

  while (index < values.length && w > 0 && h > 0) {
    const side = Math.min(w, h);
    const row: number[] = [values[index]];
    index += 1;
    while (
      index < values.length &&
      worstRatio([...row, values[index]], side) <= worstRatio(row, side)
    ) {
      row.push(values[index]);
      index += 1;
    }

    const rowSum = row.reduce((acc, value) => acc + value, 0);
    if (w >= h) {
      const stripWidth = rowSum / h;
      let cursor = y;
      row.forEach((value) => {
        const itemHeight = value / stripWidth;
        layouts.push({
          node: items[layouts.length].node,
          rect: { x, y: cursor, w: stripWidth, h: itemHeight },
        });
        cursor += itemHeight;
      });
      x += stripWidth;
      w -= stripWidth;
    } else {
      const stripHeight = rowSum / w;
      let cursor = x;
      row.forEach((value) => {
        const itemWidth = value / stripHeight;
        layouts.push({
          node: items[layouts.length].node,
          rect: { x: cursor, y, w: itemWidth, h: stripHeight },
        });
        cursor += itemWidth;
      });
      y += stripHeight;
      h -= stripHeight;
    }
  }

  return layouts;
}

interface TreemapProps {
  node: TreemapNode | null;
  onActivate: (entry: NodeEntry) => void;
  onContextMenu?: (entry: NodeEntry, event: ReactMouseEvent) => void;
  hiddenTypes?: Set<string>;
  /** True when the current folder holds files but the treemap is hiding them. */
  filesHidden?: boolean;
  onShowFiles?: () => void;
}

export default function Treemap({
  node,
  onActivate,
  onContextMenu,
  hiddenTypes,
  filesHidden = false,
  onShowFiles,
}: TreemapProps) {
  const { t } = useI18n();
  const containerRef = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ width: 480, height: 360 });

  useEffect(() => {
    const element = containerRef.current;
    if (!element) return;
    const observer = new ResizeObserver((entries) => {
      const box = entries[0]?.contentRect;
      if (box) setSize({ width: box.width, height: box.height });
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const layouts = useMemo(() => {
    if (!node) return [];
    const visible = node.children.filter((child) => {
      const key = typeKey(asEntry(child));
      return !hiddenTypes || !hiddenTypes.has(key);
    });
    return squarify(visible, size.width, size.height).filter(
      ({ rect }) => rect.w >= 2 && rect.h >= 2,
    );
  }, [node, size, hiddenTypes]);

  return (
    <div className="treemap" ref={containerRef}>
      {layouts.length === 0 && !filesHidden ? (
        <div className="treemap-empty">{t("treemap.empty")}</div>
      ) : (
        <>
          {layouts.length > 0 && (
            <svg width={size.width} height={size.height}>
              {layouts.map(({ node: child, rect }) => {
                const entry = asEntry(child);
                const color = `var(${typeVar(entry)})`;
                const label = rect.w > 56 && rect.h > 24;
                const subLabel = rect.w > 70 && rect.h > 40;
                return (
                  <g
                    key={child.path}
                    className="treemap-cell"
                    onClick={() => onActivate(entry)}
                    onContextMenu={(event) => {
                      if (!onContextMenu) return;
                      event.preventDefault();
                      onContextMenu(entry, event);
                    }}
                  >
                    <title>{`${child.name}\n${formatSize(child.size)}`}</title>
                    <rect
                      x={rect.x + 0.5}
                      y={rect.y + 0.5}
                      width={Math.max(0, rect.w - 1)}
                      height={Math.max(0, rect.h - 1)}
                      rx={3}
                      fill={color}
                      fillOpacity={0.88}
                    />
                    {label && (
                      <text x={rect.x + 6} y={rect.y + 16} className="treemap-label">
                        {child.name.length > Math.floor(rect.w / 7)
                          ? `${child.name.slice(0, Math.max(1, Math.floor(rect.w / 7) - 1))}…`
                          : child.name}
                      </text>
                    )}
                    {subLabel && (
                      <text x={rect.x + 6} y={rect.y + 30} className="treemap-size">
                        {formatSize(child.size)}
                      </text>
                    )}
                  </g>
                );
              })}
            </svg>
          )}
          {layouts.length === 0 && filesHidden && (
            <div className="treemap-hint">
              <span>{t("treemap.files_hidden_hint")}</span>
              {onShowFiles && (
                <button className="btn small" onClick={onShowFiles}>
                  {t("menu.view.show_files")}
                </button>
              )}
            </div>
          )}
        </>
      )}
    </div>
  );
}
