#pragma once

#include <QThread>
#include <QString>
#include <vector>
#include <memory>
#include <atomic>

#include "CleanupTarget.h"
#include "FileNode.h"

// CleanupScanner - background scanner for cleanup targets and large files.
//
// CleanupScanner (QThread subclass) finds junk files and large files.
// Discovers cleanable targets (S/A/B/C levels) under the scan path, populates
// their sizes from the completed directory scan tree, and collects large files
// (>50 MB) for the "Large Files" tab. Emits incremental signals so the UI can
// update in real time without freezing.
class CleanupScanner : public QThread {
    Q_OBJECT
public:
    CleanupScanner(const QString& scanPath,
                   std::shared_ptr<FileNode> rootNode,
                   QObject* parent = nullptr);
    void cancel();

    // Classify a file's danger level (S/A/B/C/D) by its path/name.
    // Used by both CleanupScanner and DuplicateScanner to skip C/D-level files.
    static DangerLevel classifyFileDanger(const QString& path, const QString& name);

    // Returns true if the file should be excluded from large-file and duplicate
    // scanning (system/program extensions like .exe/.dll/.sys, or no extension).
    // Shared between CleanupScanner (large files) and DuplicateScanner.
    static bool isExcludedByExtension(const QString& fileName);

signals:
    void targetScanned(CleanupTarget target);
    void largeFileFound(LargeFile lf);
    void finished(std::vector<CleanupTarget> targets,
                  std::vector<LargeFile> largeFiles,
                  qint64 totalSize, int totalCount,
                  qint64 largeTotal);

protected:
    void run() override;

private:
    QString m_scanPath;
    std::shared_ptr<FileNode> m_rootNode;
    std::atomic<bool> m_cancel{false};

    // Phase 1: discover all cleanable targets under the scan path.
    void discoverTargets(std::vector<CleanupTarget>& out);

    // Phase 2: collect large files (>50 MB) from the scanned tree.
    void scanLargeFiles(std::vector<LargeFile>& out);

    // Helper: build a target if the path exists and is under the scan root.
    CleanupTarget makeTarget(const QString& key, const QString& path,
                             DangerLevel danger, bool isDir,
                             bool checked, bool enabled,
                             const QString& remark = QString());

    // Recursively discover __pycache__ directories (S-level).
    void discoverPycache(const QString& root, std::vector<CleanupTarget>& out,
                         int maxDepth = 4);

    // Discover .tmp/.log/.bak/.old/.cache files in the scan root (S-level).
    void discoverTmpFiles(const QString& root, std::vector<CleanupTarget>& out);

    // Recursively discover node_modules directories (A-level).
    void discoverNodeModules(const QString& root, std::vector<CleanupTarget>& out,
                             int maxDepth = 3);

    // Discover .pyc/.pyo files in the scan root (A-level).
    void discoverCompiledPy(const QString& root, std::vector<CleanupTarget>& out);

    // Discover large archive files (>100 MB) in the scan root (B-level).
    void discoverLargeArchives(const QString& root, std::vector<CleanupTarget>& out);

    // Populate target.size / target.fileCount from the FileNode tree.
    void scanTargetsFromTree(std::vector<CleanupTarget>& targets);

    // Check if a path exists and is accessible.
    bool pathExistsAndAccessible(const QString& path) const;

    // Compute the total size and file count of a directory tree.
    qint64 computePathSize(const QString& path) const;
    int countFiles(const QString& path) const;

    // Path normalization helpers (_norm / _path_under).
    static QString normalizePath(const QString& path);
    static bool pathUnder(const QString& path, const QString& rootNorm);
};
