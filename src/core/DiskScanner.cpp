#include "DiskScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QThreadPool>
#include <QtConcurrent>
#include <algorithm>

// NOMINMAX must precede <windows.h> to prevent min/max macro pollution.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// Default directories to skip full recursion for. These are tool-generated
// directories that typically contain thousands of tiny files and are not
// interesting to drill into at the individual file level. The total size is
// still computed quickly without building child nodes.
const QStringList DiskScanner::DEFAULT_SKIP_PATTERNS = {"node_modules"};

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
    // Fast mode: I/O-bound, many threads hide NTFS syscall latency.
    m_maxWorkers = std::max(4, std::min(64, cpu * 8));
}

void DiskScanner::setSkipHeavyDirs(bool skip)
{
    m_skipSet.clear();
    if (skip) {
        for (const auto& p : DEFAULT_SKIP_PATTERNS)
            m_skipSet.insert(p.toLower());
    }
}

void DiskScanner::cancel()
{
    m_cancel = true;
}

void DiskScanner::run()
{
    if (!QFileInfo::exists(m_rootPath)) {
        emit error("Path does not exist: " + m_rootPath);
        return;
    }
    try {
        auto root = scanRoot(m_rootPath);
        if (m_cancel) {
            emit error("cancelled");
            return;
        }
        root->sortBySizeDesc();
        emit finishedTree(root);
    } catch (...) {
        emit error("Scan failed");
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

    const QString search = makeSearchPattern(path);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(reinterpret_cast<LPCWSTR>(search.utf16()), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

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
            qint64 subSize = 0;
            int subFiles = 0;
            int subDirs = 0;
            computeDirSizeFast(makeChildPath(path, name), subSize, subFiles, subDirs);
            size += subSize;
            fileCount += subFiles;
            dirCount += 1 + subDirs;
        } else {
            size += combineFileSize(fd.nFileSizeHigh, fd.nFileSizeLow);
            fileCount++;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
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
    auto node = makeDirNode(path, false);
    if (m_cancel)
        return node;

    emit progress(path);

    const QString search = makeSearchPattern(path);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(reinterpret_cast<LPCWSTR>(search.utf16()), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            node->error = QStringLiteral("Access denied");
        else
            node->error = QStringLiteral("FindFirstFile failed (error %1)").arg(err);
        return node;
    }

    do {
        if (m_cancel)
            break;

        const QString name = fileNameFromWChar(fd.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        const DWORD attrs = fd.dwFileAttributes;
        const qint64 fsize = combineFileSize(fd.nFileSizeHigh, fd.nFileSizeLow);
        const QString childPath = makeChildPath(path, name);

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
            } else {
                auto child = scanDirRecursive(childPath);
                child->parent = node;
                node->children.push_back(child);
                node->size += child->size;
                node->fileCount += child->fileCount;
                node->dirCount += 1 + child->dirCount;
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
    return node;
}

std::shared_ptr<FileNode> DiskScanner::scanRoot(const QString& path)
{
    auto root = makeDirNode(path, true);
    emit progress(path);

    const QString search = makeSearchPattern(path);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(reinterpret_cast<LPCWSTR>(search.utf16()), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            root->error = QStringLiteral("Access denied");
        else
            root->error = QStringLiteral("FindFirstFile failed (error %1)").arg(err);
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
