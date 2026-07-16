#include "CleanupWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>

#include "WinApi.h"

// ---------------------------------------------------------------------------
// Default constants
// ---------------------------------------------------------------------------
namespace {
constexpr qint64 LARGE_ARCHIVE_MIN_SIZE = 100LL * 1024 * 1024;  // 100 MB

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

// All cleanup deletions use permanent delete (direct delete, bypassing the
// recycle bin) for speed and reliability. SHFileOperationW has high per-call
// shell overhead and can stall the worker on locked files. deletePermanent
// continues on per-file errors so one failure does not stop the rest.
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
                             QObject* parent)
    : QThread(parent)
    , m_items(items)
    , m_allTargets(allTargets)
    , m_allLargeFiles(allLargeFiles)
{
}

void CleanupWorker::cancel()
{
    m_cancel = true;
}

// ---------------------------------------------------------------------------
// Virtual-group cleaners
// ---------------------------------------------------------------------------

void CleanupWorker::cleanTmpFilesInRoot(const QString& root,
                                        int& deleted, int& skipped)
{
    deleted = 0;
    skipped = 0;
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
        const QStringList paths{path};
        // tmp/log/bak files are S-level — permanent delete.
        bool ok = WinApi::deletePermanent(paths);
        if (ok)
            ++deleted;
        else
            ++skipped;
    }
}

void CleanupWorker::cleanPycFilesInRoot(const QString& root,
                                        int& deleted, int& skipped)
{
    deleted = 0;
    skipped = 0;
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
        const QStringList paths{path};
        // .pyc/.pyo are A-level cache files — permanent delete.
        bool ok = WinApi::deletePermanent(paths);
        if (ok)
            ++deleted;
        else
            ++skipped;
    }
}

void CleanupWorker::cleanLargeArchivesInRoot(const QString& root,
                                             int& deleted, int& skipped)
{
    deleted = 0;
    skipped = 0;
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
        const QStringList paths{path};
        // Large archives are B-level — permanent delete.
        bool ok = WinApi::deletePermanent(paths);
        if (ok)
            ++deleted;
        else
            ++skipped;
    }
}

// ---------------------------------------------------------------------------
// cleanTarget — clean a single CleanupTarget
// ---------------------------------------------------------------------------

void CleanupWorker::cleanTarget(const CleanupTarget& target,
                                int& deleted, int& skipped, qint64& freed)
{
    deleted = 0;
    skipped = 0;
    freed = 0;

    // Safety gate: never clean disabled targets or C/D-level targets.
    if (!target.enabled ||
        target.danger == DangerLevel::C ||
        target.danger == DangerLevel::D) {
        return;
    }

    // Virtual groups — delete individual files inside the root.
    if (target.key == "cleanup.s_tmp_files") {
        cleanTmpFilesInRoot(target.path, deleted, skipped);
        freed = (deleted > 0) ? target.size : 0;
        return;
    }
    if (target.key == "cleanup.a_pyc") {
        cleanPycFilesInRoot(target.path, deleted, skipped);
        freed = (deleted > 0) ? target.size : 0;
        return;
    }
    if (target.key == "cleanup.b_large_archives") {
        cleanLargeArchivesInRoot(target.path, deleted, skipped);
        freed = (deleted > 0) ? target.size : 0;
        return;
    }

    // Real paths — permanent delete (direct, bypass recycle bin).
    const QFileInfo info(target.path);
    if (!info.exists()) {
        skipped = 1;
        return;
    }

    freed = target.size;
    const QStringList paths{target.path};
    bool ok = WinApi::deletePermanent(paths);

    if (ok && !QFileInfo::exists(target.path)) {
        deleted = 1;
    } else {
        skipped = 1;
        freed = 0;
    }
}

// ---------------------------------------------------------------------------
// cleanLargeFile — clean a single LargeFile
// ---------------------------------------------------------------------------

void CleanupWorker::cleanLargeFile(const LargeFile& lf,
                                   int& deleted, int& skipped, qint64& freed)
{
    deleted = 0;
    skipped = 0;
    freed = 0;

    const QFileInfo info(lf.path);
    if (!info.exists()) {
        skipped = 1;
        return;
    }

    const QStringList paths{lf.path};
    bool ok = false;

    if (info.isFile()) {
        freed = info.size();
        // Large files — permanent delete (direct, bypass recycle bin).
        ok = WinApi::deletePermanent(paths);
    } else if (info.isDir()) {
        freed = dirSize(lf.path);
        ok = WinApi::deletePermanent(paths);
    }

    if (ok && !QFileInfo::exists(lf.path)) {
        deleted = 1;
    } else {
        skipped = 1;
        freed = 0;
    }
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

            int d = 0, s = 0;
            qint64 f = 0;
            cleanTarget(*matched, d, s, f);

            totalDeleted += d;
            totalSkipped += s;
            totalFreed += f;

            emit itemDone(item.key, d, s, f);

            if (s > 0 || (d == 0 && matched->fileCount > 0))
                failedItems.push_back(item);
            else
                successItems.push_back(item);

        } else if (item.type == "file") {
            // Large-file cleanup.
            const QFileInfo info(item.path);
            const QString label = info.exists()
                ? info.fileName() : item.path;
            emit progress(label);

            int d = 0, s = 0;
            qint64 f = 0;

            // Find the matching LargeFile to get its danger level.
            const LargeFile* matched = nullptr;
            for (const auto& lf : m_allLargeFiles) {
                if (lf.path == item.path) {
                    matched = &lf;
                    break;
                }
            }
            if (matched) {
                cleanLargeFile(*matched, d, s, f);
            } else {
                // No matching LargeFile — clean directly using B-level
                // defaults (permanent delete).
                LargeFile lf;
                lf.path = item.path;
                lf.name = info.fileName();
                lf.danger = DangerLevel::B;
                cleanLargeFile(lf, d, s, f);
            }

            totalDeleted += d;
            totalSkipped += s;
            totalFreed += f;

            emit itemDone(item.path, d, s, f);

            if (s > 0)
                failedItems.push_back(item);
            else
                successItems.push_back(item);
        }
    }

    emit finished(totalDeleted, totalSkipped, totalFreed, totalItems,
                  successItems, failedItems);
}
