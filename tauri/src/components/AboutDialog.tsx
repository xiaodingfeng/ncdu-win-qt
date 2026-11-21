import { useEffect, useState } from "react";
import { useI18n } from "../context/I18nContext";
import { useAppConfig } from "../context/AppConfigContext";
import Modal from "./Modal";
import { getVersion } from "../lib/misc";
import { openPath } from "../lib/ops";

interface Props {
  open: boolean;
  onClose: () => void;
}

export default function AboutDialog({ open, onClose }: Props) {
  const { t } = useI18n();
  const config = useAppConfig();
  const [version, setVersion] = useState(config.version || "2.0.0");

  useEffect(() => {
    if (!open) return;
    if (config.version) {
      setVersion(config.version);
      return;
    }
    void getVersion().then(setVersion).catch(() => undefined);
  }, [open, config.version]);

  if (!open) return null;

  return (
    <Modal
      title={t("about.title", { name: config.name })}
      width={480}
      onClose={onClose}
      footer={
        <button className="btn primary" onClick={onClose}>
          {t("button.ok")}
        </button>
      }
    >
      <div className="about">
        <div className="about-title">{config.name}</div>
        <p className="dialog-text">{t("about.body", { version, name: config.name })}</p>
        <button className="about-link" onClick={() => void openPath(config.homepage)}>
          {t("about.homepage", { url: config.homepage })}
        </button>
        <button className="about-link" onClick={() => void openPath(config.sourceUrl)}>
          {t("about.source", { url: config.sourceUrl })}
        </button>
        <p className="about-thanks">{t("about.thanks")}</p>
        {config.versionInfo && <p className="about-thanks">{config.versionInfo}</p>}
        {config.copyright && <p className="about-thanks">{config.copyright}</p>}
      </div>
    </Modal>
  );
}
