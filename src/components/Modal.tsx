import { useState, type ReactNode } from "react";
import { useI18n } from "../context/I18nContext";

const iconBase = {
  width: 15,
  height: 15,
  viewBox: "0 0 24 24",
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 1.8,
  strokeLinecap: "round" as const,
  strokeLinejoin: "round" as const,
  "aria-hidden": true,
};

function IconMaximize() {
  return (
    <svg {...iconBase}>
      <rect x="4" y="4" width="16" height="16" rx="2" />
    </svg>
  );
}

function IconRestore() {
  return (
    <svg {...iconBase}>
      <rect x="8" y="8" width="12" height="12" rx="2" />
      <path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2" />
    </svg>
  );
}

function IconClose() {
  return (
    <svg {...iconBase}>
      <path d="M6 6l12 12" />
      <path d="M18 6L6 18" />
    </svg>
  );
}

interface ModalProps {
  title: string;
  children: ReactNode;
  footer?: ReactNode;
  width?: number;
  resizable?: boolean;
  className?: string;
  onClose: () => void;
}

export default function Modal({
  title,
  children,
  footer,
  width = 480,
  resizable = false,
  className = "",
  onClose,
}: ModalProps) {
  const { t } = useI18n();
  const [maximized, setMaximized] = useState(false);

  return (
    <div className="modal-overlay" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
      <div
        className={`modal ${resizable ? "resizable" : ""} ${maximized ? "maximized" : ""} ${className}`}
        style={
          maximized
            ? { width: "calc(100vw - 32px)", height: "calc(100vh - 48px)" }
            : resizable
              ? { width, height: "min(80vh, 760px)" }
              : { width }
        }
        role="dialog"
        aria-modal="true"
        aria-label={title}
      >
        <div className="modal-header">
          <h2>{title}</h2>
          <div className="modal-header-actions">
            {resizable && (
              <button
                className="icon-btn"
                onClick={() => setMaximized((value) => !value)}
                title={t(maximized ? "modal.restore" : "modal.maximize")}
                aria-label={t(maximized ? "modal.restore" : "modal.maximize")}
              >
                {maximized ? <IconRestore /> : <IconMaximize />}
              </button>
            )}
            <button className="icon-btn" onClick={onClose} aria-label={t("button.close")} title={t("button.close")}>
              <IconClose />
            </button>
          </div>
        </div>
        <div className="modal-body">{children}</div>
        {footer && <div className="modal-footer">{footer}</div>}
      </div>
    </div>
  );
}
