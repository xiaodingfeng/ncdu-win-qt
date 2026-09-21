#pragma once

#include <QThread>
#include <QString>
#include <QStringList>
#include <vector>
#include <atomic>

#include "CleanupTarget.h"
#include "WinApi.h"

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
    //
    // Everything after *path* is filled in by the worker on the way out and read
    // back from the successItems / failedItems the caller is handed. They are
    // what lets the cleanup summary name the cause instead of listing an item and
    // leaving the user to guess — "已清理 0 项" with no reason was a dead end.
    struct ItemRef {
        QString type;  // "target" or "file"
        QString key;
        QString path;
        // Why what was left behind was left behind. None when nothing was.
        WinApi::DeleteReason reason = WinApi::DeleteReason::None;
        QString reasonPath;   // one example of what was left, when there is one
        quint32 winError = 0;
        int skipped = 0;      // how many entries stayed behind
    };

    // What cleaning one item produced. Bundled because it grew past the point
    // where four out-parameters stayed readable.
    struct Outcome {
        int deleted = 0;
        int skipped = 0;
        qint64 freed = 0;
        WinApi::DeleteReason reason = WinApi::DeleteReason::None;
        QString sample;
        quint32 winError = 0;
    };

    // Remembers the FIRST thing that went wrong while cleaning one item. Later
    // ones are almost always the same cause repeated: a Temp folder with four
    // hundred files open in one program is one sentence, not four hundred.
    struct FirstFailure {
        WinApi::DeleteReason reason = WinApi::DeleteReason::None;
        QString sample;
        quint32 winError = 0;

        bool has() const { return reason != WinApi::DeleteReason::None; }

        void note(WinApi::DeleteReason why, const QString& path, quint32 err)
        {
            if (reason != WinApi::DeleteReason::None || why == WinApi::DeleteReason::None)
                return;
            reason = why;
            sample = path;
            winError = err;
        }
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
    // *fail*, when given, records why the first refusal happened.
    bool removePath(const QString& path, FirstFailure* fail = nullptr) const;
    void cleanTmpFilesInRoot(const QString& root, Outcome& out, FirstFailure& fail);
    void cleanPycFilesInRoot(const QString& root, Outcome& out, FirstFailure& fail);
    void cleanLargeArchivesInRoot(const QString& root, Outcome& out, FirstFailure& fail);
    void cleanDownloadsFiles(const QString& root, Outcome& out, FirstFailure& fail);
    void cleanTarget(const CleanupTarget& target, Outcome& out);
    void cleanLargeFile(const LargeFile& lf, Outcome& out);
    bool isApplicationPath(const QString& path) const;

    std::vector<ItemRef> m_items;
    std::vector<CleanupTarget> m_allTargets;
    std::vector<LargeFile> m_allLargeFiles;
    DeleteMode m_mode = DeleteMode::Permanent;
    QString m_appDirNorm;
    std::atomic<bool> m_cancel{false};
};
