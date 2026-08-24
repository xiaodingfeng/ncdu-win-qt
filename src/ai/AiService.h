#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>

class QNetworkAccessManager;
class QJsonArray;

// AI service configuration: Base URL / API Key / Model.
struct AiConfig {
    QString baseUrl;
    QString apiKey;
    QString model;
};

// AiService - backend for the optional AI analysis feature.
//
// Persists AI settings (Base URL / API Key / Model) to the registry
// (HKEY_CURRENT_USER\Software\NcduWin) and performs asynchronous OpenAI-style
// HTTP calls (list models, chat completion). Every network call is async and
// never throws or blocks the UI thread; failures are delivered via the
// *Failed signals so the UI can prompt the user to enter settings manually.
//
// Default Base URL and API Key are fetched at runtime from HOMEPAGE +
// "aiBaseUrl" / "aiApiKey" (see ensureConfigured / fetchDefaults). The default
// model is "auto".
class AiService : public QObject {
    Q_OBJECT
public:
    explicit AiService(QObject* parent = nullptr);

    // Persisted settings (registry). Missing keys fall back to defaults.
    AiConfig load() const;
    void save(const AiConfig& cfg);

    // True when Base URL and API Key are both non-empty.
    bool isConfigured(const AiConfig& cfg) const;

    // True when Base URL or API Key has not been saved yet (raw registry).
    // Used to decide whether to silently fetch the defaults.
    bool needsDefaults() const;

    // Async API calls.
    void fetchBaseUrl();          // GET HOMEPAGE + "aiBaseUrl"
    void fetchApiKey();           // GET HOMEPAGE + "aiApiKey"
    void fetchDefaults();         // GET both default Base URL and API Key
    void fetchModels();           // GET {baseUrl}/models
    void analyze(const QString& prompt, const QString& overrideModel = {});  // POST {baseUrl}/chat/completions (streamed)
    // Send a full message history (role/content pairs, e.g. for follow-up Q&A
    // with conversation context) to {baseUrl}/chat/completions (streamed).
    void analyzeMessages(const QJsonArray& messages, const QString& overrideModel = {});

    // Silently fetch-and-save the default Base URL / API Key when the saved
    // config is empty. Runs in the background; never shows UI or blocks.
    void ensureConfigured();

signals:
    void baseUrlFetched(const QString& url);
    void baseUrlFetchFinished();
    void apiKeyFetched(const QString& key);
    void apiKeyFetchFinished();
    void modelsFetched(const QStringList& ids);
    void modelsFailed(const QString& error);
    void analysisChunk(const QString& delta);   // streamed markdown fragment
    void analysisStreamEnded();                 // stream finished cleanly
    void analysisFailed(const QString& error);

private:
    void processSse(QByteArray& buffer);
    void sendCompletion(const QJsonArray& messages, const QString& overrideModel);

    QNetworkAccessManager* m_nam = nullptr;
    QByteArray m_sseBuffer;
};