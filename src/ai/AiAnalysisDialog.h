#pragma once

#include <QDialog>
#include <QVector>
#include <QPair>
#include <QJsonArray>

class QTextEdit;
class QPushButton;
class QLineEdit;
class QLabel;
class QTimer;

// AiAnalysisDialog - conversational dialog for AI disk-object analysis.
//
// Displays a multi-turn Q&A transcript about a disk object. The initial
// analysis is the first turn; the user can then type follow-up questions at
// the bottom, and the whole conversation (history) is re-sent so the AI keeps
// context. While a request is pending the dialog shows a "loading" placeholder
// and incoming stream chunks are accumulated and rendered as Markdown on a
// short debounce timer (live typewriter without blocking the UI). The current
// session's message history is emitted via questionSubmitted so the caller can
// forward it to the AI backend.
class AiAnalysisDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiAnalysisDialog(QWidget* parent = nullptr);

    void setSubject(const QString& subject);
    // Re-label the window after a language change. This dialog is modeless and
    // reused across questions, so it can well be open when the user switches
    // language; without this it would stay in the old one until reopened.
    void retranslate();
    // Optional system prompt prepended to every request (e.g. product knowledge
    // for products-wise Q&A). Cleared automatically by beginAnalysis().
    void setSystemPrompt(const QString& text);
    // Start a fresh analysis conversation for a user prompt.
    void beginAnalysis(const QString& userPrompt);
    // Start an empty conversation (no initial user message), e.g. for open-ended
    // Q&A where the user types the first question.
    void beginConversation();
    // Append a streamed chunk of the current assistant answer.
    void appendChunk(const QString& delta);
    // Commit the streamed answer to the transcript and re-enable input.
    void finalizeAnswer();
    // Show an error for the current assistant turn.
    void setError(const QString& text);

signals:
    // Emitted when the user submits a follow-up question. *messages* is the full
    // role/content history (including the new question) to send to the backend.
    void questionSubmitted(const QJsonArray& messages);

private:
    void buildUI();
    QString renderTranscript() const;
    void renderNow();
    void submitQuestion();
    void updateInputState();
    QJsonArray messages() const;

    QTextEdit* m_view = nullptr;
    QLineEdit* m_input = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QLabel* m_title = nullptr;
    QTimer* m_renderTimer = nullptr;

    // Subject the transcript is about, kept so the title can be rebuilt after a
    // language change.
    QString m_subject;

    // Committed conversation turns as (role, content) pairs.
    QVector<QPair<QString, QString>> m_history;
    QString m_systemPrompt;  // prepended as {role:system} to every request
    QString m_buffer;      // current streaming assistant answer
    bool m_streaming = false;  // an assistant turn is in progress
    bool m_hasContent = false;
};