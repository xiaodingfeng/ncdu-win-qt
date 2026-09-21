#include "OpGuard.h"

#include <QDir>
#include <QFileInfo>

#include "FormatHelpers.h"
#include "I18n.h"
#include "Logger.h"

namespace OpGuard {

bool onlyBackgroundHolders(const QVector<WinApi::LockingProcess>& procs)
{
    for (const WinApi::LockingProcess& p : procs) {
        if (p.safeToClose || p.blockKey != QLatin1String("proc_close.block_system"))
            return false;
    }
    return true;
}

QStringList namesOf(const QVector<WinApi::LockingProcess>& procs)
{
    QStringList names;
    for (const WinApi::LockingProcess& p : procs) {
        if (!p.name.isEmpty() && !names.contains(p.name))
            names << p.name;
    }
    return names;
}

bool blocksOperation(const QVector<WinApi::LockingProcess>& procs)
{
    return !procs.isEmpty() && !onlyBackgroundHolders(procs);
}

QVector<WinApi::LockingProcess> probe(const QStringList& paths, int maxFiles)
{
    // Narrow the list before walking anything. A selection that contains a
    // folder and something inside it ("C:\A" and "C:\A\B") would otherwise be
    // walked twice for the same answer, and each walk is the slow part.
    QStringList roots;
    for (const QString& raw : paths) {
        const QString path = QDir::cleanPath(raw);
        if (path.isEmpty() || path == QLatin1String("."))
            continue;
        bool covered = false;
        for (int i = roots.size() - 1; i >= 0; --i) {
            if (WinApi::pathInsideDirectory(path, roots.at(i))) {
                covered = true;
                break;
            }
            // This one is broader than a path already queued: forget the
            // narrower one, this walk covers it too.
            if (WinApi::pathInsideDirectory(roots.at(i), path))
                roots.removeAt(i);
        }
        if (!covered)
            roots << path;
    }

    QVector<WinApi::LockingProcess> all;
    for (const QString& root : roots) {
        // A folder that is already gone cannot be holding anything open, and
        // asking about it would only produce an entry for a path the user can no
        // longer see.
        if (!QFileInfo::exists(root))
            continue;
        QVector<WinApi::LockingProcess> found;
        WinApi::processesLockingDir(root, &found, maxFiles);
        for (const WinApi::LockingProcess& p : found) {
            bool known = false;
            for (const WinApi::LockingProcess& q : all) {
                if (q.pid == p.pid) {
                    known = true;
                    break;
                }
            }
            if (!known)
                all << p;
        }
    }
    return all;
}

namespace {

bool isClean(const WinApi::DeleteResult& r)
{
    return r.gone && r.reason != WinApi::DeleteReason::Missing;
}

} // namespace

QString reasonSentence(WinApi::DeleteReason reason, quint32 winError)
{
    const QString key = WinApi::deleteReasonKey(reason);
    if (key.isEmpty())
        return QString();
    // Unknown carries its code along: "未知原因" on its own is a dead end, while
    // the code is the one thing a search engine can answer.
    return I18n::tr(key, QMap<QString, QString>{
                            {"code", QStringLiteral("0x%1").arg(winError, 0, 16)}});
}

QString resultLine(const WinApi::DeleteResult& r, bool recycled)
{
    // The item's own name is what the user recognises; the full path is only
    // there for the cases where there is no name to show.
    const QString name = QFileInfo(r.path).fileName();
    const QString shown = name.isEmpty() ? r.path : name;

    if (isClean(r)) {
        if (recycled) {
            return I18n::tr(QStringLiteral("op_result.line_ok_recycle"),
                            QMap<QString, QString>{{"name", shown}});
        }
        return I18n::tr(QStringLiteral("op_result.line_ok"),
                        QMap<QString, QString>{{"name", shown},
                                               {"freed", humanSize(r.freedBytes)}});
    }
    if (r.reason == WinApi::DeleteReason::Missing) {
        return I18n::tr(QStringLiteral("op_result.line_missing"),
                        QMap<QString, QString>{{"name", shown}});
    }
    if (r.partial) {
        return I18n::tr(QStringLiteral("op_result.line_partial"),
                        QMap<QString, QString>{{"name", shown},
                                               {"freed", humanSize(r.freedBytes)},
                                               {"reason", reasonSentence(r.reason, r.winError)}});
    }
    return I18n::tr(QStringLiteral("op_result.line_failed"),
                    QMap<QString, QString>{{"name", shown}, {"reason", reasonSentence(r.reason, r.winError)}});
}

bool allClean(const QVector<WinApi::DeleteResult>& results)
{
    for (const WinApi::DeleteResult& r : results) {
        if (!isClean(r))
            return false;
    }
    return true;
}

Tally tally(const QVector<WinApi::DeleteResult>& results)
{
    Tally t;
    for (const WinApi::DeleteResult& r : results) {
        if (isClean(r)) {
            ++t.ok;
            t.freed += r.freedBytes;
        } else {
            ++t.bad;
        }
    }
    return t;
}

bool onlyAlreadyGone(const QVector<WinApi::DeleteResult>& results)
{
    for (const WinApi::DeleteResult& r : results) {
        if (!isClean(r) && r.reason != WinApi::DeleteReason::Missing)
            return false;
    }
    return true;
}

QString resultLines(const QVector<WinApi::DeleteResult>& results, bool recycled)
{
    QStringList lines;
    for (const WinApi::DeleteResult& r : results) {
        if (isClean(r))
            continue;
        lines << QStringLiteral("\u2022 ") + resultLine(r, recycled);
    }
    return lines.join(QStringLiteral("\n"));
}

QString resultBody(const QVector<WinApi::DeleteResult>& results, bool recycled)
{
    const Tally t = tally(results);
    if (t.bad == 0)
        return QString();
    return I18n::tr(QStringLiteral("op_result.body"),
                    QMap<QString, QString>{{"ok", QString::number(t.ok)},
                                           {"bad", QString::number(t.bad)},
                                           {"lines", resultLines(results, recycled)}});
}

QStringList survivorsOf(const QVector<WinApi::DeleteResult>& results)
{
    QStringList paths;
    for (const WinApi::DeleteResult& r : results) {
        if (!r.gone)
            paths << r.path;
    }
    return paths;
}

QStringList vanishedOf(const QVector<WinApi::DeleteResult>& results)
{
    QStringList paths;
    for (const WinApi::DeleteResult& r : results) {
        if (r.gone)
            paths << r.path;
    }
    return paths;
}

QStringList missingOf(const QVector<WinApi::DeleteResult>& results)
{
    QStringList paths;
    for (const WinApi::DeleteResult& r : results) {
        if (r.reason == WinApi::DeleteReason::Missing)
            paths << r.path;
    }
    return paths;
}

} // namespace OpGuard
