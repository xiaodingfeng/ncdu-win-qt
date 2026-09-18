#pragma once

#include <QThread>
#include <QString>
#include <QStringList>
#include <vector>
#include <atomic>

#include "CleanupTarget.h"

// CleanupWorker - asynchronous cleanup execution thread.
//
// CleanupWorker (QThread subclass) performs the actual file deletion.
// Processes a list of cleanup items (targets and/or large files) one at a
// time on a background thread, emitting progress signals so the UI can update
// in real time without freezing.
//
// ItemRef.type is "target" (matched against allTargets) or "file" (matched
// against allLargeFiles).
class CleanupWorker : public QThread {
    Q_OBJECT
public:
    // How cleaned items are removed: permanently, or moved to the Recycle Bin.
    // (The Recycle-Bin "empty recycle bin" target is inherently permanent and
    // is unaffected by this mode.)
    enum class DeleteMode { Permanent, RecycleBin };

    // A single item reference for processing. type is "target" or "file".
    struct ItemRef {
        QString type;  // "target" or "file"
        QString key;
        QString path;
    };

    CleanupWorker(const std::vector<ItemRef>& items,
                  const std::vector<CleanupTarget>& allTargets,
                  const std::vector<LargeFile>& allLargeFiles,
                  DeleteMode mode,
                  QObject* parent = nullptr);
    void cancel();

signals:
    void progress(const QString& label);
    // Emitted just before an item starts: which item (1-based) of how many,
    // plus a short label for the UI. Gives the panel a real percentage while
    // big targets (Temp, caches) grind through thousands of files.
    void itemStarted(int index, int total, const QString& label);
    void itemDone(const QString& key, int deleted, int skipped, qint64 freed);
    void finished(int totalDeleted, int totalSkipped, qint64 totalFreed,
                  int totalItems,
                  std::vector<ItemRef> successItems,
                  std::vector<ItemRef> failedItems);

protected:
    void run() override;

private:
    // Delete a single path according to m_mode (Recycle Bin or permanent).
    bool removePath(const QString& path) const;
    void cleanTmpFilesInRoot(const QString& root, int& deleted, int& skipped);
    void cleanPycFilesInRoot(const QString& root, int& deleted, int& skipped);
    void cleanLargeArchivesInRoot(const QString& root, int& deleted, int& skipped);
    void cleanDownloadsFiles(const QString& root, int& deleted, int& skipped);
    void cleanTarget(const CleanupTarget& target, int& deleted, int& skipped, qint64& freed);
    void cleanLargeFile(const LargeFile& lf, int& deleted, int& skipped, qint64& freed);
    bool isApplicationPath(const QString& path) const;

    std::vector<ItemRef> m_items;
    std::vector<CleanupTarget> m_allTargets;
    std::vector<LargeFile> m_allLargeFiles;
    DeleteMode m_mode = DeleteMode::Permanent;
    QString m_appDirNorm;
    std::atomic<bool> m_cancel{false};
};
