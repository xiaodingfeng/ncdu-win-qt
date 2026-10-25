#pragma once

#include <QDialog>

class QLineEdit;
class QTextEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QNetworkAccessManager;

// FeedbackDialog - modal form that posts user feedback to the notification
// service. The endpoint (FEEDBACK_URL) is injected from CMakeLists.txt the
// same way HOMEPAGE is, so both live in one place.
//
// Only the message body is required; title / contact / level are optional and
// the app version plus system description are attached automatically.
class FeedbackDialog : public QDialog {
    Q_OBJECT
public:
    explicit FeedbackDialog(QWidget* parent = nullptr);

private:
    void buildUI();
    void applyStyle();
    void onSubmit();

    QLineEdit* m_titleEdit = nullptr;
    QTextEdit* m_contentEdit = nullptr;
    QLineEdit* m_contactEdit = nullptr;
    QComboBox* m_levelCombo = nullptr;
    QLabel* m_hintLabel = nullptr;
    QPushButton* m_submitBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
};
