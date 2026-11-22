import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import { formatCount, formatSize } from "../lib/format";
import { typeKey } from "../lib/fileType";
import { fileTimes, type FileTimes } from "../lib/ops";
import type { NodeEntry } from "../types/scan";
import Modal from "./Modal";

interface PropertiesDialogProps {
  entry: NodeEntry;
  parentSize: number;
  onClose: () => void;
}

function formatTime(value: number | null | undefined): string {
  if (!value) return "—";
  return new Date(value).toLocaleString();
}

export default function PropertiesDialog({ entry, parentSize, onClose }: PropertiesDialogProps) {
  const { t } = useI18n();
  const [times, setTimes] = useState<FileTimes | null>(null);

  useEffect(() => {
    let disposed = false;
    void fileTimes(entry.path)
      .then((value) => {
        if (!disposed) setTimes(value);
      })
      .catch(() => {
        if (!disposed) setTimes(null);
      });
    return () => {
      disposed = true;
    };
  }, [entry.path]);

  const percent = parentSize > 0 ? (entry.size / parentSize) * 100 : 0;

  const row = (label: string, value: string) => (
    <div className="prop-row" key={label}>
      <span className="prop-label">{label}</span>
      <span className="prop-value" title={value}>
        {value}
      </span>
    </div>
  );

  return (
    <Modal
      title={t("properties.title")}
      width={520}
      onClose={onClose}
      footer={
        <button className="btn primary" onClick={onClose}>
          {t("button.close")}
        </button>
      }
    >
      <div className="prop-list">
        {row(t("properties.name"), entry.name)}
        {row(t("properties.path"), entry.path)}
        {row(t("properties.type"), t(`type.${typeKey(entry)}`))}
        {row(t("properties.size"), `${formatSize(entry.size)} (${percent.toFixed(1)}%)`)}
        {row(t("properties.percent"), t("properties.percent_of", { pct: percent.toFixed(1) }))}
        {row(t("properties.files"), formatCount(entry.fileCount))}
        {row(t("properties.folders"), formatCount(entry.dirCount))}
        {row(t("properties.created"), formatTime(times?.created))}
        {row(t("properties.modified"), formatTime(times?.modified))}
        {row(t("properties.accessed"), formatTime(times?.accessed))}
      </div>
    </Modal>
  );
}
