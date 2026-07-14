#include "WinApi.h"

#include <QDir>
#include <QFileInfo>

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

// Recursively delete a directory's contents then the directory itself.
// Reparse points (junctions / directory symlinks) are unlinked rather than
// followed, matching DiskScanner's behavior of not recursing through them.
bool removeDirectoryRecursive(const QString& path)
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
                if (!RemoveDirectoryW(lpcwstr(child)))
                    ok = false;
            } else if (!removeDirectoryRecursive(child)) {
                ok = false;
            }
        } else {
            // Regular file or file symlink.
            clearReadOnly(child);
            if (!DeleteFileW(lpcwstr(child)))
                ok = false;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // Directory should now be empty; drop the read-only bit and remove it.
    clearReadOnly(path);
    if (!RemoveDirectoryW(lpcwstr(path)))
        ok = false;

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
            ok = false;
            continue;
        }

        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (isReparsePoint(attrs)) {
                // Directory reparse point: unlink without following.
                clearReadOnly(native);
                if (!RemoveDirectoryW(lpcwstr(native)))
                    ok = false;
            } else if (!removeDirectoryRecursive(native)) {
                ok = false;
            }
        } else {
            // Regular file or file symlink.
            clearReadOnly(native);
            if (!DeleteFileW(lpcwstr(native)))
                ok = false;
        }
    }
    return ok;
}
#else
bool deletePermanent(const QStringList&) { return false; }
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

} // namespace WinApi
