#include "AiSettingsDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>

#include "AiService.h"
#include "I18n.h"
#include "Style.h"
#include "Logger.h"

// --------------------------------------------------------------------------- //
// Construction / UI
// --------------------------------------------------------------------------- //
AiSettingsDialog::AiSettingsDialog(AiService* service, QWidget* parent)
    : QDialog(parent)
    , m_service(service)
{
    buildUI();
    applyStyle();

    // Pre-fill from persisted settings.
    const AiConfig cfg = m_service->load();
    m_baseUrlEdit->setText(cfg.baseUrl);
    m_apiKeyEdit->setText(cfg.apiKey);
    m_modelCombo->addItem(QStringLiteral("auto"));
    m_modelCombo->setEditText(cfg.model.isEmpty() ? QStringLiteral("auto") : cfg.model);
    m_modelCombo->setCurrentText(cfg.model.isEmpty() ? QStringLiteral("auto") : cfg.model);

    // Async auto-fetch of the default Base URL / API Key when missing.
    if (m_service->needsDefaults()) {
        connect(m_service, &AiService::baseUrlFetched, this,
                [this](const QString& url) {
                    const QString v = url.trimmed();
                    if (!v.isEmpty() && m_baseUrlEdit->text().trimmed().isEmpty())
                        m_baseUrlEdit->setText(v);
                });
        connect(m_service, &AiService::apiKeyFetched, this,
                [this](const QString& key) {
                    if (!key.trimmed().isEmpty() && m_apiKeyEdit->text().trimmed().isEmpty())
                        m_apiKeyEdit->setText(key.trimmed());
                });
        connect(m_service, &AiService::apiKeyFetchFinished, this, [this]() {
            if (m_apiKeyEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, I18n::tr("ai.settings.title"),
                                     I18n::tr("ai.settings.key_fetch_failed"));
            }
        });
        m_service->fetchDefaults();
    }
}

void AiSettingsDialog::buildUI()
{
    setWindowTitle(I18n::tr("ai.settings.title"));
    setMinimumWidth(460);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 18, 20, 14);
    lay->setSpacing(12);

    auto* form = new QFormLayout;
    form->setSpacing(10);

    m_baseUrlEdit = new QLineEdit;
    m_baseUrlEdit->setPlaceholderText(QStringLiteral("https://…/v1"));
    form->addRow(I18n::tr("ai.settings.base_url"), m_baseUrlEdit);

    m_apiKeyEdit = new QLineEdit;
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(QStringLiteral("sk-…"));
    form->addRow(I18n::tr("ai.settings.api_key"), m_apiKeyEdit);

    m_modelCombo = new QComboBox;
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    form->addRow(I18n::tr("ai.settings.model"), m_modelCombo);

    lay->addLayout(form);

    // Hint about "auto".
    auto* hint = new QLabel(I18n::tr("ai.settings.model_hint"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                            .arg(QString::fromLatin1(C::TEXT_MUTED())));
    lay->addWidget(hint);

    // Buttons.
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_restoreBtn = new QPushButton(I18n::tr("ai.settings.restore_defaults"));
    m_restoreBtn->setObjectName("ghost");
    m_restoreBtn->setCursor(Qt::PointingHandCursor);
    connect(m_restoreBtn, &QPushButton::clicked, this, &AiSettingsDialog::onRestoreDefaults);
    btnRow->addWidget(m_restoreBtn);

    m_fetchBtn = new QPushButton(I18n::tr("ai.settings.fetch_models"));
    m_fetchBtn->setObjectName("ghost");
    m_fetchBtn->setCursor(Qt::PointingHandCursor);
    connect(m_fetchBtn, &QPushButton::clicked, this, &AiSettingsDialog::onFetchModels);
    btnRow->addWidget(m_fetchBtn);

    m_cancelBtn = new QPushButton(I18n::tr("button.cancel"));
    m_cancelBtn->setObjectName("ghost");
    m_cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(m_cancelBtn);

    m_saveBtn = new QPushButton(I18n::tr("ai.settings.save"));
    m_saveBtn->setObjectName("primary");
    m_saveBtn->setCursor(Qt::PointingHandCursor);
    connect(m_saveBtn, &QPushButton::clicked, this, &AiSettingsDialog::onSave);
    btnRow->addWidget(m_saveBtn);

    lay->addLayout(btnRow);
}

void AiSettingsDialog::applyStyle()
{
    setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QLabel { color: %2; }"
        "QLineEdit, QComboBox { background: %3; color: %2; border: 1px solid %4;"
        "  border-radius: 6px; padding: 5px 8px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: %5; }"
        "QPushButton#primary { background: %5; color: #fff; border: none;"
        "  border-radius: 6px; padding: 6px 16px; }"
        "QPushButton#primary:hover { background: %6; }"
        "QPushButton#ghost { background: %3; color: %2; border: 1px solid %4;"
        "  border-radius: 6px; padding: 6px 14px; }"
        "QPushButton#ghost:hover { border-color: %5; }")
        .arg(QString::fromLatin1(C::BG()),
             QString::fromLatin1(C::FG()),
             QString::fromLatin1(C::SURFACE()),
             QString::fromLatin1(C::BORDER()),
             QString::fromLatin1(C::PRIMARY()),
             QString::fromLatin1(C::PRIMARY_HOVER())));
}

// --------------------------------------------------------------------------- //
// Slots
// --------------------------------------------------------------------------- //
void AiSettingsDialog::onRestoreDefaults()
{
    m_restoreBtn->setEnabled(false);
    m_restoreBtn->setText(I18n::tr("ai.settings.fetching"));
    m_pendingDefaults = 2;
    // Force-overwrite both fields with the fetched defaults.
    connect(m_service, &AiService::baseUrlFetched, this, [this](const QString& url) {
        const QString v = url.trimmed();
        if (!v.isEmpty())
            m_baseUrlEdit->setText(v);
    });
    connect(m_service, &AiService::apiKeyFetched, this, [this](const QString& key) {
        const QString v = key.trimmed();
        if (!v.isEmpty())
            m_apiKeyEdit->setText(v);
    });
    connect(m_service, &AiService::baseUrlFetchFinished, this,
            &AiSettingsDialog::restoreDefaultsStep);
    connect(m_service, &AiService::apiKeyFetchFinished, this,
            &AiSettingsDialog::restoreDefaultsStep);
    m_service->fetchDefaults();
}

void AiSettingsDialog::restoreDefaultsStep()
{
    if (--m_pendingDefaults > 0)
        return;
    m_restoreBtn->setEnabled(true);
    m_restoreBtn->setText(I18n::tr("ai.settings.restore_defaults"));
    disconnect(m_service, &AiService::baseUrlFetched, this, nullptr);
    disconnect(m_service, &AiService::apiKeyFetched, this, nullptr);
    disconnect(m_service, &AiService::baseUrlFetchFinished, this, nullptr);
    disconnect(m_service, &AiService::apiKeyFetchFinished, this, nullptr);
}

void AiSettingsDialog::onFetchModels()
{
    const QString base = m_baseUrlEdit->text().trimmed();
    const QString key = m_apiKeyEdit->text().trimmed();
    if (base.isEmpty() || key.isEmpty()) {
        QMessageBox::warning(this, I18n::tr("ai.settings.title"),
                             I18n::tr("ai.settings.fetch_models_failed",
                                       QMap<QString, QString>{
                                           {"error", I18n::tr("ai.settings.fill_url_key")}}));
        return;
    }
    // Persist the current edits so AiService::fetchModels() reads them.
    AiConfig cfg;
    cfg.baseUrl = base;
    cfg.apiKey = key;
    cfg.model = m_modelCombo->currentText().trimmed();
    m_service->save(cfg);

    m_fetchBtn->setEnabled(false);
    m_fetchBtn->setText(I18n::tr("ai.settings.fetching"));
    connect(m_service, &AiService::modelsFetched, this,
            [this](const QStringList& ids) {
                m_fetchBtn->setEnabled(true);
                m_fetchBtn->setText(I18n::tr("ai.settings.fetch_models"));
                const QString current = m_modelCombo->currentText().trimmed();
                m_modelCombo->clear();
                m_modelCombo->addItem(QStringLiteral("auto"));
                m_modelCombo->addItems(ids);
                // Restore current selection if it is still present.
                int idx = m_modelCombo->findText(current);
                if (idx >= 0)
                    m_modelCombo->setCurrentIndex(idx);
                else
                    m_modelCombo->setEditText(current.isEmpty() ? QStringLiteral("auto") : current);
            });
    connect(m_service, &AiService::modelsFailed, this,
            [this](const QString& error) {
                m_fetchBtn->setEnabled(true);
                m_fetchBtn->setText(I18n::tr("ai.settings.fetch_models"));
                QMessageBox::warning(this, I18n::tr("ai.settings.title"),
                                     I18n::tr("ai.settings.fetch_models_failed",
                                               QMap<QString, QString>{{"error", error}}));
            });
    m_service->fetchModels();
}

void AiSettingsDialog::onSave()
{
    AiConfig cfg;
    cfg.baseUrl = m_baseUrlEdit->text().trimmed();
    cfg.apiKey = m_apiKeyEdit->text().trimmed();
    cfg.model = m_modelCombo->currentText().trimmed();
    if (cfg.model.isEmpty())
        cfg.model = QStringLiteral("auto");
    m_service->save(cfg);
    accept();
}