#include "AiService.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

#include "version.h"
#include "Logger.h"

namespace {
constexpr const char* KEY_BASE_URL = "aiBaseUrl";
constexpr const char* KEY_API_KEY  = "aiApiKey";
constexpr const char* KEY_MODEL    = "aiModel";
constexpr const char* DEFAULT_MODEL = "auto";
constexpr int AI_TRANSFER_TIMEOUT_MS = 30000;

QSettings settings()
{
    // Same registry location as I18n/Theme.
    return QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\NcduWin"),
                     QSettings::NativeFormat);
}
}  // namespace

// --------------------------------------------------------------------------- //
// Construction / persistence
// --------------------------------------------------------------------------- //
AiService::AiService(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

AiConfig AiService::load() const
{
    AiConfig cfg;
    QSettings s = settings();
    cfg.baseUrl = s.value(QLatin1String(KEY_BASE_URL)).toString();
    cfg.apiKey  = s.value(QLatin1String(KEY_API_KEY)).toString();
    cfg.model   = s.value(QLatin1String(KEY_MODEL)).toString();
    // First-run default: model "auto". The Base URL / API Key are fetched at
    // runtime from the homepage (see ensureConfigured) when not saved yet.
    if (cfg.model.isEmpty())
        cfg.model = QString::fromLatin1(DEFAULT_MODEL);
    return cfg;
}

void AiService::save(const AiConfig& cfg)
{
    QSettings s = settings();
    s.setValue(QLatin1String(KEY_BASE_URL), cfg.baseUrl);
    s.setValue(QLatin1String(KEY_API_KEY), cfg.apiKey);
    s.setValue(QLatin1String(KEY_MODEL), cfg.model);
    s.sync();
}

bool AiService::isConfigured(const AiConfig& cfg) const
{
    return !cfg.baseUrl.trimmed().isEmpty() && !cfg.apiKey.trimmed().isEmpty();
}

bool AiService::needsDefaults() const
{
    QSettings s = settings();
    return s.value(QLatin1String(KEY_BASE_URL)).toString().trimmed().isEmpty()
        || s.value(QLatin1String(KEY_API_KEY)).toString().trimmed().isEmpty();
}

// --------------------------------------------------------------------------- //
// Async API calls
// --------------------------------------------------------------------------- //

// Fetch the default Base URL from the homepage (HOMEPAGE + "aiBaseUrl").
void AiService::fetchBaseUrl()
{
    QNetworkRequest req(QUrl(QString(HOMEPAGE) + QStringLiteral("aiBaseUrl")));
    req.setTransferTimeout(AI_TRANSFER_TIMEOUT_MS);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            Logger::warn(QStringLiteral("AI: failed to fetch default Base URL: %1")
                             .arg(reply->errorString()));
            emit baseUrlFetchFinished();
            return;
        }
        const QString url = QString::fromUtf8(reply->readAll()).trimmed();
        emit baseUrlFetched(url);
        emit baseUrlFetchFinished();
    });
}

// Fetch the default API Key from the homepage (HOMEPAGE + "aiApiKey").
void AiService::fetchApiKey()
{
    QNetworkRequest req(QUrl(QString(HOMEPAGE) + QStringLiteral("aiApiKey")));
    req.setTransferTimeout(AI_TRANSFER_TIMEOUT_MS);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            Logger::warn(QStringLiteral("AI: failed to fetch default API key: %1")
                             .arg(reply->errorString()));
            emit apiKeyFetchFinished();
            return;
        }
        const QString key = QString::fromUtf8(reply->readAll()).trimmed();
        emit apiKeyFetched(key);
        emit apiKeyFetchFinished();
    });
}

// Fetch both the default Base URL and API Key from the homepage.
void AiService::fetchDefaults()
{
    fetchBaseUrl();
    fetchApiKey();
}

// Silently fetch-and-save the default Base URL / API Key when the saved config
// is empty. Guards each write so it never overwrites a value the user entered.
void AiService::ensureConfigured()
{
    if (!needsDefaults())
        return;
    connect(this, &AiService::baseUrlFetched, this, [this](const QString& url) {
        const QString v = url.trimmed();
        if (v.isEmpty())
            return;
        QSettings s = settings();
        if (s.value(QLatin1String(KEY_BASE_URL)).toString().trimmed().isEmpty()) {
            s.setValue(QLatin1String(KEY_BASE_URL), v);
            s.sync();
        }
    });
    connect(this, &AiService::apiKeyFetched, this, [this](const QString& key) {
        const QString v = key.trimmed();
        if (v.isEmpty())
            return;
        QSettings s = settings();
        if (s.value(QLatin1String(KEY_API_KEY)).toString().trimmed().isEmpty()) {
            s.setValue(QLatin1String(KEY_API_KEY), v);
            s.sync();
        }
    });
    fetchDefaults();
}

// List models from {baseUrl}/models.
void AiService::fetchModels()
{
    const AiConfig cfg = load();
    const QString base = cfg.baseUrl.trimmed();
    if (base.isEmpty()) {
        emit modelsFailed(QStringLiteral("empty base URL"));
        return;
    }
    QUrl url(base + QStringLiteral("/models"));
    QNetworkRequest req(url);
    req.setTransferTimeout(AI_TRANSFER_TIMEOUT_MS);
    req.setRawHeader("Authorization", "Bearer " + cfg.apiKey.trimmed().toUtf8());
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit modelsFailed(reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        QStringList ids;
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            const QJsonArray arr = doc.object().value(QLatin1String("data")).toArray();
            for (const auto& v : arr) {
                if (v.isObject()) {
                    const QString id = v.toObject().value(QLatin1String("id")).toString();
                    if (!id.isEmpty())
                        ids << id;
                }
            }
        }
        emit modelsFetched(ids);
    });
}

// Send a streamed chat completion request for the given prompt.
void AiService::analyze(const QString& prompt, const QString& overrideModel)
{
    QJsonObject msg;
    msg.insert(QLatin1String("role"), QLatin1String("user"));
    msg.insert(QLatin1String("content"), prompt);
    sendCompletion(QJsonArray{msg}, overrideModel);
}

// Send a streamed chat completion request over the given full message history so
// follow-up questions can reference the previous analysis (Q&A context).
void AiService::analyzeMessages(const QJsonArray& messages, const QString& overrideModel)
{
    sendCompletion(messages, overrideModel);
}

void AiService::sendCompletion(const QJsonArray& messages, const QString& overrideModel)
{
    const AiConfig cfg = load();
    const QString base = cfg.baseUrl.trimmed();
    if (base.isEmpty()) {
        emit analysisFailed(QStringLiteral("empty base URL"));
        return;
    }
    QUrl url(base + QStringLiteral("/chat/completions"));
    QNetworkRequest req(url);
    req.setTransferTimeout(AI_TRANSFER_TIMEOUT_MS);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + cfg.apiKey.trimmed().toUtf8());

    // Use the explicit override when given (e.g. "auto" resolved to the first
    // available model); otherwise fall back to the configured model.
    QString model = overrideModel.trimmed();
    if (model.isEmpty() || model == QLatin1String(DEFAULT_MODEL))
        model = cfg.model.trimmed();
    if (model.isEmpty())
        model = QLatin1String(DEFAULT_MODEL);

    QJsonObject body;
    body.insert(QLatin1String("model"), model);
    body.insert(QLatin1String("messages"), messages);
    body.insert(QLatin1String("stream"), true);

    m_sseBuffer.clear();
    QNetworkReply* reply = m_nam->post(req,
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        m_sseBuffer += reply->readAll();
        processSse(m_sseBuffer);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit analysisFailed(reply->errorString());
            return;
        }
        // Flush any remaining buffered events, then finish the stream.
        m_sseBuffer += reply->readAll();
        processSse(m_sseBuffer);
        m_sseBuffer.clear();
        emit analysisStreamEnded();
    });
}

// Parse Server-Sent-Events out of *buffer*, emitting an analysisChunk for each
// content delta. Consumes complete events (separated by a blank line) and
// leaves partial trailing data in *buffer* for the next readyRead.
void AiService::processSse(QByteArray& buffer)
{
    // Normalize CRLF line endings to LF.
    buffer.replace("\r\n", "\n");
    int pos;
    while ((pos = buffer.indexOf("\n\n")) >= 0) {
        const QByteArray event = buffer.left(pos);
        buffer.remove(0, pos + 2);
        const QList<QByteArray> lines = event.split('\n');
        for (const QByteArray& line : lines) {
            if (!line.startsWith("data:"))
                continue;
            const QByteArray data = line.mid(5).trimmed();
            if (data == "[DONE]")
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isObject())
                continue;
            const QJsonArray choices = doc.object().value(QLatin1String("choices")).toArray();
            if (choices.isEmpty() || !choices.first().isObject())
                continue;
            const QJsonObject delta = choices.first().toObject()
                                         .value(QLatin1String("delta")).toObject();
            const QString text = delta.value(QLatin1String("content")).toString();
            if (!text.isEmpty())
                emit analysisChunk(text);
        }
    }
}