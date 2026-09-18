#pragma once

#include <QString>
#include <QVector>

// Windows' own library folders ("known folders") — every folder whose location
// Explorer itself can move through the folder's Properties > Location tab.
//
// One table, three consumers: the relocation tab reads the display data and the
// current path from it, SHSetKnownFolderPath is handed the FOLDERID, and the
// registry fallback writes *regValue*. Keeping them in a single row is what
// stops the three lists from drifting apart — a folder added here is detected,
// movable and restorable at once, which is exactly what the old hand-written
// maps (five entries in three places) could not guarantee.
//
// *guid* is the FOLDERID in its braced upper-case text form; CLSIDFromString
// turns it into a CLSID at the call site, so this header stays free of Windows
// types and can be unit-tested on any platform.
//
// *regValue* is the value name under
// HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders.
// Windows uses the classic names ("Personal", "My Pictures") for the folders it
// has always had, and the folder's own GUID for the ones introduced later.
//
// *profileRelative* is the last-resort default below %USERPROFILE%, used only
// when neither the shell nor the registry can answer.
struct KnownFolderEntry {
    QString id;               // stable id; also selects the entry everywhere else
    QString nameKey;          // I18n key for the display name
    QString folder;           // language-independent subfolder name (batch move)
    QString guid;             // FOLDERID: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
    QString regValue;         // "User Shell Folders" value name (fallback write)
    QString profileRelative;  // path below %USERPROFILE% (last-resort fallback)
    // Shown even when this machine has no such folder (with an empty path), as
    // the everyday folders always have been. The rest are listed only when the
    // folder really exists: Windows creates several of them on demand, and a row
    // for a folder that is not there is a row nobody can move.
    bool alwaysList;
};

inline const QVector<KnownFolderEntry>& knownFolderEntries()
{
    static const QVector<KnownFolderEntry> kEntries = {
        // --- the folders every profile has, in the order Explorer lists them ---
        { QStringLiteral("win_download"),
          QStringLiteral("app_sync.app.win_download"),
          QStringLiteral("Downloads"),
          QStringLiteral("{374DE290-123F-4565-9164-39C4925E467B}"),
          QStringLiteral("{374DE290-123F-4565-9164-39C4925E467B}"),
          QStringLiteral("Downloads"), true },
        { QStringLiteral("win_documents"),
          QStringLiteral("app_sync.app.win_documents"),
          QStringLiteral("Documents"),
          QStringLiteral("{FDD39AD0-238F-46AF-ADB4-6C85480369C7}"),
          QStringLiteral("Personal"),
          QStringLiteral("Documents"), true },
        { QStringLiteral("win_desktop"),
          QStringLiteral("app_sync.app.win_desktop"),
          QStringLiteral("Desktop"),
          QStringLiteral("{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}"),
          QStringLiteral("Desktop"),
          QStringLiteral("Desktop"), true },
        { QStringLiteral("win_pictures"),
          QStringLiteral("app_sync.app.win_pictures"),
          QStringLiteral("Pictures"),
          QStringLiteral("{33E28130-4E1E-4676-835A-98395C3BC3BB}"),
          QStringLiteral("My Pictures"),
          QStringLiteral("Pictures"), true },
        { QStringLiteral("win_videos"),
          QStringLiteral("app_sync.app.win_videos"),
          QStringLiteral("Videos"),
          QStringLiteral("{18989B1D-99B5-455B-841C-AB7C74E4DDFC}"),
          QStringLiteral("My Video"),
          QStringLiteral("Videos"), true },
        { QStringLiteral("win_music"),
          QStringLiteral("app_sync.app.win_music"),
          QStringLiteral("Music"),
          QStringLiteral("{4BD8D571-6D19-48D3-BE97-422220080E43}"),
          QStringLiteral("My Music"),
          QStringLiteral("Music"), true },

        // --- present on demand: detected when they exist, on whatever drive ---
        { QStringLiteral("win_3dobjects"),
          QStringLiteral("app_sync.app.win_3dobjects"),
          QStringLiteral("3D Objects"),
          QStringLiteral("{31C0DD25-9439-4F12-BF41-7FF4EDA38722}"),
          QStringLiteral("{31C0DD25-9439-4F12-BF41-7FF4EDA38722}"),
          QStringLiteral("3D Objects"), false },
        { QStringLiteral("win_contacts"),
          QStringLiteral("app_sync.app.win_contacts"),
          QStringLiteral("Contacts"),
          QStringLiteral("{56784854-C6CB-462B-8169-88E350ACB882}"),
          QStringLiteral("{56784854-C6CB-462B-8169-88E350ACB882}"),
          QStringLiteral("Contacts"), false },
        { QStringLiteral("win_favorites"),
          QStringLiteral("app_sync.app.win_favorites"),
          QStringLiteral("Favorites"),
          QStringLiteral("{1777F761-68AD-4D8A-87BD-30B759FA33DD}"),
          QStringLiteral("Favorites"),
          QStringLiteral("Favorites"), false },
        { QStringLiteral("win_links"),
          QStringLiteral("app_sync.app.win_links"),
          QStringLiteral("Links"),
          QStringLiteral("{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}"),
          QStringLiteral("{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}"),
          QStringLiteral("Links"), false },
        { QStringLiteral("win_savedgames"),
          QStringLiteral("app_sync.app.win_savedgames"),
          QStringLiteral("Saved Games"),
          QStringLiteral("{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}"),
          QStringLiteral("{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}"),
          QStringLiteral("Saved Games"), false },
        { QStringLiteral("win_searches"),
          QStringLiteral("app_sync.app.win_searches"),
          QStringLiteral("Searches"),
          QStringLiteral("{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}"),
          QStringLiteral("{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}"),
          QStringLiteral("Searches"), false },
    };
    return kEntries;
}

inline const KnownFolderEntry* knownFolderById(const QString& id)
{
    for (const KnownFolderEntry& entry : knownFolderEntries()) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}
