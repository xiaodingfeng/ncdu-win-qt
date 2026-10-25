#include "DuplicateScanner.h"

#include "CleanupScanner.h"     // classifyFileDanger, isExcludedByExtension
#include "MemoryMonitor.h"

#include <QFile>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFutureSynchronizer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

namespace {

// Files hashed per parallel batch. Small chunks keep progress reporting
// responsive and let a cancelled scan stop within a few files.
constexpr int PARTIAL_HASH_CHUNK = 256;
constexpr int FULL_HASH_CHUNK = 32;

// Hash `files` concurrently on `pool`, writing each result into its own slot of
// `out` (distinct elements, so no locking is needed).
void hashInParallel(QThreadPool& pool,
                    const std::vector<std::shared_ptr<FileNode>>& files,
                    const std::function<QByteArray(const QString&)>& hashFn,
                    const std::atomic<bool>& cancel,
                    std::vector<QByteArray>& out)
{
    out.assign(files.size(), QByteArray());

    QFutureSynchronizer<void> sync;
    for (size_t i = 0; i < files.size(); ++i) {
        if (cancel.load())
            break;
        sync.addFuture(QtConcurrent::run(&pool, [&files, &out, &hashFn, &cancel, i]() {
            if (!cancel.load())
                out[i] = hashFn(files[i]->path);
        }));
    }
    sync.waitForFinished();
}

}  // namespace

// --------------------------------------------------------------------------- //
// Construction / cancellation
// --------------------------------------------------------------------------- //

DuplicateScanner::DuplicateScanner(const QString& scanPath,
                                   std::shared_ptr<FileNode> rootNode,
                                   QObject* parent)
    : QThread(parent), m_scanPath(scanPath), m_rootNode(std::move(rootNode))
{
}

void DuplicateScanner::cancel()
{
    m_cancel.store(true);
}

// --------------------------------------------------------------------------- //
// Stage 1: group by size
// --------------------------------------------------------------------------- //

void DuplicateScanner::groupBySize(
    std::vector<std::vector<std::shared_ptr<FileNode>>>& out)
{
    if (!m_rootNode)
        return;

    // Bucket files by size. Only files ≥ MIN_DUP_SIZE; skip C/D-level paths
    // (system files and user personal folders) so we never hash or propose
    // them for deletion.
    std::unordered_map<qint64, std::vector<std::shared_ptr<FileNode>>> buckets;
    std::vector<std::shared_ptr<FileNode>> stack;
    stack.push_back(m_rootNode);

    while (!stack.empty()) {
        if (m_cancel.load())
            return;
        if (MemoryMonitor::isLowMemory())
            break;

        auto node = stack.back();
        stack.pop_back();

        if (node->nodeType == NodeType::File && node->size >= MIN_DUP_SIZE) {
            // Skip program/system file extensions (.dll, .exe, .sys, ...).
            // Uses the shared exclusion list from CleanupScanner so that
            // large-file and duplicate scanning always agree on what to skip.
            if (CleanupScanner::isExcludedByExtension(node->name))
                continue;
            const DangerLevel dl = CleanupScanner::classifyFileDanger(
                node->path, node->name);
            // Skip C-level (system/program directories). D-level (user data
            // like Documents/Desktop) is allowed so users can find duplicate
            // personal files; each file's danger level is shown in the UI.
            if (dl != DangerLevel::C) {
                buckets[node->size].push_back(node);
            }
        }
        for (const auto& child : node->children) {
            if (child)
                stack.push_back(child);
        }
    }

    // Keep only buckets with ≥2 files (potential duplicates).
    out.clear();
    out.reserve(buckets.size());
    for (auto& [size, vec] : buckets) {
        if (vec.size() >= 2)
            out.push_back(std::move(vec));
    }
}

// --------------------------------------------------------------------------- //
// Stage 2: partial hash (first 4 KB)
// --------------------------------------------------------------------------- //

QByteArray DuplicateScanner::hashPartial(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    QByteArray head = f.read(PARTIAL_HASH_BYTES);
    if (head.isEmpty())
        return QByteArray();
    // MD5 is only a pre-filter: a collision costs an extra full-file SHA-256
    // read in stage 3, it can never produce a false duplicate. MD5 is several
    // times faster than SHA-256 here, and this path runs on far more files.
    QCryptographicHash h(QCryptographicHash::Md5);
    h.addData(head);
    return h.result();
}

// --------------------------------------------------------------------------- //
// Stage 3: full-file hash (streamed, 4 MB buffer)
// --------------------------------------------------------------------------- //

QByteArray DuplicateScanner::hashFull(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    QCryptographicHash h(QCryptographicHash::Sha256);
    // Heap-allocated buffer — 4 MB is too large for the stack.
    std::vector<char> buf(static_cast<size_t>(FULL_HASH_BUFSIZE));
    while (!f.atEnd()) {
        if (m_cancel.load())
            return QByteArray();
        qint64 n = f.read(buf.data(), FULL_HASH_BUFSIZE);
        if (n <= 0)
            break;
        h.addData(buf.data(), static_cast<int>(n));
    }
    return h.result();
}

// --------------------------------------------------------------------------- //
// Main thread entry point
// --------------------------------------------------------------------------- //

void DuplicateScanner::run()
{
    // ── Stage 1: size grouping ───────────────────────────────────────────
    emit progress(1, 0, 0);

    std::vector<std::vector<std::shared_ptr<FileNode>>> sizeGroups;
    groupBySize(sizeGroups);
    if (m_cancel.load())
        return;

    // Total candidate count for progress reporting across stages 2 and 3.
    int stage2Total = 0;
    for (const auto& g : sizeGroups)
        stage2Total += static_cast<int>(g.size());

    // ── Stage 2: partial-hash grouping ───────────────────────────────────
    emit progress(2, 0, stage2Total);

    // A dedicated pool keeps this scan independent of other QtConcurrent work
    // (searches, cleanup enumeration). Capped because each worker holds an open
    // file handle, and full-hash workers also hold a 4 MB buffer.
    QThreadPool hashPool;
    hashPool.setMaxThreadCount(std::max(2, std::min(8, QThread::idealThreadCount())));
    const auto hashPartialFn = [this](const QString& p) { return hashPartial(p); };
    const auto hashFullFn = [this](const QString& p) { return hashFull(p); };

    // survivors[i] = files in sizeGroups[i] that share the same partial hash.
    std::vector<std::vector<std::shared_ptr<FileNode>>> partialGroups;
    int stage2Done = 0;

    for (auto& group : sizeGroups) {
        if (m_cancel.load())
            return;

        std::unordered_map<QByteArray, std::vector<std::shared_ptr<FileNode>>> buckets;
        for (size_t off = 0; off < group.size(); off += PARTIAL_HASH_CHUNK) {
            if (m_cancel.load())
                return;

            const size_t end = std::min(group.size(), off + PARTIAL_HASH_CHUNK);
            std::vector<std::shared_ptr<FileNode>> chunk(group.begin() + off,
                                                        group.begin() + end);
            std::vector<QByteArray> hashes;
            hashInParallel(hashPool, chunk, hashPartialFn, m_cancel, hashes);

            for (size_t i = 0; i < chunk.size(); ++i) {
                if (!hashes[i].isEmpty())
                    buckets[hashes[i]].push_back(chunk[i]);
            }
            stage2Done += static_cast<int>(chunk.size());
            emit progress(2, stage2Done, stage2Total);
            if (MemoryMonitor::isLowMemory())
                return;
        }
        for (auto& [hash, vec] : buckets) {
            if (vec.size() >= 2)
                partialGroups.push_back(std::move(vec));
        }
    }
    emit progress(2, stage2Done, stage2Total);

    // ── Stage 3: full-hash grouping ──────────────────────────────────────
    int stage3Total = 0;
    for (const auto& g : partialGroups)
        stage3Total += static_cast<int>(g.size());
    emit progress(3, 0, stage3Total);

    std::vector<DuplicateGroup> groups;
    int stage3Done = 0;
    qint64 totalWasted = 0;
    int totalFiles = 0;

    for (auto& group : partialGroups) {
        if (m_cancel.load())
            return;

        std::unordered_map<QByteArray, std::vector<std::shared_ptr<FileNode>>> buckets;
        for (size_t off = 0; off < group.size(); off += FULL_HASH_CHUNK) {
            if (m_cancel.load())
                return;

            const size_t end = std::min(group.size(), off + FULL_HASH_CHUNK);
            std::vector<std::shared_ptr<FileNode>> chunk(group.begin() + off,
                                                        group.begin() + end);
            std::vector<QByteArray> hashes;
            hashInParallel(hashPool, chunk, hashFullFn, m_cancel, hashes);

            for (size_t i = 0; i < chunk.size(); ++i) {
                if (!hashes[i].isEmpty())
                    buckets[hashes[i]].push_back(chunk[i]);
            }
            stage3Done += static_cast<int>(chunk.size());
            emit progress(3, stage3Done, stage3Total);
            if (MemoryMonitor::isLowMemory())
                return;
        }

        for (auto& [hash, vec] : buckets) {
            if (vec.size() < 2)
                continue;
            DuplicateGroup g;
            g.size = vec.front()->size;
            g.files.reserve(vec.size());
            for (auto& node : vec) {
                DuplicateFile df;
                df.path = node->path;
                df.name = node->name;
                df.size = node->size;
                df.danger = CleanupScanner::classifyFileDanger(node->path, node->name);
                g.files.push_back(std::move(df));
            }
            // Sort within group for stable display: shortest path first, so
            // the first file (kept by default) tends to be the "original".
            std::sort(g.files.begin(), g.files.end(),
                      [](const DuplicateFile& a, const DuplicateFile& b) {
                          return a.path.length() < b.path.length();
                      });
            totalWasted += g.wastedSpace();
            totalFiles += static_cast<int>(g.files.size());
            groups.push_back(g);
            emit groupFound(g);
        }
    }
    emit progress(3, stage3Done, stage3Total);

    // Sort groups: most wasted space first.
    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) {
                  return a.wastedSpace() > b.wastedSpace();
              });

    emit finished(std::move(groups), totalWasted, totalFiles);
}
