#pragma once

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

#include <QThread>
#include <QString>
#include <QStringList>
#include <QMap>
#include <memory>
#include <atomic>
#include <set>
#include "FileNode.h"

// DiskScanner - asynchronous, parallel directory tree scanner.
//
// DiskScanner runs on a QThread; the root's
// immediate subdirectories are scanned concurrently via QtConcurrent so the
// UI stays responsive. Windows reparse points (junctions + symlinks) are
// detected and NOT recursed into, preventing infinite loops in monorepo
// workspaces. Directories matching DEFAULT_SKIP_PATTERNS are size-computed
// quickly without building child FileNode objects.
class DiskScanner : public QThread {
    Q_OBJECT
public:
    explicit DiskScanner(const QString& rootPath, QObject* parent = nullptr);
    void setSkipHeavyDirs(bool skip);
    void cancel();
    static const QStringList DEFAULT_SKIP_PATTERNS;

signals:
    // key: i18n key (e.g. "disk.progress.scanning")
    // args: placeholder values for I18n::tr(key, args)
    void progress(const QString& key, const QMap<QString, QString>& args);
    void finishedTree(std::shared_ptr<FileNode> root);
    void error(const QString& key, const QMap<QString, QString>& args);

protected:
    void run() override;

private:
    QString m_rootPath;
    std::atomic<bool> m_cancel{false};
    std::set<QString> m_skipSet;
    int m_maxWorkers;

    std::shared_ptr<FileNode> scanRoot(const QString& path);
    std::shared_ptr<FileNode> scanDirRecursive(const QString& path);
    std::shared_ptr<FileNode> makeDirNode(const QString& path, bool top);
    std::shared_ptr<FileNode> makeSkippedDirNode(const QString& name, const QString& path);
    bool shouldSkip(const QString& name) const;
    bool isReparsePoint(DWORD fileAttributes, const QString& path) const;
    void computeDirSizeFast(const QString& path, qint64& size, int& fileCount, int& dirCount);
};
