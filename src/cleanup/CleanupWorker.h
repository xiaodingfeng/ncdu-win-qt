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
    // A single item reference for processing. type is "target" or "file".
    struct ItemRef {
        QString type;  // "target" or "file"
        QString key;
        QString path;
    };

    CleanupWorker(const std::vector<ItemRef>& items,
                  const std::vector<CleanupTarget>& allTargets,
                  const std::vector<LargeFile>& allLargeFiles,
                  QObject* parent = nullptr);
    void cancel();

signals:
    void progress(const QString& label);
    void itemDone(const QString& key, int deleted, int skipped, qint64 freed);
    void finished(int totalDeleted, int totalSkipped, qint64 totalFreed,
                  int totalItems,
                  std::vector<ItemRef> successItems,
                  std::vector<ItemRef> failedItems);

protected:
    void run() override;

private:
    std::vector<ItemRef> m_items;
    std::vector<CleanupTarget> m_allTargets;
    std::vector<LargeFile> m_allLargeFiles;
    std::atomic<bool> m_cancel{false};

    // Clean a single CleanupTarget. Returns (deleted, skipped, freed).
    void cleanTarget(const CleanupTarget& target,
                     int& deleted, int& skipped, qint64& freed);

    // Clean a single LargeFile. Returns (deleted, skipped, freed).
    void cleanLargeFile(const LargeFile& lf,
                        int& deleted, int& skipped, qint64& freed);

    // Delete .tmp/.log/.bak/.old/.cache files in a root directory.
    void cleanTmpFilesInRoot(const QString& root,
                             int& deleted, int& skipped);

    // Delete .pyc/.pyo files in a root directory.
    void cleanPycFilesInRoot(const QString& root,
                             int& deleted, int& skipped);

    // Delete large archive files (>100 MB) in a root directory.
    void cleanLargeArchivesInRoot(const QString& root,
                                  int& deleted, int& skipped);

    // Delete individual files inside the Downloads folder (not the folder
    // itself). Prevents the "entire Downloads deleted" bug.
    void cleanDownloadsFiles(const QString& root,
                             int& deleted, int& skipped);

    // Safety gate: returns true if *path* is the running application's own
    // executable or its install directory. Deleting such paths would break
    // the application (the "downloaded installer deleted itself" bug).
    bool isApplicationPath(const QString& path) const;

    // Cached normalized path of the running executable's directory.
    QString m_appDirNorm;
};
