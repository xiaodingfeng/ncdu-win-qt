import { useI18n } from "../context/I18nContext";

const ITEMS: { key: string; color: string }[] = [
  { key: "type.folder", color: "var(--type-dir)" },
  { key: "type.document", color: "var(--type-doc)" },
  { key: "type.image", color: "var(--type-image)" },
  { key: "type.video", color: "var(--type-video)" },
  { key: "type.audio", color: "var(--type-audio)" },
  { key: "type.archive", color: "var(--type-archive)" },
  { key: "type.code", color: "var(--type-code)" },
  { key: "type.executable", color: "var(--type-exec)" },
  { key: "type.data", color: "var(--type-data)" },
  { key: "type.other", color: "var(--type-other)" },
];

interface LegendBarProps {
  hiddenTypes?: Set<string>;
  onToggle?: (key: string) => void;
}

/** Clickable file-type legend; toggles visibility of the matching treemap cells. */
export default function LegendBar({ hiddenTypes, onToggle }: LegendBarProps) {
  const { t } = useI18n();
  return (
    <div className="legend-bar">
      {ITEMS.map((item) => {
        const type = item.key.slice("type.".length);
        const hidden = hiddenTypes?.has(type) ?? false;
        return (
          <button
            type="button"
            className={`legend-item ${hidden ? "off" : ""}`}
            key={item.key}
            aria-pressed={!hidden}
            onClick={() => onToggle?.(type)}
          >
            <span className="legend-swatch" style={{ background: item.color }} />
            {t(item.key)}
          </button>
        );
      })}
    </div>
  );
}
