// knownfolder_probe (run5): the relocation tab's folder set and the roots the
// data-folder scan walks.
//
// Regression target: folders Windows creates on demand (3D Objects, Contacts,
// Links, Saved Games, Searches) and Music were not detected at all, and the
// LocalLow app-data root was silently missing from the scan. Both lists now live
// in headers the app itself includes, so this probe compiles the REAL logic:
//   src/core/KnownFolderTable.h  — the table (one source of truth)
//   src/core/KnownFolderPath.h   — path resolution (shell API + fallbacks)
//   src/core/ScanRoots.h         — the default scan roots
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <cstdio>

#include "KnownFolderPath.h"
#include "KnownFolderTable.h"
#include "ScanRoots.h"

static int gPass = 0, gFail = 0;
static FILE* gLog = nullptr;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

static QStringList localeKeys(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringList();
    return QJsonDocument::fromJson(f.readAll()).object().keys();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    gLog = fopen("knownfolder_probe.log", "w");
    const auto say = [](const QString& s) {
        fprintf(gLog, "%s\n", s.toUtf8().constData());
        fflush(gLog);
    };

    const QVector<KnownFolderEntry>& table = knownFolderEntries();

    // ---- 1. the table itself -------------------------------------------------
    say(QStringLiteral("[INFO] entries: %1").arg(table.size()));
    for (const KnownFolderEntry& e : table)
        say(QStringLiteral("[INFO]   %1  guid=%2  reg=%3  rel=%4  always=%5")
                .arg(e.id, e.guid, e.regValue, e.profileRelative)
                .arg(e.alwaysList ? QStringLiteral("yes") : QStringLiteral("no")));

    CHECK(table.size() == 12, "table holds 12 library folders");
    const QStringList required = {
        QStringLiteral("win_download"), QStringLiteral("win_documents"),
        QStringLiteral("win_desktop"), QStringLiteral("win_pictures"),
        QStringLiteral("win_videos"), QStringLiteral("win_music"),
        QStringLiteral("win_3dobjects"), QStringLiteral("win_contacts"),
        QStringLiteral("win_favorites"), QStringLiteral("win_links"),
        QStringLiteral("win_savedgames"), QStringLiteral("win_searches"),
    };
    bool allPresent = true;
    for (const QString& id : required)
        allPresent = allPresent && knownFolderById(id) != nullptr;
    CHECK(allPresent, "3D Objects / Contacts / Saved Games / Links / Searches / Favorites / Music all present");
    CHECK(knownFolderById(QStringLiteral("win_nope")) == nullptr, "unknown id resolves to nothing");

    QSet<QString> ids, guids, regs, folders, nameKeys;
    const QRegularExpression guidForm(
        QStringLiteral("^\\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\\}$"));
    int guidOk = 0, keyOk = 0, folderOk = 0, idOk = 0;
    for (const KnownFolderEntry& e : table) {
        if (!ids.contains(e.id)) { ids.insert(e.id); ++idOk; }
        if (guidForm.match(e.guid).hasMatch()) ++guidOk;
        if (!guids.contains(e.guid)) guids.insert(e.guid);
        if (!regs.contains(e.regValue)) regs.insert(e.regValue);
        if (!e.folder.isEmpty() && !folders.contains(e.folder.toLower())) {
            folders.insert(e.folder.toLower());
            ++folderOk;
        }
        if (e.nameKey.startsWith(QStringLiteral("app_sync.app."))) ++keyOk;
        if (!nameKeys.contains(e.nameKey)) nameKeys.insert(e.nameKey);
    }
    CHECK(idOk == table.size(), "ids are unique");
    CHECK(guids.size() == table.size(), "FOLDERIDs are unique");
    CHECK(regs.size() == table.size(), "registry value names are unique");
    CHECK(folderOk == table.size(), "batch subfolder names are unique (case-insensitive)");
    CHECK(guidOk == table.size(), "every FOLDERID is a well-formed braced GUID");
    CHECK(keyOk == table.size(), "every row carries an app_sync.app.* i18n key");
    CHECK(guids.contains(QStringLiteral("{31C0DD25-9439-4F12-BF41-7FF4EDA38722}")),
          "3D Objects uses the documented FOLDERID");

    // ---- 2. the i18n keys must exist in both languages ----------------------
    const QStringList zh = localeKeys(QStringLiteral("locales/zh.json"));
    const QStringList en = localeKeys(QStringLiteral("locales/en.json"));
    say(QStringLiteral("[INFO] locales: zh=%1 keys, en=%2 keys")
            .arg(zh.size()).arg(en.size()));
    if (zh.isEmpty() || en.isEmpty()) {
        CHECK(false, "locales/ present next to the probe (copy them in first)");
    } else {
        QStringList missing;
        for (const QString& key : nameKeys) {
            if (!zh.contains(key)) missing << QStringLiteral("zh:") + key;
            if (!en.contains(key)) missing << QStringLiteral("en:") + key;
        }
        if (!missing.isEmpty())
            say(QStringLiteral("[INFO] missing keys: %1").arg(missing.join(QStringLiteral(", "))));
        CHECK(missing.isEmpty(), "every table nameKey exists in zh.json and en.json");
    }

    // ---- 3. resolution against the real shell + registry --------------------
    QSettings regShell(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders"),
        QSettings::NativeFormat);

    int resolved = 0, resolvable = 0;
    for (const KnownFolderEntry& e : table) {
        const QString path = resolveKnownFolderPath(e, regShell);
        if (!path.isEmpty()) ++resolved;
        if (!path.isEmpty() && !QDir(path).exists()) {
            say(QStringLiteral("[INFO] resolved but missing: %1 -> %2").arg(e.id, path));
        }
        // The regression that started this: a folder that really is on disk must
        // be found, no matter which of the three sources answers.
        const QString onDisk =
            QDir::toNativeSeparators(QDir::homePath() + QLatin1Char('/') + e.profileRelative);
        if (QDir(onDisk).exists()) {
            ++resolvable;
            CHECK(!path.isEmpty(),
                  qPrintable(QStringLiteral("existing folder is detected: %1").arg(e.profileRelative)));
        }
        say(QStringLiteral("[INFO] %1 -> %2").arg(e.id, path.isEmpty() ? QStringLiteral("(none)") : path));
    }
    say(QStringLiteral("[INFO] resolved %1/%2, present-on-disk %3")
            .arg(resolved).arg(table.size()).arg(resolvable));
    CHECK(resolved >= 5, "at least the five everyday folders resolve to a real path");

    // ---- 4. the default scan roots -----------------------------------------
    const QStringList roots = userDataScanRoots();
    for (const QString& r : roots)
        say(QStringLiteral("[INFO] scan root: %1").arg(r));
    CHECK(roots.size() >= 3, "at least three default scan roots");

    const QString localLow = QDir::toNativeSeparators(
        qEnvironmentVariable("USERPROFILE") + QStringLiteral("/AppData/LocalLow"));
    const QString docsStd = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));

    bool hasRoaming = false, hasLocal = false, hasLocalLow = false, hasDocs = false;
    const QString profile = qEnvironmentVariable("USERPROFILE");
    // Expected spellings: the documented variables when the process has them,
    // the profile below otherwise — the same rule the header applies. (A shell
    // started with a minimal environment really can lack %APPDATA%; that is how
    // this probe found out.)
    const QString appData = qEnvironmentVariable("APPDATA").isEmpty()
        ? QDir::toNativeSeparators(profile + QStringLiteral("/AppData/Roaming"))
        : QDir::toNativeSeparators(qEnvironmentVariable("APPDATA"));
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA").isEmpty()
        ? QDir::toNativeSeparators(profile + QStringLiteral("/AppData/Local"))
        : QDir::toNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    for (const QString& r : roots) {
        if (r.compare(appData, Qt::CaseInsensitive) == 0)
            hasRoaming = true;
        if (r.compare(localAppData, Qt::CaseInsensitive) == 0)
            hasLocal = true;
        if (r.compare(localLow, Qt::CaseInsensitive) == 0)
            hasLocalLow = true;
        if (!docsStd.isEmpty() && r.compare(docsStd, Qt::CaseInsensitive) == 0)
            hasDocs = true;
    }
    CHECK(hasRoaming, "Roaming (%APPDATA%) is scanned");
    CHECK(hasLocal, "Local (%LOCALAPPDATA%) is scanned");
    // The one that was missing: no environment variable points at it, so it has
    // to be built from %USERPROFILE% — it was omitted entirely before this change.
    CHECK(QDir(localLow).exists(), "this machine has a LocalLow to test with");
    CHECK(hasLocalLow, "LocalLow (the low-integrity app-data root) is scanned");
    CHECK(docsStd.isEmpty() || hasDocs, "Documents is scanned");

    bool rootsExist = true, rootsUnique = true;
    QSet<QString> seen;
    for (const QString& r : roots) {
        if (!QDir(r).exists())
            rootsExist = false;
        const QString key = QDir::fromNativeSeparators(r).toLower();
        if (seen.contains(key))
            rootsUnique = false;
        seen.insert(key);
    }
    CHECK(rootsExist, "every default scan root exists on this machine");
    CHECK(rootsUnique, "no default scan root is listed twice");

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
