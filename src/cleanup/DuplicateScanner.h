#pragma once

#include <QThread>
#include <QString>
#include <vector>
#include <memory>
#include <atomic>

#include "CleanupTarget.h"  // DangerLevel
#include "FileNode.h"

// DuplicateFile - one file instance inside a duplicate group.
struct DuplicateFile {
    QString path;
    QString name;
    qint64 size = 0;
    DangerLevel danger = DangerLevel::B;  // default: confirm before delete
};

// DuplicateGroup - a set of ≥2 files with identical content.
struct DuplicateGroup {
    qint64 size = 0;                       // per-file size
    std::vector<DuplicateFile> files;      // ≥2 entries
    qint64 wastedSpace() const { return size * (static_cast<qint64>(files.size()) - 1); }
};

// Register metatypes for cross-thread queued signal connections.
// Without these, signals carrying DuplicateGroup / vector<DuplicateGroup>
// silently fail and the UI tree stays empty.
Q_DECLARE_METATYPE(DuplicateFile)
Q_DECLARE_METATYPE(DuplicateGroup)
Q_DECLARE_METATYPE(std::vector<DuplicateGroup>)

// DuplicateScanner - background scanner for duplicate files.
//
// Uses the standard three-stage funnel algorithm (same as Czkawka / jdupes /
// fclones) to find files with identical content:
//   1. Group by size (free, reuses FileNode tree)
//   2. Group by SHA256 of first 4 KB (small I/O, eliminates most non-dupes)
//   3. Group by full-content SHA256 (only runs on the few survivors)
//
// Files below MIN_DUP_SIZE (1 MB) are skipped to avoid noise from .git objects
// and similar tiny files. C/D-level files (system / user data) are skipped via
// CleanupScanner::classifyFileDanger. Hash results live only in run() locals —
// FileNode is not mutated.
class DuplicateScanner : public QThread {
    Q_OBJECT
public:
    DuplicateScanner(const QString& scanPath,
                     std::shared_ptr<FileNode> rootNode,
                     QObject* parent = nullptr);
    void cancel();

signals:
    void groupFound(DuplicateGroup group);
    void progress(int phase, int processed, int total);
    void finished(std::vector<DuplicateGroup> groups,
                  qint64 totalWasted, int totalFiles);

protected:
    void run() override;

private:
    QString m_scanPath;
    std::shared_ptr<FileNode> m_rootNode;
    std::atomic<bool> m_cancel{false};

    static constexpr qint64 MIN_DUP_SIZE = 1LL * 1024 * 1024;      // 1 MB
    static constexpr qint64 PARTIAL_HASH_BYTES = 4LL * 1024;       // first 4 KB
    static constexpr qint64 FULL_HASH_BUFSIZE = 4LL * 1024 * 1024; // 4 MB stream

    // Stage 1: walk FileNode tree, group files by size (≥1 MB, skip C/D level).
    void groupBySize(std::vector<std::vector<std::shared_ptr<FileNode>>>& out);

    // Stage 2: SHA256 of the first PARTIAL_HASH_BYTES of a file.
    QByteArray hashPartial(const QString& path);

    // Stage 3: SHA256 of the full file content (streamed).
    QByteArray hashFull(const QString& path);
};
