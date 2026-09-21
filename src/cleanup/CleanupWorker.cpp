#include "CleanupWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCoreApplication>
#include <algorithm>
#include <stack>
#include <vector>

#include "WinApi.h"
#include "Logger.h"

// ---------------------------------------------------------------------------
// Default constants
// ---------------------------------------------------------------------------
namespace {
constexpr qint64 LARGE_ARCHIVE_MIN_SIZE = 100LL * 1024 * 1024;  // 100 MB

// The two "already gone" codes Windows reports, spelled out because this file
// builds without <windows.h> in the probe lab.
constexpr quint32 kErrorFileNotFound = 2;

const QStringList& tmpFileSuffixes()
{
    static const QStringList s = {".tmp", ".log", ".bak", ".old", ".cache"};
    return s;
}

const QStringList& pycSuffixes()
{
    static const QStringList s = {".pyc", ".pyo"};
    return s;
}

const QStringList& archiveSuffixes()
{
    static const QStringList s = {".zip", ".rar", ".7z", ".tar", ".iso",
                                  ".msi", ".dmg", ".tgz", ".xz"};
    return s;
}

// All cleanup deletions honor the selected DeleteMode: Permanent (direct
// delete, bypassing the recycle bin) or RecycleBin (moved to the recycle bin
// via SHFileOperationW). The Recycle-Bin "empty recycle bin" target is
// inherently permanent regardless of the mode. deletePermanent continues on
// per-file errors so one failure does not stop the rest.
// C/D-level: never reached (caller filters them out).

// Compute directory size before deletion (for freed-byte accounting).
qint64 dirSize(const QString& path)
{
    qint64 total = 0;
    QDir dir(path);
    const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& e : entries) {
        if (e.isDir())
            total += dirSize(e.absoluteFilePath());
        else
            total += e.size();
    }
    return total;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / cancel
// ---------------------------------------------------------------------------

CleanupWorker::CleanupWorker(const std::vector<ItemRef>& items,
                             const std::vector<CleanupTarget>& allTargets,
                             const std::vector<LargeFile>& allLargeFiles,
                             DeleteMode mode,
                             QObject* parent)
    : QThread(parent)
    , m_items(items)
    , m_allTargets(allTargets)
    , m_allLargeFiles(allLargeFiles)
    , m_mode(mode)
{
    // Cache the normalized directory of the running executable so we can
    // refuse to delete it (prevents the "app deleted itself" bug).
    const QString appExe = QCoreApplication::applicationFilePath();
    if (!appExe.isEmpty()) {
        QString dir = QFileInfo(appExe).absolutePath();
        dir = dir.toLower();
        dir.replace('\\', '/');
        while (dir.endsWith('/'))
            dir.chop(1);
        m_appDirNorm = dir;
    }
}

void CleanupWorker::cancel()
{
    m_cancel = true;
}

bool CleanupWorker::removePath(const QString& path, FirstFailure* fail) const
{
    const QStringList paths{path};
    if (m_mode == DeleteMode::RecycleBin) {
        int shellCode = 0;
        const bool ok = WinApi::sendToRecycleBin(paths, &shellCode);
        if (!ok && fail)
            fail->note(WinApi::classifyShellError(shellCode), path,
                       static_cast<quint32>(shellCode));
        return ok;
    }

    const QVector<WinApi::DeleteResult> results = WinApi::deletePermanentDetailed(paths);
    if (results.isEmpty())
        return false;
    const WinApi::DeleteResult& r = results.first();
    // A path that was already gone is not a file this run removed, so it counts
    // as skipped — and that is also where its reason comes from, which is the
    // whole point: "已清理 0 项" used to be the only thing said about a folder
    // that had been moved away an hour earlier.
    const bool removed = r.gone && r.reason != WinApi::DeleteReason::Missing;
    if (!removed && fail) {
        fail->note(r.reason == WinApi::DeleteReason::None ? WinApi::DeleteReason::Unknown
                                                          : r.reason,
                   path, r.winError);
    }
    return removed;
}

bool CleanupWorker::isApplicationPath(const QString& path) const
{
    if (m_appDirNorm.isEmpty())
        return false;
    QString p = QFileInfo(path).absoluteFilePath().toLower();
    p.replace('\\', '/');
    while (p.endsWith('/'))
        p.chop(1);
    // Refuse to delete the app's own directory OR anything inside it.
    return p == m_appDirNorm || p.startsWith(m_appDirNorm + '/');
}

// --------------------------------------------------------------------------- //
// Virtual-group cleaners
// --------------------------------------------------------------------------- //

void CleanupWorker::cleanDownloadsFiles(const QString& root, Outcome& out,
                                        FirstFailure& fail)
{
    // Delete individual FILES inside the Downloads folder, but never the
    // folder itself. This prevents the "entire Downloads deleted" bug where
    // the user's installer files / portable apps were destroyed.

    // Collect files first (post-order), then remove now-empty subdirectories.
    // This keeps the Downloads folder itself but cleans up empty subfolders.
    std::stack<QString> pending;
    pending.push(root);
    std::vector<QString> files;
    std::vector<QString> subdirs;  // collected for post-order cleanup

    while (!pending.empty()) {
        if (m_cancel.load())
            return;
        const QString cur = pending.top();
        pending.pop();
        QDir dir(cur);
        const auto entries = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& e : entries) {
            if (e.isDir()) {
                pending.push(e.absoluteFilePath());
                subdirs.push_back(e.absoluteFilePath());
            } else {
                files.push_back(e.absoluteFilePath());
            }
        }
    }

    // Delete files.
    for (const QString& fp : files) {
        if (m_cancel.load())
            return;
        // Safety: never delete the app's own files.
        if (isApplicationPath(fp)) {
            ++out.skipped;
            continue;
        }
        if (removePath(fp, &fail)) {
            ++out.deleted;
        } else {
            Logger::warn(QStringLiteral("[cleanDownloads] DELETE FAILED: %1").arg(fp));
            ++out.skipped;
        }
    }

    // Remove now-empty subdirectories (deepest first), but never the root.
    // Only remove if empty — non-empty dirs (e.g. containing locked files)
    // are left intact so we don't force-delete anything the user might want.
    std::sort(subdirs.begin(), subdirs.end(),
              [](const QString& a, const QString& b) {
                  return a.length() > b.length();  // deepest first
              });
    for (const QString& sd : subdirs) {
        if (m_cancel.load())
            return;
        QDir d(sd);
        d.rmdir(sd);  // only succeeds if empty; ignores failure silently
    }
}

void CleanupWorker::cleanTmpFilesInRoot(const QString& root, Outcome& out,
                                        FirstFailure& fail)
{
    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Files);
    for (const auto& entry : entries) {
        if (m_cancel.load())
            return;
        const QString nameL = entry.fileName().toLower();
        bool match = false;
        for (const auto& suf : tmpFileSuffixes()) {
            if (nameL.endsWith(suf)) {
                match = true;
                break;
            }
        }
        if (!match)
            continue;

        const QString path = entry.absoluteFilePath();
        // tmp/log/bak files are S-level.
        bool ok = removePath(path, &fail);
        if (ok)
            ++out.deleted;
        else
            ++out.skipped;
    }
}

void CleanupWorker::cleanPycFilesInRoot(const QString& root, Outcome& out,
                                        FirstFailure& fail)
{
    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Files);
    for (const auto& entry : entries) {
        if (m_cancel.load())
            return;
        const QString nameL = entry.fileName().toLower();
        bool match = false;
        for (const auto& suf : pycSuffixes()) {
            if (nameL.endsWith(suf)) {
                match = true;
                break;
            }
        }
        if (!match)
            continue;

        const QString path = entry.absoluteFilePath();
        // .pyc/.pyo are A-level cache files.
        bool ok = removePath(path, &fail);
        if (ok)
            ++out.deleted;
        else
            ++out.skipped;
    }
}

void CleanupWorker::cleanLargeArchivesInRoot(const QString& root, Outcome& out,
                                             FirstFailure& fail)
{
    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Files);
    for (const auto& entry : entries) {
        if (m_cancel.load())
            return;
        const QString nameL = entry.fileName().toLower();
        bool match = false;
        if (nameL.endsWith(".tar.gz"))
            match = true;
        if (!match) {
            for (const auto& suf : archiveSuffixes()) {
                if (nameL.endsWith(suf)) {
                    match = true;
                    break;
                }
            }
        }
        if (!match)
            continue;

        if (entry.size() <= LARGE_ARCHIVE_MIN_SIZE)
            continue;

        const QString path = entry.absoluteFilePath();
        // Large archives are B-level.
        bool ok = removePath(path, &fail);
        if (ok)
            ++out.deleted;
        else
            ++out.skipped;
    }
}

// ---------------------------------------------------------------------------
// cleanTarget — clean a single CleanupTarget
// ---------------------------------------------------------------------------

void CleanupWorker::cleanTarget(const CleanupTarget& target, Outcome& out)
{
    FirstFailure fail;

    // Safety gate: never clean disabled targets or C/D-level targets.
    if (!target.enabled ||
        target.danger == DangerLevel::C ||
        target.danger == DangerLevel::D) {
        Logger::warn(QStringLiteral("[cleanTarget] SKIPPED (disabled or C/D level): %1").arg(target.key));
        return;
    }

    // Safety gate: never delete the running application's own directory.
    // This is a hard backstop — even if a target path somehow points at the
    // app's install folder (e.g. user scanned Program Files), refuse here.
    if (isApplicationPath(target.path)) {
        Logger::warn(QStringLiteral("[cleanTarget] SKIPPED (app path): %1").arg(target.path));
        out.skipped = 1;
        return;
    }

    // Safety gate: the Windows directory itself is off limits. Its contents are
    // what the cleanup categories are for, and they are never addressed as the
    // directory itself.
    if (WinApi::isWindowsRoot(target.path)) {
        Logger::warn(QStringLiteral("[cleanTarget] SKIPPED (protected root): %1").arg(target.path));
        out.skipped = 1;
        return;
    }

    // Virtual groups — delete individual files inside the root.
    if (target.key == "cleanup.s_tmp_files") {
        cleanTmpFilesInRoot(target.path, out, fail);
        out.freed = (out.deleted > 0) ? target.size : 0;
    } else if (target.key == "cleanup.a_pyc") {
        cleanPycFilesInRoot(target.path, out, fail);
        out.freed = (out.deleted > 0) ? target.size : 0;
    } else if (target.key == "cleanup.b_large_archives") {
        cleanLargeArchivesInRoot(target.path, out, fail);
        out.freed = (out.deleted > 0) ? target.size : 0;
    } else if (target.key == "cleanup.b_downloads") {
        // Downloads — virtual group: delete individual files, keep the folder.
        cleanDownloadsFiles(target.path, out, fail);
        out.freed = (out.deleted > 0) ? target.size : 0;
    } else if (target.key == "cleanup.b_recycle") {
        // Recycle bin — use SHEmptyRecycleBinW, not deletePermanent.
        // deletePermanent on $Recycle.Bin would corrupt the shell's recycle bin
        // metadata and leave orphaned entries.
        // Extract drive root from the path (e.g. "C:/$Recycle.Bin" → "C:/")
        QString driveRoot;
        if (target.path.length() >= 2 && target.path[1] == ':')
            driveRoot = target.path.left(2) + QStringLiteral("/");
        else
            driveRoot = target.path;
        if (WinApi::emptyRecycleBin(driveRoot)) {
            out.deleted = 1;
            out.freed = target.size;
        } else {
            out.skipped = 1;
        }
    } else {
        // Real paths — clean directory contents (keep the dir) or delete a file.
        const QFileInfo info(target.path);
        if (!info.exists()) {
            // The cleanup list was built by an earlier scan; the folder can be
            // gone by the time the button is pressed (another tool cleaned it,
            // the user moved it). Counting that as a silent skip is what made
            // "已清理 0 项" a dead end, so the reason travels with it.
            out.skipped = 1;
            fail.note(WinApi::DeleteReason::Missing, target.path, kErrorFileNotFound);
        } else if (info.isDir()) {
            // For directory targets (Temp, Logs, caches, etc.): delete the
            // contents but KEEP the directory itself. System directories like
            // C:\Windows\Temp and AppData\Local\Temp must not be removed —
            // doing so causes system/app malfunctions. Locked files are skipped.
            // In RecycleBin mode the contents are moved to the Recycle Bin.
            int shellCode = 0;
            WinApi::DeleteReason why = WinApi::DeleteReason::None;
            QString sample;
            const std::pair<int, int> res =
                (m_mode == DeleteMode::RecycleBin)
                    ? WinApi::cleanDirectoryContentsToRecycleBin(target.path, &shellCode)
                    : WinApi::cleanDirectoryContents(target.path, &why, &sample);
            out.deleted = res.first;
            out.skipped = res.second;
            if (out.skipped > 0) {
                if (m_mode == DeleteMode::RecycleBin) {
                    fail.note(WinApi::classifyShellError(shellCode), target.path,
                              static_cast<quint32>(shellCode));
                } else {
                    fail.note(why == WinApi::DeleteReason::None ? WinApi::DeleteReason::Unknown
                                                                : why,
                              sample, 0);
                }
            }
            out.freed = (out.deleted > 0) ? target.size : 0;
        } else {
            // Single file target — delete/move per mode.
            out.freed = target.size;
            bool ok = removePath(target.path, &fail);
            if (ok && !QFileInfo::exists(target.path)) {
                out.deleted = 1;
            } else {
                out.skipped = 1;
                out.freed = 0;
            }
        }
    }

    out.reason = fail.reason;
    out.sample = fail.sample;
    out.winError = fail.winError;
}

// ---------------------------------------------------------------------------
// cleanLargeFile — clean a single LargeFile
// ---------------------------------------------------------------------------

void CleanupWorker::cleanLargeFile(const LargeFile& lf, Outcome& out)
{
    FirstFailure fail;

    // Safety gate: never delete the running application's own files.
    if (isApplicationPath(lf.path)) {
        out.skipped = 1;
        return;
    }

    // Safety gate: same protection as for targets — the Windows directory itself
    // is never removed as a "large file".
    if (WinApi::isWindowsRoot(lf.path)) {
        Logger::warn(QStringLiteral("[cleanLargeFile] SKIPPED (protected root): %1").arg(lf.path));
        out.skipped = 1;
        return;
    }

    const QFileInfo info(lf.path);
    if (!info.exists()) {
        // Same as a target: the file list is from an earlier scan.
        out.skipped = 1;
        fail.note(WinApi::DeleteReason::Missing, lf.path, kErrorFileNotFound);
        out.reason = fail.reason;
        out.sample = fail.sample;
        out.winError = fail.winError;
        return;
    }

    bool ok = false;

    if (info.isFile()) {
        out.freed = info.size();
        ok = removePath(lf.path, &fail);
    } else if (info.isDir()) {
        out.freed = dirSize(lf.path);
        ok = removePath(lf.path, &fail);
    }

    if (ok && !QFileInfo::exists(lf.path)) {
        out.deleted = 1;
    } else {
        out.skipped = 1;
        out.freed = 0;
    }

    out.reason = fail.reason;
    out.sample = fail.sample;
    out.winError = fail.winError;
}

// ---------------------------------------------------------------------------
// Main thread entry point (CleanupWorker.run)
// ---------------------------------------------------------------------------

void CleanupWorker::run()
{
    int totalDeleted = 0;
    int totalSkipped = 0;
    qint64 totalFreed = 0;
    const int totalItems = static_cast<int>(m_items.size());
    int processed = 0;
    std::vector<ItemRef> successItems;
    std::vector<ItemRef> failedItems;

    for (const auto& item : m_items) {
        if (m_cancel.load())
            break;

        if (item.type == "target") {
            // Find the matching target by (key, path).
            const CleanupTarget* matched = nullptr;
            for (const auto& t : m_allTargets) {
                if (t.key == item.key && t.path == item.path) {
                    matched = &t;
                    break;
                }
            }
            if (!matched)
                continue;

            emit progress(item.key);
            emit itemStarted(++processed, totalItems, item.key);

            Outcome out;
            cleanTarget(*matched, out);

            totalDeleted += out.deleted;
            totalSkipped += out.skipped;
            totalFreed += out.freed;

            emit itemDone(item.key, out.deleted, out.skipped, out.freed);

            // The report travels with the item, so the summary can say why this
            // one is still on the disk instead of only that it is.
            ItemRef done = item;
            done.reason = out.reason;
            done.reasonPath = out.sample;
            done.winError = out.winError;
            done.skipped = out.skipped;

            // Failure criteria:
            // Most cleanup targets clean many individual files inside a
            // directory (Temp, Logs, caches, Downloads, etc.). A few skipped
            // files (locked .exe, in-use system logs) are NORMAL and should
            // NOT mark the whole category as "failed" — the user would see a
            // scary "cleanup failed" popup even though 99% succeeded.
            // Only mark as failed if NOTHING was deleted at all.
            // Exception: single-file targets (rare) — fail if skipped.
            const bool failed = (out.deleted == 0 && matched->fileCount > 0);
            if (failed)
                failedItems.push_back(done);
            else
                successItems.push_back(done);

        } else if (item.type == "file") {
            // Large-file cleanup.
            const QFileInfo info(item.path);
            const QString label = info.exists()
                ? info.fileName() : item.path;
            emit progress(label);
            emit itemStarted(++processed, totalItems, label);

            // Find the matching LargeFile to get its danger level.
            const LargeFile* matched = nullptr;
            for (const auto& lf : m_allLargeFiles) {
                if (lf.path == item.path) {
                    matched = &lf;
                    break;
                }
            }
            Outcome out;
            if (matched) {
                cleanLargeFile(*matched, out);
            } else {
                // No matching LargeFile — clean directly using B-level
                // defaults (honoring the selected DeleteMode).
                LargeFile lf;
                lf.path = item.path;
                lf.name = info.fileName();
                lf.danger = DangerLevel::B;
                cleanLargeFile(lf, out);
            }

            totalDeleted += out.deleted;
            totalSkipped += out.skipped;
            totalFreed += out.freed;

            emit itemDone(item.path, out.deleted, out.skipped, out.freed);

            ItemRef done = item;
            done.reason = out.reason;
            done.reasonPath = out.sample;
            done.winError = out.winError;
            done.skipped = out.skipped;

            if (out.skipped > 0)
                failedItems.push_back(done);
            else
                successItems.push_back(done);
        }
    }

    emit finished(totalDeleted, totalSkipped, totalFreed, totalItems,
                  successItems, failedItems);
}
