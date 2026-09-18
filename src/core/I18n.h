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

// Switch the active language at runtime. ``persist`` writes the choice to
// settings (what the language switcher wants); pass false to switch for this
// run only — the marker-file tests do that so a probe never leaves the app
// itself in a language the user did not choose.
void setLanguage(const QString& code, bool persist = true);

// Supported language codes (e.g. ["en", "zh"]).
QStringList availableLanguages();

// Native display name for a language code (e.g. "zh" -> "简体中文").
QString languageDisplayName(const QString& code);

// Translate a key. Falls back to English, then to the key itself.
QString tr(const QString& key);

// Translate a key in a NAMED language without switching the active one.
// Same fallback chain as tr(): the named table, then English, then the key
// itself — so a key with no translation returns the key verbatim.
//
// Needed for artefacts that stay on disk across a language change: the
// "do not rename" marker file is written under the active language's name, and
// a later session in another language still has to recognise the one already
// there. Asking every shipped language for the same key keeps locales/*.json
// the single source of truth for the spelling.
QString trIn(const QString& lang, const QString& key);

// Translate a key and interpolate {placeholder} tokens from ``args``.
QString tr(const QString& key, const QMap<QString, QString>& args);

}  // namespace I18n
