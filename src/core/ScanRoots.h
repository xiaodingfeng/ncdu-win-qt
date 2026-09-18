#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

// The per-user roots the "software data folders" scan walks by default.
//
// All THREE per-user application-data roots belong here. Windows splits them by
// integrity level, and each one grows its own bundle of software data:
//   Roaming    (%APPDATA%)      — data that follows the user between machines
//   Local      (%LOCALAPPDATA%) — machine-bound caches, the biggest of the three
//   LocalLow   (%USERPROFILE%\AppData\LocalLow) — the low-integrity root that
//                                browsers' sandboxed helpers, Unity/Unreal
//                                titles and installers write to
// LocalLow has no environment variable of its own (Windows defines APPDATA and
// LOCALAPPDATA only), which is why it was missing from a list built purely from
// %VAR% lookups — a real gap, not a deliberate omission: it is ordinary user
// data and grows just as large.
//
// Documents is included as well: a good share of programs ignore AppData and
// keep their projects next to the user's documents.
//
// The set is pinned by probe_lab/run5, so a root can no longer disappear
// unnoticed the way LocalLow did.
inline QStringList userDataScanRoots()
{
    QStringList roots;
    const QString profile = qEnvironmentVariable("USERPROFILE");
    // The two documented variables are the primary source; a process started
    // with a minimal environment can lack them, and a root that is dropped in
    // silence hides every folder under it. The same locations below the profile
    // are the fallback, which is where the variables point anyway.
    const QString roaming = qEnvironmentVariable("APPDATA").isEmpty()
        ? (profile.isEmpty() ? QString()
                             : QDir::toNativeSeparators(profile + QStringLiteral("/AppData/Roaming")))
        : QDir::toNativeSeparators(qEnvironmentVariable("APPDATA"));
    const QString local = qEnvironmentVariable("LOCALAPPDATA").isEmpty()
        ? (profile.isEmpty() ? QString()
                             : QDir::toNativeSeparators(profile + QStringLiteral("/AppData/Local")))
        : QDir::toNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    const QString localLow = profile.isEmpty()
        ? QString()
        : QDir::toNativeSeparators(profile + QStringLiteral("/AppData/LocalLow"));
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    if (!roaming.isEmpty())
        roots << roaming;
    if (!local.isEmpty())
        roots << local;
    // Only when it is really there: an absent LocalLow must not add a root that
    // can only ever produce an empty list.
    if (!localLow.isEmpty() && QDir(localLow).exists())
        roots << localLow;
    if (!docs.isEmpty())
        roots << QDir::toNativeSeparators(docs);

    // A profile where two of these resolve to the same place (redirected
    // AppData, junctioned profile) must not be walked twice: every folder under
    // it would be listed once per matching root.
    QStringList unique;
    for (const QString& root : roots) {
        bool duplicate = false;
        for (const QString& seen : unique) {
            if (seen.compare(root, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            unique << root;
    }
    return unique;
}
