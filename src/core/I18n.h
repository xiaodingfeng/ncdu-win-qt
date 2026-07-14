#pragma once

#include <QString>
#include <QStringList>
#include <QMap>

// Internationalization for NcduWin.
//
// A tiny JSON-based translation system. Translations live in
// ``locales/<lang>.json`` (copied next to the executable at build time, or
// embedded via a Qt resource at ``:/locales/<lang>.json``). The active
// language is persisted to the registry (HKEY_CURRENT_USER\Software\NcduWin)
// via QSettings.
//
// Usage:
//   I18n::load();                       // load persisted preference (or auto-detect)
//   I18n::setLanguage("zh");            // switch at runtime
//   I18n::tr("button.scan");            // -> "扫描" / "Scan"
namespace I18n {

// Load every supported locale and pick the persisted (or detected) language.
// Returns the active language code.
QString load();

// The currently active language code (e.g. "en", "zh").
QString currentLanguage();

// Switch the active language at runtime and persist the choice.
void setLanguage(const QString& code);

// Supported language codes (e.g. ["en", "zh"]).
QStringList availableLanguages();

// Native display name for a language code (e.g. "zh" -> "简体中文").
QString languageDisplayName(const QString& code);

// Translate a key. Falls back to English, then to the key itself.
QString tr(const QString& key);

// Translate a key and interpolate {placeholder} tokens from ``args``.
QString tr(const QString& key, const QMap<QString, QString>& args);

}  // namespace I18n
