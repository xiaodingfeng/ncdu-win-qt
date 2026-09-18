#include "SafeMoveWorker.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QCryptographicHash>
#include <QDateTime>
#include "WinApi.h"
#include "I18n.h"

// I18n::tr is called from the worker thread. That is safe here because the
// progress dialog is window-modal, so the language cannot be switched while a
// move is running (which would otherwise rehash the translation tables).
static QString tr1(const char* key, const QString& value)
{
    return I18n::tr(QString::fromLatin1(key), QMap<QString, QString>{{QStringLiteral("value"), value}});
}

SafeMoveWorker::SafeMoveWorker(const QString& sourcePath, const QString& targetDir, QObject* parent)
    : QThread(parent)
    , m_sourcePath(QDir::cleanPath(sourcePath))
    , m_targetDir(QDir::cleanPath(targetDir))
{
}

void SafeMoveWorker::cancel()
{
    m_cancelled = true;
}

void SafeMoveWorker::run()
{
    m_success = false;
    m_errorString.clear();
    m_entries.clear();
    m_totalBytes = 0;
    m_copiedBytes = 0;
    m_lastEmittedPct = -1;

    QFileInfo srcInfo(m_sourcePath);
    if (!srcInfo.exists()) {
        m_errorString = tr1("move.err.no_source", m_sourcePath);
        return;
    }

    QDir targetDirObj(m_targetDir);
    if (!targetDirObj.exists()) {
        if (!targetDirObj.mkpath(QStringLiteral("."))) {
            m_errorString = tr1("move.err.mkdir", m_targetDir);
            return;
        }
    }

    // Determine final destination path
    m_finalDestPath = m_targetDir + QLatin1Char('/') + srcInfo.fileName();
    if (QFileInfo::exists(m_finalDestPath)) {
        m_errorString = tr1("move.err.dst_exists", m_finalDestPath);
        return;
    }

    emit statusMessage(I18n::tr("move.status.counting"));
    if (!collectFiles(m_sourcePath, m_finalDestPath)) {
        if (m_cancelled) m_errorString = I18n::tr("move.err.cancelled");
        return;
    }

    // Step 1: Copy phase
    emit statusMessage(I18n::tr("move.status.copying"));
    for (const auto& entry : m_entries) {
        if (m_cancelled) {
            m_errorString = I18n::tr("move.err.cancelled_src_kept");
            return;
        }

        if (entry.isDir) {
            QDir().mkpath(entry.dstFullPath);
        } else {
            // Ensure parent dir exists
            QFileInfo fi(entry.dstFullPath);
            QDir().mkpath(fi.absolutePath());

            if (!copyFileWithProgress(entry.srcFullPath, entry.dstFullPath, entry.size)) {
                if (m_cancelled) {
                    m_errorString = I18n::tr("move.err.cancelled_copy");
                } else if (m_errorString.isEmpty()) {
                    m_errorString = tr1("move.err.copy_failed", entry.srcFullPath);
                }
                return;
            }
        }
    }

    // Step 2: Verification phase
    emit statusMessage(I18n::tr("move.status.verifying"));
    if (!verifyIntegrity()) {
        if (m_cancelled) {
            m_errorString = I18n::tr("move.err.cancelled_verify");
        } else if (m_errorString.isEmpty()) {
            m_errorString = I18n::tr("move.err.verify_failed");
        }
        return;
    }

    // Step 3: Remove source phase
    emit statusMessage(I18n::tr("move.status.removing"));
    if (!removeSource()) {
        m_errorString = I18n::tr("move.err.remove_failed");
        return;
    }

    m_success = true;
    emit progress(100, m_totalBytes, m_totalBytes, I18n::tr("move.status.done"));
}

bool SafeMoveWorker::collectFiles(const QString& src, const QString& dst)
{
    QFileInfo fi(src);
    if (!fi.isDir()) {
        FileEntry entry;
        entry.relPath = fi.fileName();
        entry.srcFullPath = src;
        entry.dstFullPath = dst;
        entry.size = fi.size();
        entry.isDir = false;
        m_entries.push_back(entry);
        m_totalBytes += entry.size;
        return true;
    }

    // It's a directory: create the root dir entry
    FileEntry rootEntry;
    rootEntry.relPath = QString();
    rootEntry.srcFullPath = src;
    rootEntry.dstFullPath = dst;
    rootEntry.size = 0;
    rootEntry.isDir = true;
    m_entries.push_back(rootEntry);

    QDirIterator it(src, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    int srcLen = src.length();

    while (it.hasNext()) {
        if (m_cancelled) return false;
        it.next();
        QFileInfo info = it.fileInfo();
        QString fullPath = info.absoluteFilePath();
        QString rel = fullPath.mid(srcLen);
        if (rel.startsWith(QLatin1Char('/')) || rel.startsWith(QLatin1Char('\\')))
            rel = rel.mid(1);

        FileEntry entry;
        entry.relPath = rel;
        entry.srcFullPath = fullPath;
        entry.dstFullPath = dst + QLatin1Char('/') + rel;
        entry.isDir = info.isDir();
        entry.size = entry.isDir ? 0 : info.size();

        m_entries.push_back(entry);
        if (!entry.isDir)
            m_totalBytes += entry.size;
    }
    return true;
}

bool SafeMoveWorker::copyFileWithProgress(const QString& src, const QString& dst, qint64 fileSize)
{
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        m_errorString = tr1("move.err.read_failed", src);
        return false;
    }

    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly)) {
        m_errorString = tr1("move.err.write_failed", dst);
        return false;
    }

    constexpr qint64 BUFFER_SIZE = 128 * 1024; // 128 KB chunks
    QByteArray buffer(BUFFER_SIZE, Qt::Uninitialized);

    qint64 fileCopied = 0;
    while (!in.atEnd()) {
        if (m_cancelled) {
            in.close();
            out.close();
            QFile::remove(dst);
            return false;
        }

        qint64 bytesRead = in.read(buffer.data(), BUFFER_SIZE);
        if (bytesRead <= 0) break;

        qint64 bytesWritten = out.write(buffer.constData(), bytesRead);
        if (bytesWritten != bytesRead) {
            m_errorString = tr1("move.err.write_disk", dst);
            in.close();
            out.close();
            QFile::remove(dst);
            return false;
        }

        fileCopied += bytesWritten;
        m_copiedBytes += bytesWritten;

        // Throttle: the copy loop runs on 128 KB chunks, so emitting on every
        // chunk would flood the main thread's event queue for multi-GB files.
        int pct = m_totalBytes > 0 ? static_cast<int>(m_copiedBytes * 100 / m_totalBytes) : 0;
        if (pct != m_lastEmittedPct) {
            m_lastEmittedPct = pct;
            emit progress(pct, m_copiedBytes, m_totalBytes, QFileInfo(src).fileName());
        }
    }

    // Always push a final update so the label shows the file that just finished
    // even when the overall percentage did not change.
    int finalPct = m_totalBytes > 0 ? static_cast<int>(m_copiedBytes * 100 / m_totalBytes) : 100;
    emit progress(finalPct, m_copiedBytes, m_totalBytes, QFileInfo(src).fileName());

    out.flush();
    in.close();
    out.close();

    // Preserve modification time
    QDateTime modTime = QFileInfo(src).lastModified();
    if (modTime.isValid())
        out.setFileTime(modTime, QFileDevice::FileModificationTime);

    return true;
}

QByteArray SafeMoveWorker::calculateFileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 CHUNK_SIZE = 256 * 1024;
    QByteArray buffer(CHUNK_SIZE, Qt::Uninitialized);

    while (!file.atEnd()) {
        if (m_cancelled) return QByteArray();
        qint64 bytesRead = file.read(buffer.data(), CHUNK_SIZE);
        if (bytesRead > 0)
            hash.addData(buffer.constData(), bytesRead);
    }
    return hash.result();
}

bool SafeMoveWorker::verifyIntegrity()
{
    int index = 0;
    int total = static_cast<int>(m_entries.size());

    for (const auto& entry : m_entries) {
        if (m_cancelled) return false;
        index++;

        if (entry.isDir) {
            if (!QFileInfo::exists(entry.dstFullPath)) {
                m_errorString = tr1("move.err.missing_dir", entry.dstFullPath);
                return false;
            }
            continue;
        }

        QFileInfo dstFi(entry.dstFullPath);
        if (!dstFi.exists()) {
            m_errorString = tr1("move.err.missing_file", entry.dstFullPath);
            return false;
        }

        if (dstFi.size() != entry.size) {
            m_errorString = tr1("move.err.size_mismatch", entry.srcFullPath);
            return false;
        }

        // Verify SHA-256
        emit progress(100, m_totalBytes, m_totalBytes,
            I18n::tr("move.status.verify_item", QMap<QString, QString>{
                {"index", QString::number(index)},
                {"total", QString::number(total)},
                {"name", QFileInfo(entry.srcFullPath).fileName()}}));
        QByteArray srcHash = calculateFileSha256(entry.srcFullPath);
        if (m_cancelled) return false;
        QByteArray dstHash = calculateFileSha256(entry.dstFullPath);
        if (m_cancelled) return false;

        if (srcHash.isEmpty() || dstHash.isEmpty() || srcHash != dstHash) {
            m_errorString = tr1("move.err.hash_mismatch", entry.srcFullPath);
            return false;
        }
    }
    return true;
}

bool SafeMoveWorker::removeSource()
{
    // First try moving to Recycle Bin for maximum safety
    bool recycleOk = WinApi::sendToRecycleBin({m_sourcePath});
    if (recycleOk)
        return true;

    // Fallback: standard file / dir removal
    QFileInfo fi(m_sourcePath);
    if (fi.isDir()) {
        return QDir(m_sourcePath).removeRecursively();
    } else {
        return QFile::remove(m_sourcePath);
    }
}
