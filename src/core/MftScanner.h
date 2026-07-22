#pragma once

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

#include <QThread>
#include <QString>
#include <QMap>
#include <memory>
#include <atomic>
#include <set>
#include <unordered_map>
#include <vector>
#include "FileNode.h"

class MftScanner : public QThread {
    Q_OBJECT
public:
    explicit MftScanner(const QString& rootPath, QObject* parent = nullptr);
    void setSkipHeavyDirs(bool skip);
    void cancel();

    static bool isSupported(const QString& path);
    static const QStringList DEFAULT_SKIP_PATTERNS;

signals:
    // key: i18n key (e.g. "mft.progress.enumerating_usn")
    // args: placeholder values for I18n::tr(key, args)
    void progress(const QString& key, const QMap<QString, QString>& args);
    void finishedTree(std::shared_ptr<FileNode> root);
    void error(const QString& key, const QMap<QString, QString>& args);

protected:
    void run() override;

private:
    struct MftEntry {
        uint64_t fileRefNum = 0;
        uint64_t parentRefNum = 0;
        QString name;
        DWORD attributes = 0;
        qint64 size = 0;
        bool isDir = false;
        bool isSymlink = false;
        bool isHidden = false;
        bool skipped = false;
    };

    QString m_rootPath;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_lowMemory{false};  // P0c: set when MemoryMonitor triggers
    std::set<QString> m_skipSet;

    HANDLE openVolume(const QString& path);
    bool getNtfsVolumeData(HANDLE hVolume, NTFS_VOLUME_DATA_BUFFER& data);
    bool enumUsnData(HANDLE hVolume, std::vector<MftEntry>& entries, QString& errorMsg);
    bool getFileSizes(HANDLE hVolume, const NTFS_VOLUME_DATA_BUFFER& volData,
                      std::unordered_map<uint64_t, qint64>& sizes, QString& errorMsg);
    // entries and sizes are mutable: buildTree sorts entries in place by
    // fileRefNum (eliminating a separate entryIndex), and frees sizes early
    // after the node-creation loop.
    std::shared_ptr<FileNode> buildTree(std::vector<MftEntry>& entries, const QString& scanPath,
                                         std::unordered_map<uint64_t, qint64>& sizes);
    bool shouldSkip(const QString& name) const;
    bool isReparsePoint(DWORD fileAttributes) const;
};
