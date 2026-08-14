#include "AiAnalysisDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>

#include "I18n.h"
#include "Style.h"

// --------------------------------------------------------------------------- //
// Construction / UI
// --------------------------------------------------------------------------- //
AiAnalysisDialog::AiAnalysisDialog(QWidget* parent)
    : QDialog(parent)
{
    buildUI();
    setMinimumSize(760, 560);
    resize(920, 720);

    // Debounce kernel: chunk deltas are accumulated and re-rendered as
    // Markdown shortly after the last one, yielding a live typewriter effect.
    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(80);
    connect(m_renderTimer, &QTimer::timeout, this, &AiAnalysisDialog::renderMarkdown);
}

void AiAnalysisDialog::buildUI()
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 14, 16, 12);
    lay->setSpacing(10);

    auto* title = new QLabel(I18n::tr("ai.analysis.title"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSize(12);
    title->setFont(tf);
    lay->addWidget(title);

    m_view = new QTextEdit;
    m_view->setReadOnly(true);
    m_view->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_view->setStyleSheet(
        QStringLiteral("QTextEdit { background: %1; color: %2; border: 1px solid %3;"
                       "  border-radius: 6px; padding: 8px; }")
            .arg(QString::fromLatin1(C::SURFACE()),
                 QString::fromLatin1(C::FG()),
                 QString::fromLatin1(C::BORDER())));
    lay->addWidget(m_view, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_copyBtn = new QPushButton(I18n::tr("ai.analysis.copy"));
    m_copyBtn->setObjectName("ghost");
    m_copyBtn->setCursor(Qt::PointingHandCursor);
    connect(m_copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_view->toPlainText());
    });
    btnRow->addWidget(m_copyBtn);

    auto* closeBtn = new QPushButton(I18n::tr("button.close"));
    closeBtn->setObjectName("primary");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(closeBtn);
    lay->addLayout(btnRow);

    setStyleSheet(QStringLiteral("QDialog { background: %1; color: %2; }"
                                 "QLabel { color: %2; }"
                                 "QPushButton#primary { background: %3; color: #fff;"
                                 "  border: none; border-radius: 6px; padding: 6px 16px; }"
                                 "QPushButton#primary:hover { background: %4; }"
                                 "QPushButton#ghost { background: %5; color: %2;"
                                 "  border: 1px solid %6; border-radius: 6px; padding: 6px 14px; }"
                                 "QPushButton#ghost:hover { border-color: %3; }")
        .arg(QString::fromLatin1(C::BG()),
             QString::fromLatin1(C::FG()),
             QString::fromLatin1(C::PRIMARY()),
             QString::fromLatin1(C::PRIMARY_HOVER()),
             QString::fromLatin1(C::SURFACE()),
             QString::fromLatin1(C::BORDER())));
}

// --------------------------------------------------------------------------- //
// Public API
// --------------------------------------------------------------------------- //
void AiAnalysisDialog::setSubject(const QString& subject)
{
    setWindowTitle(I18n::tr("ai.analysis.title") + QStringLiteral(" — ") + subject);
}

void AiAnalysisDialog::setLoading()
{
    m_buffer.clear();
    m_hasContent = false;
    if (m_renderTimer)
        m_renderTimer->stop();
    m_view->clear();
    m_view->setHtml(QStringLiteral("<i style=\"color:%1;\">%2</i>")
                        .arg(QString::fromLatin1(C::TEXT_MUTED()),
                             I18n::tr("ai.analysis.analyzing").toHtmlEscaped()));
}

void AiAnalysisDialog::appendChunk(const QString& delta)
{
    if (delta.isEmpty())
        return;
    m_buffer += delta;
    if (!m_hasContent) {
        m_hasContent = true;
        m_view->clear();
    }
    // Debounce the Markdown re-render so rapid chunks don't redraw the doc.
    m_renderTimer->start();
}

void AiAnalysisDialog::finalize()
{
    if (m_renderTimer)
        m_renderTimer->stop();
    renderMarkdown();
}

void AiAnalysisDialog::renderMarkdown()
{
    if (!m_hasContent)
        return;
    m_view->setMarkdown(m_buffer);
    QScrollBar* sb = m_view->verticalScrollBar();
    if (sb)
        sb->setValue(sb->maximum());
}

void AiAnalysisDialog::setError(const QString& text)
{
    if (m_renderTimer)
        m_renderTimer->stop();
    m_buffer.clear();
    m_hasContent = false;
    m_view->clear();
    m_view->setHtml(QStringLiteral("<span style=\"color:%1;\">%2</span>")
                        .arg(QString::fromLatin1(C::DANGER()),
                             I18n::tr("ai.analysis.failed",
                                      QMap<QString, QString>{{"error", text}}).toHtmlEscaped()));
}