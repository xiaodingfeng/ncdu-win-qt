import { pathSegments } from "../lib/format";
import { useI18n } from "../context/I18nContext";
import { IconArrowUp, IconFolder } from "./icons";

interface BreadcrumbProps {
  path: string;
  onNavigate: (path: string) => void;
}

export default function Breadcrumb({ path, onNavigate }: BreadcrumbProps) {
  const { t } = useI18n();
  const segments = pathSegments(path);
  const parent = segments.length > 1 ? segments[segments.length - 2].path : null;

  return (
    <nav className="breadcrumb" aria-label={t("breadcrumb.path")}>
      <span className="breadcrumb-mode" title={t("breadcrumb.tree")}>
        <IconFolder />
      </span>
      <button
        className="crumb up"
        disabled={!parent}
        onClick={() => parent && onNavigate(parent)}
        title=".."
        aria-label={t("button.up")}
      >
        <IconArrowUp />
      </button>
      {segments.map((segment, index) => (
        <span key={segment.path} className="crumb-group">
          {index > 0 && <span className="crumb-sep">›</span>}
          <button
            className={`crumb ${index === segments.length - 1 ? "current" : ""}`}
            onClick={() => onNavigate(segment.path)}
          >
            {segment.label}
          </button>
        </span>
      ))}
    </nav>
  );
}
