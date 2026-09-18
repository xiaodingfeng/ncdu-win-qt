#pragma once

#include <QDir>
#include <QSettings>
#include <QString>

#include "KnownFolderTable.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <objbase.h>   // CLSIDFromString / CoTaskMemFree
#  include <shlobj.h>
#endif

// Registry values under "User Shell Folders" are REG_EXPAND_SZ (for example
// "%USERPROFILE%\Downloads"). QSettings returns them verbatim, so expand the
// %VAR% tokens before the path is displayed or compared against the disk.
inline QString knownFolderExpandEnvVars(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        if (value.at(i) == QLatin1Char('%')) {
            const int end = value.indexOf(QLatin1Char('%'), i + 1);
            if (end > i + 1) {
                const QString name = value.mid(i + 1, end - i - 1);
                const QString expanded = qEnvironmentVariable(name.toUtf8().constData());
                if (!expanded.isEmpty()) {
                    out += expanded;
                    i = end;
                    continue;
                }
            }
        }
        out += value.at(i);
    }
    return out;
}

inline QString knownFolderNativeDir(const QString& raw)
{
    if (raw.isEmpty())
        return QString();
    return QDir::toNativeSeparators(QDir::cleanPath(raw));
}

// Where a library folder really lives right now.
//
// SHGetKnownFolderPath is asked FIRST: it is the only source that knows about
// every redirection Windows supports (OneDrive, a moved profile, a per-machine
// override) and it answers for folders whose per-user registry value was never
// written — which is the norm, not the exception, for 3D Objects, Contacts,
// Links, Saved Games and Searches. Resolving them through the registry alone is
// exactly why those folders used to be invisible.
//
// The registry value and the default name below %USERPROFILE% stay as fallbacks
// for the rare case where the shell cannot answer (a heavily locked-down or
// still-initialising profile).
//
// Returns an empty string when there is no such folder on this machine — the
// caller decides whether that hides the row or shows it with an empty path.
inline QString resolveKnownFolderPath(const KnownFolderEntry& entry, const QSettings& regShell)
{
#ifdef _WIN32
    CLSID clsid{};
    if (SUCCEEDED(CLSIDFromString(reinterpret_cast<const OLECHAR*>(entry.guid.utf16()), &clsid))) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(clsid, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
            const QString resolved = knownFolderNativeDir(QString::fromWCharArray(raw));
            CoTaskMemFree(raw);
            if (QDir(resolved).exists())
                return resolved;
        }
    }
#else
    Q_UNUSED(regShell);
#endif
    const QString fromRegistry =
        knownFolderNativeDir(knownFolderExpandEnvVars(regShell.value(entry.regValue).toString()));
    if (!fromRegistry.isEmpty() && QDir(fromRegistry).exists())
        return fromRegistry;

    const QString fromProfile =
        knownFolderNativeDir(QDir::homePath() + QLatin1Char('/') + entry.profileRelative);
    return QDir(fromProfile).exists() ? fromProfile : QString();
}
