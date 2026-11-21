import { useI18n } from "../context/I18nContext";
import Modal from "./Modal";
import type { LockingProcess } from "../types/locker";

interface Props {
  title: string;
  intro: string;
  procs: LockingProcess[];
  busy?: boolean;
  allowSkip?: boolean;
  closeText?: string;
  skipText?: string;
  onCancel: () => void;
  onCloseAndContinue?: () => void;
  onSkip?: () => void;
}

export default function LockerDialog({
  title,
  intro,
  procs,
  busy = false,
  allowSkip = false,
  closeText,
  skipText,
  onCancel,
  onCloseAndContinue,
  onSkip,
}: Props) {
  const { t } = useI18n();
  const closable = procs.filter((process) => process.safeToClose).length;

  return (
    <Modal
      title={title}
      width={620}
      onClose={() => !busy && onCancel()}
      footer={
        <>
          <button className="btn" disabled={busy} onClick={onCancel}>
            {t("button.cancel")}
          </button>
          {allowSkip && onSkip && (
            <button className="btn" disabled={busy} onClick={onSkip}>
              {skipText ?? t("button.skip")}
            </button>
          )}
          {closable > 0 && onCloseAndContinue && (
            <button className="btn primary" disabled={busy} onClick={onCloseAndContinue}>
              {closeText ?? t("op_guard.btn_close")}
            </button>
          )}
        </>
      }
    >
      <p className="dialog-text pre">{intro}</p>
      <div className="locker-table">
        <div className="locker-row head">
          <span>{t("proc_close.col_proc")}</span>
          <span>{t("proc_close.col_pid")}</span>
          <span>{t("proc_close.col_action")}</span>
        </div>
        {procs.map((process) => (
          <div className="locker-row" key={process.pid}>
            <span className="locker-name" title={process.exePath || process.name}>
              {process.name}
            </span>
            <span className="mono">{process.pid}</span>
            <span className={process.safeToClose ? "locker-safe" : "locker-blocked"}>
              {process.safeToClose ? t("proc_close.can_close") : t(process.blockKey)}
            </span>
          </div>
        ))}
      </div>
      <p className={closable > 0 ? "locker-warn" : "locker-danger"}>
        {closable > 0 ? t("proc_close.warn_lost") : t("proc_close.warn_none")}
      </p>
    </Modal>
  );
}
