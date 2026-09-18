#include "WinApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include "Logger.h"
#include "I18n.h"

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
// *freedBytes (optional) accumulates the size of every file removed, so the
// caller can show the deletion moving instead of staring at a stalled bar.
bool removeDirectoryRecursive(const QString& path, bool removeSelf = true,
                              std::atomic<qint64>* freedBytes = nullptr)
{
    const QString pattern = makeSearchPattern(path);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(lpcwstr(pattern), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return false;

    // Ask for the delete first and only pay for the attribute round-trip when
    // Windows actually refuses. Clearing the read-only bit up-front costs two
    // extra syscalls on *every* item — which is exactly what makes deleting a
    // tree with tens of thousands of entries slow. (Doubles as the retry path
    // for a directory whose read-only bit blocks RemoveDirectory.)
    const auto unlinkOne = [freedBytes](LPCWSTR wide, const QString& shown, bool isDir,
                                        qint64 bytes) {
        const auto attempt = [&]() {
            return isDir ? RemoveDirectoryW(wide) : DeleteFileW(wide);
        };
        if (attempt()) {
            if (freedBytes && bytes > 0)
                freedBytes->fetch_add(bytes, std::memory_order_relaxed);
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            clearReadOnly(shown);
            if (attempt()) {
                if (freedBytes && bytes > 0)
                    freedBytes->fetch_add(bytes, std::memory_order_relaxed);
                return true;
            }
            err = GetLastError();
        }
        // Error 5 (ACCESS_DENIED) and 32 (SHARING_VIOLATION) are expected for
        // locked/in-use files, so this stays at warn level without spam.
        Logger::warn(QStringLiteral("[removeDir] delete failed: %1 err=%2 (dir=%3)")
                         .arg(shown).arg(err).arg(isDir));
        return false;
    };

    bool ok = true;
    do {
        const QString name = QString::fromWCharArray(fd.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        const QString child = joinChild(path, name);

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (isReparsePoint(fd.dwFileAttributes)) {
                // Junction / directory symlink: remove the link only, never the
                // target. RemoveDirectoryW is exactly that (no /S equivalent).
                if (!unlinkOne(lpcwstr(child), child, true, 0))
                    ok = false;
            } else if (!removeDirectoryRecursive(child, true, freedBytes)) {
                ok = false;
            }
        } else {
            // Regular file or file symlink.
            const qint64 bytes = (static_cast<qint64>(fd.nFileSizeHigh) << 32)
                                 | static_cast<qint64>(fd.nFileSizeLow);
            if (!unlinkOne(lpcwstr(child), child, false, bytes))
                ok = false;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    if (removeSelf && !unlinkOne(lpcwstr(path), path, true, 0))
        ok = false;

    return ok;
}

} // namespace
#endif // _WIN32

// ---------------------------------------------------------------------------
// isWindowsRoot
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

// Lowercase, forward slashes, no trailing slash: "C:\Windows\", "c:/windows"
// and "C:/Windows" all end up identical so they can be compared directly.
QString normalizedKey(const QString& path)
{
    if (path.isEmpty())
        return QString();
    QString p = QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

} // namespace

bool isWindowsRoot(const QString& path)
{
    const QString target = normalizedKey(path);
    if (target.isEmpty())
        return false;

    QString winDir = qEnvironmentVariable("SystemRoot");
    if (winDir.isEmpty())
        winDir = QStringLiteral("C:\\Windows");
    return target == normalizedKey(winDir);
}
#else
bool isWindowsRoot(const QString&) { return false; }
#endif

// ---------------------------------------------------------------------------
// renamePath / isReparsePointAt
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool renamePath(const QString& from, const QString& to, quint32* winError)
{
    if (from.isEmpty() || to.isEmpty())
        return false;
    const QString a = QDir::toNativeSeparators(from);
    const QString b = QDir::toNativeSeparators(to);
    // MoveFileW (not MoveFileExW with COPY_ALLOWED) never copies: on a different
    // volume or with an existing target it simply fails, so the caller can fall
    // back instead of unexpectedly streaming gigabytes around.
    if (MoveFileW(lpcwstr(a), lpcwstr(b)) != 0) {
        if (winError)
            *winError = 0;
        return true;
    }
    const DWORD err = GetLastError();
    if (winError)
        *winError = err;
    // Logged here because this is the only place the Win32 error still exists:
    // a rename of a folder an application has open fails with 32 and no other
    // symptom, which is impossible to diagnose from the bool alone.
    Logger::warn(QStringLiteral("[renamePath] MoveFileW failed: %1 -> %2 err=%3 (0x%4)")
                     .arg(a, b)
                     .arg(err)
                     .arg(err, 0, 16));
    return false;
}

bool isReparsePointAt(const QString& path)
{
    const DWORD attrs = GetFileAttributesW(lpcwstr(QDir::toNativeSeparators(path)));
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
bool renamePath(const QString& from, const QString& to, quint32* winError)
{
    if (winError)
        *winError = 0;
    return QFile::rename(from, to);
}

bool isReparsePointAt(const QString& path)
{
    return QFileInfo(path).isSymLink();
}
#endif

// ---------------------------------------------------------------------------
// dirSizeNoReparse
// ---------------------------------------------------------------------------
#ifdef _WIN32
qint64 dirSizeNoReparse(const QString& path, std::atomic<qint64>* visited)
{
    const QString pattern = makeSearchPattern(path);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(lpcwstr(pattern), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;

    qint64 total = 0;
    do {
        const QString name = QString::fromWCharArray(fd.cFileName);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        // A junction or symlink is only a link, never storage of its own.
        // Following it would count — and later copy — files that already live
        // somewhere else.
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            continue;

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            total += dirSizeNoReparse(joinChild(path, name), visited);
        } else {
            const qint64 bytes = (static_cast<qint64>(fd.nFileSizeHigh) << 32)
                                 | static_cast<qint64>(fd.nFileSizeLow);
            total += bytes;
            // Only files are published here, never the subtree totals the
            // recursion returns: adding a whole subtree on top of the files that
            // were already counted would report the same bytes twice, and the
            // counter must only ever grow for the UI to be able to trust it.
            if (visited)
                visited->fetch_add(bytes, std::memory_order_relaxed);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return total;
}
#else
qint64 dirSizeNoReparse(const QString& path, std::atomic<qint64>* visited)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const qint64 bytes = it.fileInfo().size();
        total += bytes;
        if (visited)
            visited->fetch_add(bytes, std::memory_order_relaxed);
    }
    return total;
}
#endif

// ---------------------------------------------------------------------------
// deletePermanent
// ---------------------------------------------------------------------------
#ifdef _WIN32
bool deletePermanent(const QStringList& paths, std::atomic<qint64>* freedBytes)
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
            } else if (!removeDirectoryRecursive(native, true, freedBytes)) {
                Logger::warn(QStringLiteral("[deletePermanent] removeDirectoryRecursive failed: %1")
                                 .arg(native));
                ok = false;
            }
        } else {
            // Regular file or file symlink.
            clearReadOnly(native);
            // Measured before the delete: afterwards there is nothing left to ask.
            const qint64 bytes = QFileInfo(native).size();
            if (!DeleteFileW(lpcwstr(native))) {
                const DWORD err = GetLastError();
                Logger::warn(QStringLiteral("[deletePermanent] DeleteFile failed: %1 attrs=0x%2 err=%3 (0x%4)")
                                 .arg(native).arg(attrs, 0, 16).arg(err).arg(err, 0, 16));
                ok = false;
            } else if (freedBytes && bytes > 0) {
                freedBytes->fetch_add(bytes, std::memory_order_relaxed);
            }
        }
    }
    return ok;
}
#else
bool deletePermanent(const QStringList&, std::atomic<qint64>*) { return false; }
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
// cleanDirectoryContentsToRecycleBin — move contents to Recycle Bin, keep dir
// ---------------------------------------------------------------------------
#ifdef _WIN32
std::pair<int, int> cleanDirectoryContentsToRecycleBin(const QString& dirPath)
{
    QStringList children;
    QDir dir(dirPath);
    const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& e : entries)
        children << e.absoluteFilePath();
    if (children.isEmpty())
        return {0, 0};
    // sendToRecycleBin uses SHFileOperationW(FO_DELETE|FOF_ALLOWUNDO), which
    // recursively moves directories into the Recycle Bin. The directory itself
    // is kept because we only pass its top-level children.
    const bool ok = sendToRecycleBin(children);
    const int n = static_cast<int>(children.size());
    return ok ? std::make_pair(n, 0)
              : std::make_pair(0, n);
}
#else
std::pair<int, int> cleanDirectoryContentsToRecycleBin(const QString&) { return {0, 0}; }
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
// relaunchAsAdmin / processesLockingDir
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  include <RestartManager.h>
#endif

#ifdef _WIN32
bool relaunchAsAdmin()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    // Rebuild the argument line exactly as passed in, quoting every argument
    // so paths with spaces survive.
    const QStringList args = QCoreApplication::arguments().mid(1);
    QString params;
    for (const QString& a : args) {
        if (!params.isEmpty())
            params += QLatin1Char(' ');
        QString q = a;
        q.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        params += QLatin1Char('"') + q + QLatin1Char('"');
    }
    const HINSTANCE h = ShellExecuteW(nullptr, L"runas", lpcwstr(exe),
                                      params.isEmpty() ? nullptr : lpcwstr(params),
                                      nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns >32 on success; the SE_ERR_* codes start at 0.
    if (h <= reinterpret_cast<HINSTANCE>(32)) {
        Logger::warn(QStringLiteral("[relaunchAsAdmin] ShellExecuteW runas failed err=%1")
                         .arg(reinterpret_cast<qintptr>(h)));
        return false;
    }
    return true;
}

int processesLockingDir(const QString& dir, QStringList* procNames, int maxFiles)
{
    if (procNames)
        procNames->clear();

    // Restart Manager works on FILES, not directories — and the handle that
    // blocks the rename can sit on any file in the tree. Sample a bounded set;
    // trees bigger than the cap may hide their lock beyond it.
    std::vector<std::wstring> files;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && static_cast<int>(files.size()) < maxFiles)
        files.push_back(QDir::toNativeSeparators(it.next()).toStdWString());
    if (files.empty())
        return 0;

    std::vector<LPCWSTR> paths;
    paths.reserve(files.size());
    for (const std::wstring& f : files)
        paths.push_back(f.c_str());

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = { 0 };
    const DWORD startRc = RmStartSession(&session, 0, key);
    if (startRc != ERROR_SUCCESS) {
        Logger::warn(QStringLiteral("[processesLockingDir] RmStartSession failed rc=%1").arg(startRc));
        return 0;
    }
    int found = 0;
    const DWORD regRc = RmRegisterResources(session, static_cast<UINT>(paths.size()),
                                            paths.data(), 0, nullptr, 0, nullptr);
    if (regRc != ERROR_SUCCESS) {
        Logger::warn(QStringLiteral("[processesLockingDir] RmRegisterResources failed rc=%1 files=%2")
                         .arg(regRc).arg(paths.size()));
    }
    if (regRc == ERROR_SUCCESS) {
        std::vector<RM_PROCESS_INFO> info(64);
        std::vector<DWORD> reasons(64);
        UINT needed = 0, got = 0;
        // got is an IN/OUT parameter: on input it is the ARRAY CAPACITY, on
        // output the number of entries filled. Passing 0 (as an uninitialized
        // value) says "no room" and RmGetList answers MORE_DATA forever.
        got = static_cast<UINT>(info.size());
        DWORD rc = RmGetList(session, &needed, &got, info.data(), reasons.data());
        Logger::info(QStringLiteral("[processesLockingDir] RmGetList rc=%1 needed=%2 got=%3")
                         .arg(rc).arg(needed).arg(got));
        // MORE_DATA means the array was too small: needed holds the real count.
        if (rc == ERROR_MORE_DATA && needed > got) {
            info.resize(needed);
            reasons.resize(needed);
            got = static_cast<UINT>(info.size());
            rc = RmGetList(session, &needed, &got, info.data(), reasons.data());
            Logger::info(QStringLiteral("[processesLockingDir] RmGetList retry rc=%1 got=%2")
                             .arg(rc).arg(got));
        }
        // Partial fills still name real processes, so iterate whatever arrived.
        for (UINT i = 0; i < got && i < info.size(); ++i) {
            ++found;
            QString name = QString::fromWCharArray(info[i].strAppName);
            if (name.isEmpty()) {
                // Some processes have no friendly name registered — fall back
                // to their executable file name.
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                       info[i].Process.dwProcessId);
                if (h) {
                    WCHAR buf[1024];
                    DWORD sz = 1024;
                    if (QueryFullProcessImageNameW(h, 0, buf, &sz))
                        name = QFileInfo(QString::fromWCharArray(buf)).fileName();
                    CloseHandle(h);
                }
            }
            if (!name.isEmpty() && procNames && !procNames->contains(name))
                procNames->push_back(name);
        }
    }
    RmEndSession(session);
    return found;
}
#else
bool relaunchAsAdmin() { return false; }
int processesLockingDir(const QString&, QStringList*, int) { return 0; }
#endif

// ---------------------------------------------------------------------------
// writeDontDeleteMarker
// ---------------------------------------------------------------------------
namespace {

// The marker's name for one language, or an empty string when that language has
// no translation for it (I18n hands back the key itself in that case — never
// usable as a file name).
QString markerNameIn(const QString& lang)
{
    const QString key = QStringLiteral("app_move.marker_name");
    const QString name = I18n::trIn(lang, key);
    if (name.isEmpty() || name == key)
        return QString();
    return name;
}

}  // namespace

QStringList dontDeleteMarkerNames()
{
    QStringList names;
    const QString active = markerNameIn(I18n::currentLanguage());
    if (!active.isEmpty())
        names << active;   // the name a fresh move would use, first
    for (const QString& lang : I18n::availableLanguages()) {
        const QString name = markerNameIn(lang);
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    if (names.isEmpty())
        // No locale files at all (a stripped build): still mark the folder,
        // under the name this feature has always used.
        names << QStringLiteral("!请勿删除、移动或重命名文件夹.ico");
    return names;
}

bool writeDontDeleteMarker(const QString& dir)
{
    if (dir.isEmpty())
        return false;
    const QStringList names = dontDeleteMarkerNames();
    // Already marked — in whatever language was active at the time. Adding the
    // current language's name next to it would only put two icons in one folder.
    for (const QString& name : names) {
        if (QFile::exists(dir + QLatin1Char('/') + name))
            return true;
    }
    const QString target = dir + QLatin1Char('/') + names.first();
    // qt_add_resources keeps the file's directory in the alias, so the icon
    // lives at :/moveguard/resources/... (verified by probe_lab/run4).
    QFile res(QStringLiteral(":/moveguard/resources/dont_delete_folder.ico"));
    if (!res.open(QIODevice::ReadOnly))
        return false;
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(res.readAll());
    return true;
}

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
