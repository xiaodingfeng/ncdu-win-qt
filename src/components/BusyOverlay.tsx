import Fade from "./Fade";

interface Props {
  open: boolean;
  label: string;
  detail?: string;
  /** `null`/`undefined` renders an indeterminate state without a progress bar. */
  percent?: number | null;
}

/** Full-window busy indicator so long operations never look frozen. */
export default function BusyOverlay({ open, label, detail, percent }: Props) {
  const clamped =
    percent === null || percent === undefined ? null : Math.max(0, Math.min(100, percent));
  return (
    <Fade show={open}>
      <div className="busy-overlay" role="status" aria-live="polite">
        <div className="busy-card">
          <span className="busy-spinner" aria-hidden="true" />
          <div className="busy-text">
            <span className="busy-label">{label}</span>
            {detail && <span className="busy-detail mono">{detail}</span>}
          </div>
          {clamped !== null && (
            <div className="progress-track">
              <span style={{ width: `${clamped}%` }} />
            </div>
          )}
        </div>
      </div>
    </Fade>
  );
}
