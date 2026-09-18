#include "AiAnalysisDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>
#include <QJsonObject>

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
    connect(m_renderTimer, &QTimer::timeout, this, &AiAnalysisDialog::renderNow);
}

void AiAnalysisDialog::buildUI()
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 14, 16, 12);
    lay->setSpacing(10);

    m_title = new QLabel(I18n::tr("ai.analysis.title"));
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSize(12);
    m_title->setFont(tf);
    lay->addWidget(m_title);

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

    // Follow-up question input row.
    auto* inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);
    m_input = new QLineEdit;
    m_input->setPlaceholderText(I18n::tr("ai.analysis.input_placeholder"));
    m_input->setClearButtonEnabled(true);
    m_input->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3;"
                       "  border-radius: 6px; padding: 6px 10px; }"
                       "QLineEdit:focus { border-color: %4; }")
            .arg(QString::fromLatin1(C::SURFACE()),
                 QString::fromLatin1(C::FG()),
                 QString::fromLatin1(C::BORDER()),
                 QString::fromLatin1(C::PRIMARY())));
    connect(m_input, &QLineEdit::returnPressed, this, &AiAnalysisDialog::submitQuestion);
    inputRow->addWidget(m_input, 1);

    m_sendBtn = new QPushButton(I18n::tr("ai.analysis.send"));
    m_sendBtn->setObjectName("primary");
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    connect(m_sendBtn, &QPushButton::clicked, this, &AiAnalysisDialog::submitQuestion);
    inputRow->addWidget(m_sendBtn);
    lay->addLayout(inputRow);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_copyBtn = new QPushButton(I18n::tr("ai.analysis.copy"));
    m_copyBtn->setObjectName("ghost");
    m_copyBtn->setCursor(Qt::PointingHandCursor);
    connect(m_copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_view->toPlainText());
    });
    btnRow->addWidget(m_copyBtn);

    m_closeBtn = new QPushButton(I18n::tr("button.close"));
    m_closeBtn->setObjectName("primary");
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(m_closeBtn);
    lay->addLayout(btnRow);

    setStyleSheet(QStringLiteral("QDialog { background: %1; color: %2; }"
                                 "QLabel { color: %2; }"
                                 "QPushButton#primary { background: %3; color: #fff;"
                                 "  border: none; border-radius: 6px; padding: 6px 16px; }"
                                 "QPushButton#primary:hover { background: %4; }"
                                 "QPushButton#primary:disabled { background: %5; }"
                                 "QPushButton#ghost { background: %6; color: %2;"
                                 "  border: 1px solid %7; border-radius: 6px; padding: 6px 14px; }"
                                 "QPushButton#ghost:hover { border-color: %3; }")
        .arg(QString::fromLatin1(C::BG()),
             QString::fromLatin1(C::FG()),
             QString::fromLatin1(C::PRIMARY()),
             QString::fromLatin1(C::PRIMARY_HOVER()),
             QString::fromLatin1(C::BORDER()),
             QString::fromLatin1(C::SURFACE()),
             QString::fromLatin1(C::BORDER())));
}

// --------------------------------------------------------------------------- //
// Public API
// --------------------------------------------------------------------------- //
void AiAnalysisDialog::setSubject(const QString& subject)
{
    m_subject = subject;
    if (subject.isEmpty()) {
        setWindowTitle(I18n::tr("ai.analysis.title"));
        return;
    }
    setWindowTitle(I18n::tr("ai.analysis.title") + QStringLiteral(" — ") + subject);
}

void AiAnalysisDialog::retranslate()
{
    setSubject(m_subject);   // rebuilds the window title with the subject
    m_title->setText(I18n::tr("ai.analysis.title"));
    m_input->setPlaceholderText(I18n::tr("ai.analysis.input_placeholder"));
    m_sendBtn->setText(I18n::tr("ai.analysis.send"));
    m_copyBtn->setText(I18n::tr("ai.analysis.copy"));
    m_closeBtn->setText(I18n::tr("button.close"));
    // The transcript carries translated role labels and the "analyzing…" line,
    // so it is re-rendered from the turns still held in memory. Message bodies
    // themselves are what the user or the model wrote and stay untouched.
    renderNow();
}

void AiAnalysisDialog::setSystemPrompt(const QString& text)
{
    m_systemPrompt = text;
}

void AiAnalysisDialog::beginAnalysis(const QString& userPrompt)
{
    if (m_renderTimer)
        m_renderTimer->stop();
    m_systemPrompt.clear();
    m_history.clear();
    m_buffer.clear();
    m_hasContent = false;
    m_streaming = true;
    // First turn: treat the initial prompt as a user message so it stays in the
    // conversation context for later follow-ups.
    if (!userPrompt.isEmpty())
        m_history.append({QLatin1String("user"), userPrompt});
    renderNow();
    updateInputState();
}

void AiAnalysisDialog::beginConversation()
{
    if (m_renderTimer)
        m_renderTimer->stop();
    m_history.clear();
    m_buffer.clear();
    m_hasContent = false;
    m_streaming = false;
    renderNow();
    updateInputState();
}

void AiAnalysisDialog::appendChunk(const QString& delta)
{
    if (delta.isEmpty())
        return;
    if (!m_streaming)
        m_streaming = true;
    m_buffer += delta;
    if (!m_hasContent) {
        m_hasContent = true;
        m_view->clear();
    }
    // Debounce the Markdown re-render so rapid chunks don't redraw the doc.
    m_renderTimer->start();
}

void AiAnalysisDialog::finalizeAnswer()
{
    if (m_renderTimer)
        m_renderTimer->stop();
    if (m_streaming) {
        m_history.append({QLatin1String("assistant"), m_buffer});
        m_buffer.clear();
        m_hasContent = false;
        m_streaming = false;
    }
    renderNow();
    updateInputState();
}

void AiAnalysisDialog::setError(const QString& text)
{
    if (m_renderTimer)
        m_renderTimer->stop();
    m_streaming = false;
    m_buffer.clear();
    m_hasContent = false;
    // Record the failure as the current assistant turn so the transcript stays
    // coherent and the user can retry with a new question.
    m_history.append({QLatin1String("assistant"),
                      I18n::tr("ai.analysis.failed",
                               QMap<QString, QString>{{"error", text}})});
    renderNow();
    updateInputState();
}

// --------------------------------------------------------------------------- //
// Internals
// --------------------------------------------------------------------------- //
QJsonArray AiAnalysisDialog::messages() const
{
    QJsonArray arr;
    // Prepend the system prompt (product knowledge) so every request carries it.
    if (!m_systemPrompt.isEmpty()) {
        QJsonObject sys;
        sys.insert(QLatin1String("role"), QLatin1String("system"));
        sys.insert(QLatin1String("content"), m_systemPrompt);
        arr.append(sys);
    }
    for (const auto& turn : m_history) {
        QJsonObject m;
        m.insert(QLatin1String("role"), turn.first);
        m.insert(QLatin1String("content"), turn.second);
        arr.append(m);
    }
    return arr;
}

QString AiAnalysisDialog::renderTranscript() const
{
    const QString you = I18n::tr("ai.analysis.you");
    const QString assistant = I18n::tr("ai.analysis.assistant");
    const QString loading = I18n::tr("ai.analysis.analyzing");
    QString md;
    for (const auto& turn : m_history) {
        md += QStringLiteral("**%1**\n\n%2\n\n")
                  .arg(turn.first == QLatin1String("user") ? you : assistant,
                       turn.second.toHtmlEscaped());
    }
    if (m_streaming) {
        md += QStringLiteral("**%1**\n\n%2")
                  .arg(assistant,
                       m_hasContent ? m_buffer.toHtmlEscaped()
                                    : QStringLiteral("*%1*").arg(loading.toHtmlEscaped()));
    }
    return md;
}

void AiAnalysisDialog::renderNow()
{
    m_view->setMarkdown(renderTranscript());
    // Scroll to the bottom on the next event-loop tick, after the markdown has
    // been laid out, so the reply is never hidden behind the viewport edge.
    QTimer::singleShot(0, this, [this]() {
        QScrollBar* sb = m_view ? m_view->verticalScrollBar() : nullptr;
        if (sb)
            sb->setValue(sb->maximum());
    });
}

void AiAnalysisDialog::submitQuestion()
{
    if (m_streaming)
        return;
    const QString text = m_input->text().trimmed();
    if (text.isEmpty())
        return;
    m_input->clear();
    m_input->setFocus();
    m_history.append({QLatin1String("user"), text});
    m_buffer.clear();
    m_hasContent = false;
    m_streaming = true;
    renderNow();
    updateInputState();
    emit questionSubmitted(messages());
}

void AiAnalysisDialog::updateInputState()
{
    // The input stays editable so the user can keep typing (and the caret stays
    // in the field) even while a reply streams; only the send button is gated
    // on the busy state. Duplicate sends are guarded by submitQuestion().
    m_sendBtn->setEnabled(!m_streaming);
    if (!m_streaming)
        m_input->setFocus();
}