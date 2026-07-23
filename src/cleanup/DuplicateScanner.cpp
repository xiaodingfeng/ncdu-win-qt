#include "DuplicateScanner.h"

#include "CleanupScanner.h"     // classifyFileDanger (static)
#include "MemoryMonitor.h"

#include <QFile>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>

#include <unordered_map>
#include <vector>
#include <algorithm>

// --------------------------------------------------------------------------- //
// Excluded file extensions
// --------------------------------------------------------------------------- //
// These are program/system file types whose deletion can break applications
// or the OS. Duplicate cleanup should focus on user-managed files
// (documents, media, archives, downloads). Mainstream tools like Czkawka
// rely on path exclusion; we additionally exclude by extension so that
// e.g. a stray .dll copied into a user folder is still skipped.
namespace {
const QSet<QString>& excludedExtensions()
{
    // Lowercased, with leading dot.
    static const QSet<QString> s = {
        // Executables & libraries
        ".exe", ".dll", ".sys", ".drv",      // Windows program/driver
        ".ocx", ".ax", ".cpl", ".scr",        // ActiveX / DirectShow / CPL / screensaver
        ".msi", ".msp", ".mst",               // Windows Installer
        ".lib", ".a", ".obj",                 // Static lib / object files
        // System / driver support
        ".cat", ".inf",                       // Driver catalog / info
        ".cab",                               // System cabinet (Windows uses for updates)
        // Font files (system-registered; deleting breaks fonts)
        ".ttf", ".otf", ".fon", ".fnt",
        // Junk / no extension (typically system metadata or temp files
        // like desktop.ini, Thumbs.db are <1MB so already filtered by size,
        // but extensionless files ≥1MB are usually system page/hibernation
        // fragments or raw disk images the user should inspect manually)
    };
    return s;
}

bool isExcludedByExtension(const QString& fileName)
{
    const int dot = fileName.lastIndexOf('.');
    if (dot < 0)
        return true;  // no extension → skip (likely system/raw file)
    return excludedExtensions().contains(fileName.mid(dot).toLower());
}
} // namespace

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
            // These are almost never safe to delete even when duplicated —
            // apps rely on their specific copy. Also skip extensionless files
            // (usually system page/hibernation fragments or raw disk images).
            if (isExcludedByExtension(node->name))
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
    QCryptographicHash h(QCryptographicHash::Sha256);
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

    // survivors[i] = files in sizeGroups[i] that share the same partial hash.
    std::vector<std::vector<std::shared_ptr<FileNode>>> partialGroups;
    int stage2Done = 0;

    for (auto& group : sizeGroups) {
        if (m_cancel.load())
            return;

        std::unordered_map<QByteArray, std::vector<std::shared_ptr<FileNode>>> buckets;
        for (auto& node : group) {
            if (m_cancel.load())
                return;
            QByteArray ph = hashPartial(node->path);
            if (!ph.isEmpty())
                buckets[ph].push_back(node);
            ++stage2Done;
            if ((stage2Done & 0x3F) == 0) {  // every 64 files
                emit progress(2, stage2Done, stage2Total);
                if (MemoryMonitor::isLowMemory())
                    return;
            }
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
        for (auto& node : group) {
            if (m_cancel.load())
                return;
            QByteArray fh = hashFull(node->path);
            if (!fh.isEmpty())
                buckets[fh].push_back(node);
            ++stage3Done;
            if ((stage3Done & 0x0F) == 0) {  // every 16 files (full reads are heavy)
                emit progress(3, stage3Done, stage3Total);
                if (MemoryMonitor::isLowMemory())
                    return;
            }
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
