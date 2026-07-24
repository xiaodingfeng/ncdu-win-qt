#include "WinApi.h"

#include <QDir>
#include <QFileInfo>

#include "Logger.h"

#ifdef _WIN32
// NOMINMAX must precede <windows.h> to prevent min/max macro pollution.
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
#endif

namespace WinApi {

// ---------------------------------------------------------------------------
// sendToRecycleBin
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool sendToRecycleBin(const QStringList& paths)
{
    if (paths.isEmpty())
        return true;

    // SHFileOperationW expects a double-null-terminated list of null-terminated
    // wide strings: "path1\0path2\0\0". Use absolute, native-separator paths so
    // the shell resolves them correctly regardless of the current directory.
    QString combined;
    for (const QString& p : paths) {
        const QString abs = QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath());
        combined += abs + QChar::Null;
    }
    combined += QChar::Null; // terminating empty string (double null)

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd = nullptr;
    op.wFunc = FO_DELETE;                                    // 0x0003
    op.pFrom = reinterpret_cast<LPCWSTR>(combined.utf16());
    op.pTo = nullptr;
    op.fFlags = FOF_ALLOWUNDO      // 0x0040 - send to recycle bin, not permanent
              | FOF_NOCONFIRMATION  // 0x0010 - answer "yes to all"
              | FOF_SILENT           // 0x0004 - no progress UI
              | FOF_NOERRORUI;       // 0x0400 - suppress error dialogs
    op.fAnyOperationsAborted = FALSE;
    op.hNameMappings = nullptr;
    op.lpszProgressTitle = nullptr;

    const int result = SHFileOperationW(&op);
    return result == 0 && op.fAnyOperationsAborted == FALSE;
}
#else
bool sendToRecycleBin(const QStringList&) { return false; }
#endif

// ---------------------------------------------------------------------------
// Internal helpers (Windows only)
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

// QString::utf16() yields char16_t* (Qt6); reinterpret to the LPCWSTR the
// Win32 APIs expect. char16_t and wchar_t are both 16-bit on Windows.
inline LPCWSTR lpcwstr(const QString& s)
{
    return reinterpret_cast<LPCWSTR>(s.utf16());
}

// Remove the FILE_ATTRIBUTE_READONLY bit so the path can be deleted.
void clearReadOnly(const QString& path)
{
    const DWORD attrs = GetFileAttributesW(lpcwstr(path));
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return;
    if (attrs & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(lpcwstr(path), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
}

inline bool isReparsePoint(DWORD attrs)
{
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Build a Win32 search pattern "<nativePath>\*".
QString makeSearchPattern(const QString& path)
{
    QString p = QDir::toNativeSeparators(path);
    if (!p.endsWith(QLatin1Char('\\')))
        p += QLatin1Char('\\');
    p += QLatin1Char('*');
    return p;
}

// Join a parent path (any separators) with a child name using a forward slash.
QString joinChild(const QString& parent, const QString& name)
{
    QString p = parent;
    if (!p.endsWith(QLatin1Char('/')) && !p.endsWith(QLatin1Char('\\')))
        p += QLatin1Char('/');
    p += name;
    return p;
}

// Recursively delete a directory's contents.
// If removeSelf is true, also removes the directory itself at the end.
// If removeSelf is false, keeps the directory (for system dirs like Temp/Logs
// that must not be deleted themselves). Returns true if all operations
// succeeded (locked/in-use files return false but don't abort the loop).
// Reparse points (junctions / directory symlinks) are unlinked rather than
// followed, matching DiskScanner's behavior of not recursing through them.
bool removeDirectoryRecursive(const QString& path, bool removeSelf = true)
{
    const QString pattern = makeSearchPattern(path);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(lpcwstr(pattern), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return false;

    bool ok = true;
    do {
        const QString name = QString::fromWCharArray(fd.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        const QString child = joinChild(path, name);

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (isReparsePoint(fd.dwFileAttributes)) {
                // Junction / directory symlink: remove the link only.
                clearReadOnly(child);
                if (!RemoveDirectoryW(lpcwstr(child))) {
                    Logger::warn(QStringLiteral("[removeDir] RemoveDirectory(reparse) failed: %1 err=%2")
                                     .arg(child).arg(GetLastError()));
                    ok = false;
                }
            } else if (!removeDirectoryRecursive(child, true)) {
                ok = false;
            }
        } else {
            // Regular file or file symlink.
            clearReadOnly(child);
            if (!DeleteFileW(lpcwstr(child))) {
                const DWORD err = GetLastError();
                // Error 5 (ACCESS_DENIED) and 32 (SHARING_VIOLATION) are
                // expected for locked/in-use files — log at debug level,
                // not warn, to avoid log spam on Temp directories.
                Logger::warn(QStringLiteral("[removeDir] DeleteFile failed: %1 err=%2")
                                .arg(child).arg(err));
                ok = false;
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    if (removeSelf) {
        clearReadOnly(path);
        if (!RemoveDirectoryW(lpcwstr(path))) {
            const DWORD err = GetLastError();
            Logger::warn(QStringLiteral("[removeDir] RemoveDirectory failed: %1 err=%2")
                             .arg(path).arg(err));
            ok = false;
        }
    }

    return ok;
}

} // namespace
#endif // _WIN32

// ---------------------------------------------------------------------------
// deletePermanent
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool deletePermanent(const QStringList& paths)
{
    if (paths.isEmpty())
        return true;

    bool ok = true;
    for (const QString& p : paths) {
        const QString native = QDir::toNativeSeparators(p);
        const DWORD attrs = GetFileAttributesW(lpcwstr(native));
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD err = GetLastError();
            Logger::warn(QStringLiteral("[deletePermanent] GetFileAttributes failed: %1 err=%2 (0x%3)")
                             .arg(native).arg(err).arg(err, 0, 16));
            ok = false;
            continue;
        }

        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (isReparsePoint(attrs)) {
                // Directory reparse point: unlink without following.
                clearReadOnly(native);
                if (!RemoveDirectoryW(lpcwstr(native))) {
                    const DWORD err = GetLastError();
                    Logger::warn(QStringLiteral("[deletePermanent] RemoveDirectory(reparse) failed: %1 err=%2 (0x%3)")
                                     .arg(native).arg(err).arg(err, 0, 16));
                    ok = false;
                }
            } else if (!removeDirectoryRecursive(native)) {
                Logger::warn(QStringLiteral("[deletePermanent] removeDirectoryRecursive failed: %1")
                                 .arg(native));
                ok = false;
            }
        } else {
            // Regular file or file symlink.
            clearReadOnly(native);
            if (!DeleteFileW(lpcwstr(native))) {
                const DWORD err = GetLastError();
                Logger::warn(QStringLiteral("[deletePermanent] DeleteFile failed: %1 attrs=0x%2 err=%3 (0x%4)")
                                 .arg(native).arg(attrs, 0, 16).arg(err).arg(err, 0, 16));
                ok = false;
            }
        }
    }
    return ok;
}
#else
bool deletePermanent(const QStringList&) { return false; }
#endif

// ---------------------------------------------------------------------------
// cleanDirectoryContents — delete contents, keep the directory itself
// ---------------------------------------------------------------------------
#ifdef _WIN32
std::pair<int, int> cleanDirectoryContents(const QString& dirPath)
{
    const QString native = QDir::toNativeSeparators(dirPath);
    const QString pattern = makeSearchPattern(native);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(lpcwstr(pattern), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        Logger::warn(QStringLiteral("[cleanDir] FindFirstFile failed: %1 err=%2")
                         .arg(native).arg(GetLastError()));
        return {0, 0};
    }

    int deleted = 0;
    int skipped = 0;
    do {
        const QString name = QString::fromWCharArray(fd.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        const QString child = joinChild(native, name);
        bool childOk = true;

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (isReparsePoint(fd.dwFileAttributes)) {
                clearReadOnly(child);
                if (!RemoveDirectoryW(lpcwstr(child)))
                    childOk = false;
            } else {
                // Recurse into subdirectory: delete its contents AND the
                // subdirectory itself (subdirectories are safe to remove,
                // unlike the top-level system directory we're cleaning).
                childOk = removeDirectoryRecursive(child, true);
            }
        } else {
            clearReadOnly(child);
            if (!DeleteFileW(lpcwstr(child))) {
                const DWORD err = GetLastError();
                Logger::warn(QStringLiteral("[cleanDir] DeleteFile failed: %1 err=%2")
                                .arg(child).arg(err));
                childOk = false;
            }
        }

        if (childOk)
            ++deleted;
        else
            ++skipped;
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return {deleted, skipped};
}
#else
std::pair<int, int> cleanDirectoryContents(const QString&) { return {0, 0}; }
#endif

// ---------------------------------------------------------------------------
// revealInExplorer
// ---------------------------------------------------------------------------
#ifdef _WIN32
void revealInExplorer(const QString& path)
{
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const QString params = QStringLiteral("/select,\"%1\"").arg(native);
    ShellExecuteW(nullptr, L"open", L"explorer", lpcwstr(params), nullptr, SW_SHOWNORMAL);
}
#else
void revealInExplorer(const QString&) {}
#endif

// ---------------------------------------------------------------------------
// openPath
// ---------------------------------------------------------------------------
#ifdef _WIN32
void openPath(const QString& path)
{
    ShellExecuteW(nullptr, L"open", lpcwstr(path), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
void openPath(const QString&) {}
#endif

// ---------------------------------------------------------------------------
// isAdmin
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool isAdmin()
{
    // IsUserAnAdmin is deprecated but remains the simplest check; the app
    // manifest already forces elevation, so this is a defensive re-check.
    return IsUserAnAdmin() != FALSE;
}
#else
bool isAdmin() { return false; }
#endif

// ---------------------------------------------------------------------------
// getDiskFreeSpace
// ---------------------------------------------------------------------------
#ifdef _WIN32
std::tuple<qint64, qint64, qint64> getDiskFreeSpace(const QString& path)
{
    ULARGE_INTEGER freeBytesAvailable;   // available to the caller (quotas)
    ULARGE_INTEGER totalBytes;           // total size of the volume
    ULARGE_INTEGER totalFreeBytes;       // actual free bytes on the volume

    if (!GetDiskFreeSpaceExW(lpcwstr(path),
                             &freeBytesAvailable,
                             &totalBytes,
                             &totalFreeBytes)) {
        return std::make_tuple(qint64(0), qint64(0), qint64(0));
    }

    const qint64 free = static_cast<qint64>(freeBytesAvailable.QuadPart);
    const qint64 total = static_cast<qint64>(totalBytes.QuadPart);
    const qint64 used = total - free;
    return std::make_tuple(free, used, total);
}
#else
std::tuple<qint64, qint64, qint64> getDiskFreeSpace(const QString&)
{
    return std::make_tuple(qint64(0), qint64(0), qint64(0));
}
#endif

// ---------------------------------------------------------------------------
// queryRecycleBin — SHQueryRecycleBinW
// ---------------------------------------------------------------------------
#ifdef _WIN32
std::pair<qint64, qint64> queryRecycleBin(const QString& driveRoot)
{
    // SHQueryRecycleBinW expects a path or drive root (e.g. "C:\" or "C:/").
    // Use the drive root as the pszRootPath.
    QString root = QDir::toNativeSeparators(driveRoot);
    if (!root.endsWith(QLatin1Char('\\')))
        root += QLatin1Char('\\');

    SHQUERYRBINFO qbInfo;
    ZeroMemory(&qbInfo, sizeof(qbInfo));
    qbInfo.cbSize = sizeof(qbInfo);

    if (FAILED(SHQueryRecycleBinW(lpcwstr(root), &qbInfo)))
        return {0, 0};

    return {static_cast<qint64>(qbInfo.i64Size),
            static_cast<qint64>(qbInfo.i64NumItems)};
}
#else
std::pair<qint64, qint64> queryRecycleBin(const QString&) { return {0, 0}; }
#endif

// ---------------------------------------------------------------------------
// emptyRecycleBin — SHEmptyRecycleBinW
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool emptyRecycleBin(const QString& driveRoot)
{
    QString root = QDir::toNativeSeparators(driveRoot);
    if (!root.endsWith(QLatin1Char('\\')))
        root += QLatin1Char('\\');

    // SHERB_NOCONFIRMATION — don't ask "are you sure?"
    // SHERB_NOPROGRESSUI   — no progress bar
    // SHERB_NOSOUND        — no sound on completion
    const DWORD flags = SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND;
    return SUCCEEDED(SHEmptyRecycleBinW(nullptr, lpcwstr(root), flags));
}
#else
bool emptyRecycleBin(const QString&) { return false; }
#endif

} // namespace WinApi
