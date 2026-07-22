#include "DiskScanner.h"

#include "MemoryMonitor.h"

#include <QDir>
#include <QFileInfo>
#include <QThreadPool>
#include <QtConcurrent>
#include <algorithm>
#include <stack>

// NOMINMAX must precede <windows.h> to prevent min/max macro pollution.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// Default directories to skip full recursion for. These are tool-generated
// directories that typically contain thousands of tiny files and are not
// interesting to drill into at the individual file level. The total size is
// still computed quickly without building child nodes.
const QStringList DiskScanner::DEFAULT_SKIP_PATTERNS = {
    "node_modules",
    "WinSxS"  // Windows component store — tens of thousands of hardlinks;
              // recursing causes high CPU/memory and crashes.
};

// Safety caps to prevent runaway memory use / stack growth on pathological
// directory trees. MAX_DEPTH covers Windows MAX_LONG_PATH scenarios; reaching
// it produces a skipped placeholder node instead of recursing further.
namespace {
constexpr int MAX_SCAN_DEPTH = 64;
}

// ---------------------------------------------------------------------------
// Helpers (file-local)
// ---------------------------------------------------------------------------

namespace {

// Build a Win32 search pattern "<path>\*" from a Qt-style path.
// FindFirstFileW accepts backslashes; Qt uses forward slashes internally.
QString makeSearchPattern(const QString& path)
{
    QString p = QDir::toNativeSeparators(path);
    if (!p.endsWith(QLatin1Char('\\')))
        p += QLatin1Char('\\');
    p += QLatin1Char('*');
    return p;
}

// Build a child path using forward slashes (Qt canonical form).
QString makeChildPath(const QString& parentPath, const QString& name)
{
    QString p = parentPath;
    if (!p.endsWith(QLatin1Char('/')) && !p.endsWith(QLatin1Char('\\')))
        p += QLatin1Char('/');
    p += name;
    return p;
}

// Combine the two 32-bit halves of a WIN32_FIND_DATAW size into a 64-bit value.
inline qint64 combineFileSize(DWORD high, DWORD low)
{
    return (static_cast<qint64>(high) << 32) | static_cast<qint64>(low);
}

// Convert a wchar_t filename from WIN32_FIND_DATAW to QString.
inline QString fileNameFromWChar(const wchar_t* ws)
{
    return QString::fromWCharArray(ws);
}

} // namespace

// ---------------------------------------------------------------------------
// DiskScanner
// ---------------------------------------------------------------------------

DiskScanner::DiskScanner(const QString& rootPath, QObject* parent)
    : QThread(parent), m_rootPath(QFileInfo(rootPath).absoluteFilePath())
{
    for (const auto& p : DEFAULT_SKIP_PATTERNS)
        m_skipSet.insert(p.toLower());

    int cpu = QThread::idealThreadCount();
    if (cpu <= 0)
        cpu = 4;
    // Cap parallelism conservatively. Each worker holds multiple open
    // FindFirstFileW handles, which consume kernel paged pool. With 64
    // workers on a deep tree, paged pool exhaustion caused BSODs on
    // low-spec machines. 8 threads still hides NTFS syscall latency
    // (I/O-bound) while keeping concurrent kernel handles manageable.
    m_maxWorkers = std::max(2, std::min(8, cpu));
}

void DiskScanner::setSkipHeavyDirs(bool skip)
{
    m_skipSet.clear();
    if (skip) {
        for (const auto& p : DEFAULT_SKIP_PATTERNS)
            m_skipSet.insert(p.toLower());
    }
    // WinSxS must always be skipped — recursing into it causes high CPU,
    // excessive memory and crashes. Its total size is still computed via
    // computeDirSizeFast without building per-file nodes.
    m_skipSet.insert(QStringLiteral("winsxs"));
}

void DiskScanner::cancel()
{
    m_cancel = true;
}

void DiskScanner::run()
{
    if (!QFileInfo::exists(m_rootPath)) {
        emit error("disk.err.path_not_found", {{"path", m_rootPath}});
        return;
    }
    try {
        auto root = scanRoot(m_rootPath);
        if (m_cancel) {
            emit error("scanner.err.cancelled", {});
            return;
        }
        root->sortBySizeDesc();
        emit finishedTree(root);
    } catch (...) {
        emit error("disk.err.scan_failed", {});
    }
}

bool DiskScanner::shouldSkip(const QString& name) const
{
    if (m_skipSet.empty())
        return false;
    // Case-insensitive: Windows filesystem is case-insensitive.
    return m_skipSet.count(name.toLower()) > 0;
}

bool DiskScanner::isReparsePoint(DWORD fileAttributes, const QString& /*path*/) const
{
    // FILE_ATTRIBUTE_REPARSE_POINT (0x400) covers junctions, symlinks, and
    // other reparse points. We do NOT recurse into these to prevent infinite
    // loops in monorepo workspaces where sub-project node_modules is
    // junctioned to the parent project.
    return (fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void DiskScanner::computeDirSizeFast(const QString& path, qint64& size, int& fileCount, int& dirCount)
{
    size = 0;
    fileCount = 0;
    dirCount = 0;

    // Iterative traversal with an explicit path stack — avoids C++ stack
    // overflow on deeply nested trees. Each stack entry is one directory
    // whose contents we enumerate exactly once.
    std::stack<QString> pending;
    pending.push(path);

    while (!pending.empty() && !m_cancel) {
        const QString cur = pending.top();
        pending.pop();

        // Memory pressure: stop descending into more directories. Counts
        // accumulated so far remain valid for the parent node.
        if (MemoryMonitor::isLowMemory())
            break;

        const QString search = makeSearchPattern(cur);
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(reinterpret_cast<LPCWSTR>(search.utf16()), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;

        do {
            if (m_cancel)
                break;

            const QString name = fileNameFromWChar(fd.cFileName);
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;

            const DWORD attrs = fd.dwFileAttributes;

            // Never follow reparse points during fast size computation.
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                if (shouldSkip(name)) {
                    dirCount++;
                    continue;
                }
                dirCount++;
                pending.push(makeChildPath(cur, name));
            } else {
                size += combineFileSize(fd.nFileSizeHigh, fd.nFileSizeLow);
                fileCount++;
            }
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
    }
}

std::shared_ptr<FileNode> DiskScanner::makeDirNode(const QString& path, bool top)
{
    QString name = QFileInfo(path).fileName();
    NodeType nt = NodeType::Directory;

    // Detect Windows drive root like "C:/" or "C:\".
    if (top && path.length() >= 2 && path.length() <= 3 && path[1] == QLatin1Char(':')) {
        name = path.left(2);  // e.g. "C:"
        nt = NodeType::Drive;
    }
    if (name.isEmpty())
        name = path;

    auto node = std::make_shared<FileNode>();
    node->name = name;
    node->path = path;
    node->nodeType = nt;
    node->isHidden = name.startsWith(QLatin1Char('.')) || name.startsWith(QLatin1Char('$'));
    return node;
}

std::shared_ptr<FileNode> DiskScanner::makeSkippedDirNode(const QString& name, const QString& path)
{
    qint64 size = 0;
    int fileCount = 0;
    int dirCount = 0;
    computeDirSizeFast(path, size, fileCount, dirCount);

    auto node = std::make_shared<FileNode>();
    node->name = name;
    node->path = path;
    node->nodeType = NodeType::Directory;
    node->size = size;
    node->fileCount = fileCount;
    node->dirCount = dirCount;
    node->isHidden = name.startsWith(QLatin1Char('.'));
    node->skipped = true;
    return node;
}

std::shared_ptr<FileNode> DiskScanner::scanDirRecursive(const QString& path)
{
    // Iterative traversal with an explicit stack of (parent, path, depth)
    // tuples. Avoids C++ stack overflow on deeply nested trees. Children
    // are attached directly to their parent; sizes/counts are accumulated
    // in a post-order pass after the tree is built (computeSizesUpward).
    struct WorkItem {
        std::shared_ptr<FileNode> parent;
        QString path;
        int depth;
    };

    auto root = makeDirNode(path, false);
    if (m_cancel)
        return root;

    std::stack<WorkItem> pending;
    pending.push({nullptr, path, 0});

    while (!pending.empty() && !m_cancel) {
        if (MemoryMonitor::isLowMemory()) {
            emit progress("scanner.warn.low_memory", {});
            break;
        }

        WorkItem item = std::move(pending.top());
        pending.pop();

        auto node = makeDirNode(item.path, false);
        node->parent = item.parent;
        if (item.parent)
            item.parent->children.push_back(node);
        if (item.depth == 0)
            root = node;

        emit progress("disk.progress.scanning", {{"path", item.path}});

        const QString search = makeSearchPattern(item.path);
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(reinterpret_cast<LPCWSTR>(search.utf16()), &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED)
                node->error = NodeError::AccessDenied;
            else {
                node->error = NodeError::FindFirstFailed;
                node->lastError = static_cast<int>(err);
            }
            continue;
        }

        do {
            if (m_cancel)
                break;

            const QString name = fileNameFromWChar(fd.cFileName);
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;

            const DWORD attrs = fd.dwFileAttributes;
            const qint64 fsize = combineFileSize(fd.nFileSizeHigh, fd.nFileSizeLow);
            const QString childPath = makeChildPath(item.path, name);

            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
                // Symlink/junction: record but don't recurse.
                auto child = std::make_shared<FileNode>();
                child->name = name;
                child->path = childPath;
                child->nodeType = NodeType::Symlink;
                child->size = fsize;
                child->isHidden = name.startsWith(QLatin1Char('.'));
                child->parent = node;
                node->children.push_back(child);
                node->size += child->size;
                // Note: symlinks do NOT increment fileCount.
            } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                if (shouldSkip(name)) {
                    auto child = makeSkippedDirNode(name, childPath);
                    child->parent = node;
                    node->children.push_back(child);
                    node->size += child->size;
                    node->fileCount += child->fileCount;
                    node->dirCount += 1 + child->dirCount;
                } else if (item.depth >= MAX_SCAN_DEPTH) {
                    // Depth cap reached: record a skipped placeholder so the
                    // user still sees the directory exists, without recursing.
                    auto child = makeSkippedDirNode(name, childPath);
                    child->parent = node;
                    node->children.push_back(child);
                    node->size += child->size;
                    node->fileCount += child->fileCount;
                    node->dirCount += 1 + child->dirCount;
                } else {
                    // Push child for processing; size/count accumulated
                    // upward in the post-order pass below.
                    pending.push({node, childPath, item.depth + 1});
                    node->dirCount++;  // provisional; refined in post-pass
                }
            } else {
                // Regular file.
                auto child = std::make_shared<FileNode>();
                child->name = name;
                child->path = childPath;
                child->nodeType = NodeType::File;
                child->size = fsize;
                child->isHidden = name.startsWith(QLatin1Char('.'));
                child->parent = node;
                node->children.push_back(child);
                node->size += child->size;
                node->fileCount++;
            }
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
    }

    // Post-order pass: propagate each directory's size/fileCount/dirCount
    // to its parent. Iterative (explicit stack) to avoid recursion.
    // Note: skipped/symlink subtrees already finalized their own counts
    // inline above, so we only propagate from directories that were pushed
    // onto the pending stack (their children were attached after the parent).
    // The simplest correct approach: walk the whole tree bottom-up.
    computeSizesUpward(root);
    return root;
}

void DiskScanner::computeSizesUpward(const std::shared_ptr<FileNode>& root)
{
    if (!root)
        return;

    // First pass: reset provisional dirCount on stack-pushed directories
    // (we incremented them when pushing children; we'll recompute from
    // finalized child values). Skip directories whose counts were already
    // finalized inline (skipped/symlink children attached directly).
    // To keep this simple and correct, we recompute size/fileCount/dirCount
    // for every directory from scratch based on its children. Directories
    // with no children keep their existing values (covers skipped/empty).
    //
    // Two-stack iterative post-order:
    //   stack1: nodes to visit
    //   stack2: nodes in post-order (children before parents)
    std::stack<FileNode*> s1, s2;
    s1.push(root.get());
    while (!s1.empty()) {
        FileNode* n = s1.top();
        s1.pop();
        s2.push(n);
        for (auto& c : n->children) {
            if (c)
                s1.push(c.get());
        }
    }

    // Process in post-order: each node is visited after all its descendants.
    // For directories, recompute aggregated fields from children. Files and
    // symlinks already have correct per-node values; skip them.
    while (!s2.empty()) {
        FileNode* n = s2.top();
        s2.pop();
        if (!n->isDir())
            continue;
        // Don't touch skipped dirs — their size/counts were computed by
        // computeDirSizeFast and are authoritative.
        if (n->skipped)
            continue;

        qint64 aggSize = 0;
        int aggFiles = 0;
        int aggDirs = 0;
        for (auto& c : n->children) {
            if (!c)
                continue;
            aggSize += c->size;
            if (c->isDir()) {
                aggDirs += 1 + c->dirCount;
                aggFiles += c->fileCount;
            } else if (c->nodeType == NodeType::File) {
                aggFiles++;
            }
            // Symlinks contribute size only (already added above), no count.
        }
        n->size = aggSize;
        n->fileCount = aggFiles;
        n->dirCount = aggDirs;
    }
}

std::shared_ptr<FileNode> DiskScanner::scanRoot(const QString& path)
{
    auto root = makeDirNode(path, true);
    emit progress("disk.progress.scanning", {{"path", path}});

    const QString search = makeSearchPattern(path);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(reinterpret_cast<LPCWSTR>(search.utf16()), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            root->error = NodeError::AccessDenied;
        else {
            root->error = NodeError::FindFirstFailed;
            root->lastError = static_cast<int>(err);
        }
        return root;
    }

    // Classify entries into files/symlinks, regular dirs, and skipped dirs.
    struct Entry {
        QString name;
        QString path;
        qint64 size;
        DWORD attrs;
    };
    QList<Entry> files, dirs, skippedDirs;

    do {
        const QString name = fileNameFromWChar(fd.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        const DWORD attrs = fd.dwFileAttributes;
        const qint64 fsize = combineFileSize(fd.nFileSizeHigh, fd.nFileSizeLow);
        const QString childPath = makeChildPath(path, name);

        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            files.append({name, childPath, fsize, attrs});
        } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (shouldSkip(name))
                skippedDirs.append({name, childPath, 0, attrs});
            else
                dirs.append({name, childPath, 0, attrs});
        } else {
            files.append({name, childPath, fsize, attrs});
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // 1. Files first (sizes are cheap - already in WIN32_FIND_DATAW).
    for (const auto& e : files) {
        if (m_cancel)
            break;
        const bool isLink = (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        auto child = std::make_shared<FileNode>();
        child->name = e.name;
        child->path = e.path;
        child->nodeType = isLink ? NodeType::Symlink : NodeType::File;
        child->size = e.size;
        child->isHidden = e.name.startsWith(QLatin1Char('.'));
        child->parent = root;
        root->children.push_back(child);
        root->size += child->size;
        // Only FILE increments fileCount; symlinks don't.
        if (child->nodeType == NodeType::File)
            root->fileCount++;
    }

    // 2. Skipped directories: compute size quickly without building children.
    for (const auto& e : skippedDirs) {
        if (m_cancel)
            break;
        auto child = makeSkippedDirNode(e.name, e.path);
        child->parent = root;
        root->children.push_back(child);
        root->size += child->size;
        root->fileCount += child->fileCount;
        root->dirCount += 1 + child->dirCount;
    }

    // 3. Regular directories in parallel via QtConcurrent (thread-pooled).
    if (!dirs.isEmpty() && !m_cancel) {
        QThreadPool::globalInstance()->setMaxThreadCount(m_maxWorkers);

        QList<QString> dirPaths;
        for (const auto& d : dirs)
            dirPaths << d.path;

        QList<std::shared_ptr<FileNode>> results =
            QtConcurrent::blockingMapped<QList<std::shared_ptr<FileNode>>>(
                dirPaths,
                [this](const QString& p) -> std::shared_ptr<FileNode> {
                    return scanDirRecursive(p);
                });

        for (auto& sub : results) {
            if (m_cancel)
                break;
            if (!sub)
                continue;
            sub->parent = root;
            root->children.push_back(sub);
            root->size += sub->size;
            root->fileCount += sub->fileCount;
            root->dirCount += 1 + sub->dirCount;
        }
    }

    return root;
}
