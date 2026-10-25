#include "FeedbackDialog.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSysInfo>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include "DialogI18n.h"
#include "I18n.h"
#include "Style.h"
#include "version.h"

namespace {
constexpr const char* FEEDBACK_API_KEY = "ncduwin";
constexpr int FEEDBACK_TRANSFER_TIMEOUT_MS = 15000;
}  // namespace

// --------------------------------------------------------------------------- //
// Construction / UI
// --------------------------------------------------------------------------- //
FeedbackDialog::FeedbackDialog(QWidget* parent)
    : QDialog(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    buildUI();
    applyStyle();
}

void FeedbackDialog::buildUI()
{
    setWindowTitle(I18n::tr("feedback.title"));
    setMinimumWidth(480);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 18, 20, 14);
    lay->setSpacing(12);

    auto* form = new QFormLayout;
    form->setSpacing(10);

    m_titleEdit = new QLineEdit;
    m_titleEdit->setPlaceholderText(I18n::tr("feedback.placeholder.title"));
    form->addRow(I18n::tr("feedback.field.title"), m_titleEdit);

    m_contentEdit = new QTextEdit;
    m_contentEdit->setPlaceholderText(I18n::tr("feedback.placeholder.content"));
    m_contentEdit->setFixedHeight(140);
    form->addRow(I18n::tr("feedback.field.content"), m_contentEdit);

    m_contactEdit = new QLineEdit;
    m_contactEdit->setPlaceholderText(I18n::tr("feedback.placeholder.contact"));
    form->addRow(I18n::tr("feedback.field.contact"), m_contactEdit);

    m_levelCombo = new QComboBox;
    m_levelCombo->addItem(I18n::tr("feedback.level.debug"), QStringLiteral("debug"));
    m_levelCombo->addItem(I18n::tr("feedback.level.info"), QStringLiteral("info"));
    m_levelCombo->addItem(I18n::tr("feedback.level.warn"), QStringLiteral("warn"));
    m_levelCombo->addItem(I18n::tr("feedback.level.error"), QStringLiteral("error"));
    m_levelCombo->addItem(I18n::tr("feedback.level.fatal"), QStringLiteral("fatal"));
    m_levelCombo->setCurrentIndex(1);  // default: info
    form->addRow(I18n::tr("feedback.field.level"), m_levelCombo);

    lay->addLayout(form);

    m_hintLabel = new QLabel(I18n::tr("feedback.hint"));
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                                   .arg(QString::fromLatin1(C::TEXT_MUTED())));
    lay->addWidget(m_hintLabel);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(I18n::tr("button.cancel"));
    m_cancelBtn->setObjectName("ghost");
    m_cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(m_cancelBtn);

    m_submitBtn = new QPushButton(I18n::tr("feedback.submit"));
    m_submitBtn->setObjectName("primary");
    m_submitBtn->setCursor(Qt::PointingHandCursor);
    connect(m_submitBtn, &QPushButton::clicked, this, &FeedbackDialog::onSubmit);
    btnRow->addWidget(m_submitBtn);

    lay->addLayout(btnRow);
}

void FeedbackDialog::applyStyle()
{
    setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QLabel { color: %2; }"
        "QLineEdit, QComboBox { background: %3; color: %2; border: 1px solid %4;"
        "  border-radius: 6px; padding: 5px 8px; }"
        "QLineEdit:focus, QComboBox:focus, QTextEdit:focus { border-color: %5; }"
        "QTextEdit { background: %3; color: %2; border: 1px solid %4;"
        "  border-radius: 6px; padding: 6px 8px; }"
        "QPushButton#primary { background: %5; color: #fff; border: none;"
        "  border-radius: 6px; padding: 6px 16px; }"
        "QPushButton#primary:hover { background: %6; }"
        "QPushButton#primary:disabled { background: %4; }"
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
// Submission
// --------------------------------------------------------------------------- //
void FeedbackDialog::onSubmit()
{
    const QString content = m_contentEdit->toPlainText().trimmed();
    if (content.isEmpty()) {
        Dialogs::warn(this, I18n::tr("feedback.title"), I18n::tr("feedback.empty_content"));
        return;
    }

    QJsonObject body;
    body.insert(QLatin1String("app"), QString::fromLatin1(APP_NAME));
    body.insert(QLatin1String("version"), QString::fromLatin1(APP_VERSION));
    body.insert(QLatin1String("content"), content);
    body.insert(QLatin1String("level"), m_levelCombo->currentData().toString());
    const QString title = m_titleEdit->text().trimmed();
    if (!title.isEmpty())
        body.insert(QLatin1String("title"), title);
    const QString contact = m_contactEdit->text().trimmed();
    if (!contact.isEmpty())
        body.insert(QLatin1String("contact"), contact);
    body.insert(QLatin1String("device"),
                QStringLiteral("%1 / %2").arg(QSysInfo::prettyProductName(),
                                              QSysInfo::currentCpuArchitecture()));

    QJsonObject extra;
    extra.insert(QLatin1String("os"), QStringLiteral("windows"));
    extra.insert(QLatin1String("arch"), QSysInfo::currentCpuArchitecture());
    body.insert(QLatin1String("extra"), extra);

    QNetworkRequest req(QUrl(QString::fromLatin1(FEEDBACK_URL)));
    req.setTransferTimeout(FEEDBACK_TRANSFER_TIMEOUT_MS);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("X-API-Key", FEEDBACK_API_KEY);

    m_submitBtn->setEnabled(false);
    m_submitBtn->setText(I18n::tr("feedback.sending"));

    QNetworkReply* reply = m_nam->post(req,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_submitBtn->setEnabled(true);
        m_submitBtn->setText(I18n::tr("feedback.submit"));

        if (reply->error() != QNetworkReply::NoError) {
            Dialogs::warn(this, I18n::tr("feedback.title"),
                I18n::tr("feedback.failed",
                         QMap<QString, QString>{{"error", reply->errorString()}}));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.value(QLatin1String("code")).toInt(-1) == 0) {
            Dialogs::info(this, I18n::tr("feedback.title"), I18n::tr("feedback.success_body"));
            accept();
            return;
        }

        QString error = obj.value(QLatin1String("message")).toString().trimmed();
        if (error.isEmpty())
            error = reply->errorString();
        Dialogs::warn(this, I18n::tr("feedback.title"),
            I18n::tr("feedback.failed", QMap<QString, QString>{{"error", error}}));
    });
}
