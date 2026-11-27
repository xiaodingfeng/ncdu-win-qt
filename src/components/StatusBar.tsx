import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import { subscribeScanProgress } from "../lib/scan";
import { formatCount, formatDuration, formatSize } from "../lib/format";
import type { NodeEntry, ScanProgress, ScanSummary } from "../types/scan";

interface StatusBarProps {
  scanning: boolean;
  cancelling: boolean;
  error: string | null;
  notice: string | null;
  results: NodeEntry[] | null;
  query: string;
  current: NodeEntry | null;
  summary: ScanSummary | null;
  diskText: string;
  indexBadge: string;
  onGlobalSearch: () => void;
}

export default function StatusBar({
  scanning,
  cancelling,
  error,
  notice,
  results,
  query,
  current,
  summary,
  diskText,
  indexBadge,
  onGlobalSearch,
}: StatusBarProps) {
  const { t } = useI18n();
  const [progress, setProgress] = useState<ScanProgress | null>(null);

  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    void subscribeScanProgress((payload) => {
      if (!disposed) setProgress(payload);
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
    if (!scanning) setProgress(null);
  }, [scanning]);

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
        let text = `${phase}${count}`;
        if (progress.elapsedMs != null) {
          text += ` · ${formatDuration(progress.elapsedMs, t)}`;
        }
        return text;
      }
      const target = progress?.path || current?.path || "";
      let text = cancelling ? t("status.cancelling") : t("status.scanning", { path: target });
      if (progress?.elapsedMs != null) {
        text += ` · ${formatDuration(progress.elapsedMs, t)}`;
      }
      if (progress?.filesPerSec != null) {
        text += ` · ${t("status.throughput", { n: formatCount(progress.filesPerSec) })}`;
      }
      return text;
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

  return (
    <footer className="status-bar">
      <span className="status-text" title={statusText}>
        {statusText}
      </span>
      {engineText && (
        <span className="status-engine" title={t("scan.engine_tip")}>
          {engineText}
        </span>
      )}
      <button
        className="status-index"
        title={t("search_global.title")}
        onClick={onGlobalSearch}
      >
        {indexBadge}
      </button>
      <span className="status-disk">{diskText}</span>
    </footer>
  );
}
