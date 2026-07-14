#include "I18n.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QCoreApplication>

namespace I18n {

namespace {

// lang code -> (key -> value)
QHash<QString, QHash<QString, QString>> g_translations;
QString g_currentLang;
bool g_loaded = false;

const QStringList& supported() {
    static const QStringList s = { QStringLiteral("en"), QStringLiteral("zh") };
    return s;
}

// Build candidate paths for a locale file. Tries the Qt resource system
// (:/locales/...) first, then the directory next to the executable, then the
// working directory.
QStringList localeCandidates(const QString& lang) {
    const QString rel = QStringLiteral("locales/") + lang + QStringLiteral(".json");
    QStringList out;
    out << QStringLiteral(":") + rel;
    const QString appDir = QCoreApplication::applicationDirPath();
    out << appDir + QStringLiteral("/") + rel;
    out << rel;
    return out;
}

QByteArray loadFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return data;
}

void loadTranslationsIfNeeded() {
    if (g_loaded)
        return;
    g_loaded = true;
    for (const QString& lang : supported()) {
        QHash<QString, QString> table;
        for (const QString& cand : localeCandidates(lang)) {
            const QByteArray raw = loadFile(cand);
            if (raw.isEmpty())
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject())
                break;
            const QJsonObject obj = doc.object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                table.insert(it.key(), it.value().toString());
            break;  // first readable candidate wins
        }
        g_translations.insert(lang, table);
    }
}

QString detectSystemLanguage() {
    // QLocale::system().name() returns e.g. "zh_CN" / "en_US".
    const QString name = QLocale::system().name().toLower();
    if (name.startsWith(QStringLiteral("zh")))
        return QStringLiteral("zh");
    return QStringLiteral("en");
}

QSettings settings() {
    // Persist to HKEY_CURRENT_USER\Software\NcduWin on Windows.
    return QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\NcduWin"),
                    QSettings::NativeFormat);
}

}  // namespace

QString load() {
    loadTranslationsIfNeeded();
    QString lang = settings().value(QStringLiteral("language")).toString();
    if (!supported().contains(lang)) {
        lang = detectSystemLanguage();
        if (!supported().contains(lang))
            lang = QStringLiteral("en");
    }
    g_currentLang = lang;
    return g_currentLang;
}

QString currentLanguage() {
    return g_currentLang;
}

void setLanguage(const QString& code) {
    if (!supported().contains(code))
        return;
    loadTranslationsIfNeeded();
    g_currentLang = code;
    settings().setValue(QStringLiteral("language"), code);
}

QStringList availableLanguages() {
    return supported();
}

QString languageDisplayName(const QString& code) {
    if (code == QStringLiteral("zh"))
        // UCN escapes keep this source-encoding-independent (no /utf-8 needed).
        // 简体中文 = U+7B80 U+4F53 U+4E2D U+6587
        return QString::fromUtf8(u8"\u7B80\u4F53\u4E2D\u6587");
    if (code == QStringLiteral("en"))
        return QStringLiteral("English");
    return code;
}

QString tr(const QString& key) {
    // Current language table.
    auto curIt = g_translations.find(g_currentLang);
    if (curIt != g_translations.end() && curIt->contains(key))
        return curIt->value(key);
    // Fall back to English so missing translations still render something.
    auto enIt = g_translations.find(QStringLiteral("en"));
    if (enIt != g_translations.end() && enIt->contains(key))
        return enIt->value(key);
    return key;
}

QString tr(const QString& key, const QMap<QString, QString>& args) {
    QString s = tr(key);  // resolve template (with fallback)
    if (args.isEmpty())
        return s;
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        s.replace(QStringLiteral("{") + it.key() + QStringLiteral("}"), it.value());
    }
    return s;
}

}  // namespace I18n
