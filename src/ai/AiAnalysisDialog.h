#pragma once

#include <QDialog>

class QTextEdit;
class QPushButton;
class QTimer;

// AiAnalysisDialog - read-only dialog showing the AI analysis result.
//
// Displays the result of a streamed AI analysis for a disk object. While the
// request is pending it shows a "loading" placeholder; incoming stream chunks
// are accumulated and rendered as Markdown on a short debounce timer, which
// produces a live "typewriter" effect without blocking the UI. Provides copy
// and close actions.
class AiAnalysisDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiAnalysisDialog(QWidget* parent = nullptr);

    void setSubject(const QString& subject);
    void setLoading();
    void appendChunk(const QString& delta);
    void finalize();
    void setError(const QString& text);

private:
    void buildUI();
    void renderMarkdown();

    QTextEdit* m_view = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QTimer* m_renderTimer = nullptr;
    QString m_buffer;
    bool m_hasContent = false;
};