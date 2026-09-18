#pragma once

#include <QThread>
#include <QString>
#include <QFileInfo>
#include <atomic>
#include <vector>

class SafeMoveWorker : public QThread {
    Q_OBJECT
public:
    SafeMoveWorker(const QString& sourcePath, const QString& targetDir, QObject* parent = nullptr);

    void cancel();
    bool isSuccess() const { return m_success; }
    QString errorString() const { return m_errorString; }
    QString finalDestPath() const { return m_finalDestPath; }

signals:
    void progress(int percent, qint64 copiedBytes, qint64 totalBytes, const QString& currentFile);
    void statusMessage(const QString& msg);

protected:
    void run() override;

private:
    struct FileEntry {
        QString relPath;
        QString srcFullPath;
        QString dstFullPath;
        qint64 size = 0;
        bool isDir = false;
    };

    bool collectFiles(const QString& src, const QString& dst);
    bool copyFileWithProgress(const QString& src, const QString& dst, qint64 fileSize);
    bool verifyIntegrity();
    bool removeSource();
    QByteArray calculateFileSha256(const QString& path);

    QString m_sourcePath;
    QString m_targetDir;
    QString m_finalDestPath;
    std::atomic<bool> m_cancelled{false};
    bool m_success = false;
    QString m_errorString;

    std::vector<FileEntry> m_entries;
    qint64 m_totalBytes = 0;
    qint64 m_copiedBytes = 0;
    int m_lastEmittedPct = -1;  // throttle: only emit when the percentage moves
};
