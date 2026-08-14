#pragma once

#include <QDialog>

class QLineEdit;
class QComboBox;
class QPushButton;
class AiService;

// AiSettingsDialog - edit AI service settings (Base URL / API Key / Model).
//
// Allows the user to configure the AI service used by the optional analysis
// feature. The model defaults to "auto". If the Base URL or API Key is empty
// it is fetched asynchronously from the homepage on open. A "restore defaults"
// button re-fetches both. Models can be fetched from the configured endpoint
// and picked from a list. Any failure is surfaced as a non-blocking message;
// the user can always save manual values.
class AiSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiSettingsDialog(AiService* service, QWidget* parent = nullptr);

private slots:
    void onFetchModels();
    void onRestoreDefaults();
    void onSave();

private:
    void buildUI();
    void applyStyle();
    void restoreDefaultsStep();

    AiService* m_service = nullptr;
    QLineEdit* m_baseUrlEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QComboBox* m_modelCombo = nullptr;
    QPushButton* m_fetchBtn = nullptr;
    QPushButton* m_restoreBtn = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    int m_pendingDefaults = 0;
};