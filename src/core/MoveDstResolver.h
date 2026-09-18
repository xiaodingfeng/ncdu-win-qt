#pragma once

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QMap>
#include <QString>
#include <QStringList>

// Batch-move destination resolution.
//
// Every picked folder lands under the target root MIRRORED by its source
// parent: "root\Roaming\Foo" for a folder coming from AppData\Roaming,
// "root\Local\Foo" for one from AppData\Local. Qualifying unconditionally
// (rather than only on a name clash) keeps a whole batch readable — the
// destination mirrors where each folder came from, the way a full profile
// migration lays out the original structure — and same-named folders from
// Roaming and Local can never collide in the first place.
//
// Parent + name is unique per source path, so two picked folders cannot
// clash with each other. The only remaining clash is against a folder still
// queued for this same root whose relative path is identical — that stays
// an error (*ok = false) rather than a silent merge.
//
// The function is deliberately pure (no panel state, no dialogs) so the
// collision rules can be unit-tested without the UI.
//
// KEY CONTRACT: the returned map is keyed by POSITION in the names/paths
// lists (0..n-1), NOT by any caller-side directory index. Callers must remap
// to their own identifiers before looking entries up — a direct
// .value(callerIndex) silently returns an empty path for any non-zero-based
// selection, which downstream existence checks then misread.
inline QMap<int, QString> resolveBatchDst(const QString& root,
                                          const QStringList& names,
                                          const QStringList& paths,
                                          const QStringList& queuedRel,
                                          bool* ok)
{
    // Relative paths already claimed under this root, normalized and lowercased
    // for a case-insensitive (NTFS) comparison.
    QSet<QString> taken;
    for (const QString& rel : queuedRel)
        taken.insert(QDir::fromNativeSeparators(rel).toLower());

    QMap<int, QString> out;
    *ok = true;
    for (int i = 0; i < names.size(); ++i) {
        // Source parent ("Roaming", "Local", …) is always kept as an extra
        // level. dirName(), NOT fileName(): the entry's own name is the last
        // component already, the qualifier has to be the folder it came from.
        const QString rel = QFileInfo(paths.at(i)).dir().dirName()
                            + QLatin1Char('/') + names.at(i);
        const QString key = QDir::fromNativeSeparators(rel).toLower();
        if (taken.contains(key)) {
            *ok = false;
            return QMap<int, QString>();
        }
        taken.insert(key);
        out.insert(i, QDir::toNativeSeparators(QDir::cleanPath(root + QLatin1Char('\\') + rel)));
    }
    return out;
}
