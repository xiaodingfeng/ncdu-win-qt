#include "WinApi.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <queue>
#include <vector>

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
#  include <sddl.h>
// CreateToolhelp32Snapshot / PROCESSENTRY32W: enumerating the running processes
// is what makes "a program is running from inside this folder" answerable.
#  include <tlhelp32.h>
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
// pathInsideDirectory — does this file sit under that folder?
// ---------------------------------------------------------------------------

// Case-insensitive and boundary-aware. Both halves matter: a plain prefix test
// says "C:\Data2" is inside "C:\Data", which is how a process gets blamed for a
// folder it has nothing to do with. Kept as pure string work, without touching
// the file system, so the probe lab can check the awkward cases directly.
bool pathInsideDirectory(const QString& file, const QString& dir)
{
    if (file.isEmpty() || dir.isEmpty())
        return false;

    const QString f = QDir::cleanPath(QDir::fromNativeSeparators(file)).toLower();
    QString d = QDir::cleanPath(QDir::fromNativeSeparators(dir)).toLower();
    while (d.endsWith(QLatin1Char('/')))
        d.chop(1);
    if (d.isEmpty() || f.isEmpty())
        return false;
    if (f == d)
        return true;
    // The character after the prefix must be a separator, which is exactly what
    // rules out "C:\Data2" while still accepting "C:\Data\sub".
    return f.startsWith(d) && f.size() > d.size()
           && f.at(d.size()) == QLatin1Char('/');
}

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

namespace {

// Restart Manager answers per (file, process) pair, so a process holding
// several files of the same folder arrives several times.
bool containsPid(const QVector<LockingProcess>& procs, quint32 pid)
{
    for (const LockingProcess& p : procs)
        if (p.pid == pid)
            return true;
    return false;
}

// The user a process runs as, as a SID string. Empty when the token cannot be
// read, which callers treat as "cannot vouch for this one".
QString sidStringOfToken(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return QString();
    DWORD need = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    QString out;
    if (need > 0) {
        QByteArray buf(static_cast<int>(need), 0);
        if (GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
            auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR str = nullptr;
            if (ConvertSidToStringSidW(user->User.Sid, &str)) {
                out = QString::fromWCharArray(str);
                LocalFree(str);
            }
        }
    }
    CloseHandle(token);
    return out;
}

QString currentUserSidString()
{
    // Asked once: the answer cannot change while we run.
    static const QString sid = sidStringOfToken(GetCurrentProcess());
    return sid;
}

// May we end this process? When we may not, *blockKey* receives the I18n key
// that explains it, so the UI can say why instead of silently skipping it.
bool closableProcess(quint32 pid, const QString& exePath, const QString& appName,
                     QString* blockKey)
{
    auto refuse = [blockKey](const char* key) {
        if (blockKey)
            *blockKey = QLatin1String(key);
        return false;
    };

    // Ourselves: terminating the window that runs the move would abort the very
    // operation the user is waiting for.
    if (pid == 0 || pid == static_cast<quint32>(GetCurrentProcessId()))
        return refuse("proc_close.block_self");

    // Either name can be the one that matches: Restart Manager reports the
    // friendly name ("Windows Explorer") while the guard list holds file names.
    if (isNeverCloseName(exePath) || isNeverCloseName(appName))
        return refuse("proc_close.block_system");

    // Another logon session — a service in session 0, or another signed-in
    // user — has no window of ours to close and no work of ours to save.
    DWORD theirSession = 0, ourSession = 0;
    ProcessIdToSessionId(pid, &theirSession);
    ProcessIdToSessionId(GetCurrentProcessId(), &ourSession);
    if (theirSession != ourSession)
        return refuse("proc_close.block_session");

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return refuse("proc_close.block_unknown");
    const QString theirSid = sidStringOfToken(h);
    CloseHandle(h);

    const QString ourSid = currentUserSidString();
    if (theirSid.isEmpty() || ourSid.isEmpty())
        return refuse("proc_close.block_unknown");
    if (theirSid.compare(ourSid, Qt::CaseInsensitive) != 0)
        return refuse("proc_close.block_user");
    return true;
}

} // namespace
#endif // _WIN32

// The guard list itself is pure Qt: it is what the UI explains, and it has to
// exist on every platform so a build without the Restart Manager still answers
// "may this be closed?" the same way.
QStringList neverCloseNames()
{
    // Lower case, matched against both the executable name and the friendly
    // name, which is why every entry carries the extension it will be compared
    // against ("System" and "Registry" have none — those are not files).
    return {
        // Machine- or session-critical: terminating any of these logs the user
        // out or bug-checks the box. No data folder is worth that.
        QStringLiteral("system"), QStringLiteral("registry"),
        QStringLiteral("smss.exe"), QStringLiteral("csrss.exe"),
        QStringLiteral("wininit.exe"), QStringLiteral("winlogon.exe"),
        QStringLiteral("services.exe"), QStringLiteral("lsass.exe"),
        QStringLiteral("svchost.exe"), QStringLiteral("fontdrvhost.exe"),
        QStringLiteral("dwm.exe"), QStringLiteral("audiodg.exe"),
        QStringLiteral("sihost.exe"), QStringLiteral("ctfmon.exe"),
        QStringLiteral("taskhostw.exe"), QStringLiteral("runtimebroker.exe"),
        QStringLiteral("searchindexer.exe"),
        // The shell. Windows does restart it, but the taskbar, the desktop
        // icons and every open Explorer window go with it — and it is the most
        // common holder of Documents / Desktop / Pictures, so seeing it means a
        // manual step for the user, not an automatic one for us.
        QStringLiteral("explorer.exe"),
        // Security software. Killing it is pointless (it respawns) and is the
        // fastest way to get this program quarantined by one of them.
        QStringLiteral("msmpeng.exe"), QStringLiteral("nissrv.exe"),
        QStringLiteral("securityhealthservice.exe"), QStringLiteral("msseces.exe"),
        QStringLiteral("usysdiag.exe"), QStringLiteral("hipsdaemon.exe"),
        QStringLiteral("qqpctray.exe"), QStringLiteral("qqpcsrv.exe"),
        QStringLiteral("360tray.exe"), QStringLiteral("360safe.exe"),
        QStringLiteral("zhudongfangyu.exe"),
    };
}

bool isNeverCloseName(const QString& nameOrPath)
{
    // Takes a path or a bare name: Restart Manager hands back the latter for
    // processes with no registered application name.
    const QString base = QFileInfo(nameOrPath).fileName().toLower();
    if (base.isEmpty())
        return false;
    return neverCloseNames().contains(base);
}

#ifdef _WIN32
namespace {

// How many files a single Restart Manager query is allowed to register, and how
// many the walk will look at while choosing them. The second number only needs
// to be large enough that a pathological tree cannot stall the probe: it runs on
// a worker thread, but "slow" and "hung" look the same to the user.
constexpr int kProbeWalkCap = 200000;

// The *cap* most recently modified files under *dir*.
//
// Which files get registered is the whole difference between a query that works
// and one that does not. RmGetList only reports processes holding a file that
// was registered, and a folder like a video editor's working directory holds
// hundreds of thousands of them, so "the first 800 the walk happens to return"
// reliably misses the one that matters. A program holding a file open for
// writing is, essentially by definition, holding a file it has just modified —
// so picking the newest candidates turns an arbitrary sample into a targeted
// one, at the cost of one bounded walk and O(cap) memory.
//
// Directory reparse points are not descended into: the folder being probed can
// contain the junction this program left behind, and walking through it would
// register the whole relocated tree (and, on a self-referential link, go round
// in circles).
std::vector<QString> newestFilesUnder(const QString& dir, int cap)
{
    struct Candidate {
        qint64 mtime = 0;
        QString path;
    };
    // A min-heap on mtime: the top is the oldest file kept so far, and therefore
    // the one to evict when something newer turns up.
    auto olderFirst = [](const Candidate& a, const Candidate& b) { return a.mtime > b.mtime; };
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(olderFirst)> heap(olderFirst);

    int examined = 0;
    bool capped = false;
    QStringList stack;
    stack << dir;
    while (!stack.isEmpty() && !capped) {
        const QString current = stack.takeLast();
        QDirIterator it(current,
                        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden
                            | QDir::System,
                        QDirIterator::NoIteratorFlags);
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            if (fi.isDir()) {
                if (!isReparsePointAt(fi.absoluteFilePath()))
                    stack << fi.absoluteFilePath();
                continue;
            }
            if (++examined > kProbeWalkCap) {
                capped = true;
                break;
            }
            const Candidate c{fi.lastModified().toMSecsSinceEpoch(), fi.absoluteFilePath()};
            if (static_cast<int>(heap.size()) < cap) {
                heap.push(c);
            } else if (c.mtime > heap.top().mtime) {
                heap.pop();
                heap.push(c);
            }
        }
    }

    std::vector<QString> out;
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(heap.top().path);
        heap.pop();
    }
    if (capped)
        Logger::warn(QStringLiteral("[probe] %1: walk stopped after %2 entries; the "
                                    "lock may sit in the part that was not seen")
                         .arg(dir).arg(kProbeWalkCap));
    Logger::info(QStringLiteral("[probe] %1: %2 entries examined, %3 registered")
                     .arg(dir).arg(examined).arg(out.size()));
    return out;
}

// Fill in everything the UI needs about a process it already has a PID for.
// Shared by both detections, because "who is this and may we close it" must not
// depend on which detection noticed it.
LockingProcess describeProcess(quint32 pid, const QString& friendlyName)
{
    LockingProcess lp;
    lp.pid = pid;
    lp.name = friendlyName;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        WCHAR buf[1024];
        DWORD sz = 1024;
        if (QueryFullProcessImageNameW(h, 0, buf, &sz))
            lp.exePath = QString::fromWCharArray(buf);
        CloseHandle(h);
    }
    if (lp.name.isEmpty())
        lp.name = lp.exePath.isEmpty() ? QString::number(pid)
                                       : QFileInfo(lp.exePath).fileName();

    lp.safeToClose = closableProcess(pid, lp.exePath, lp.name, &lp.blockKey);
    return lp;
}

} // namespace

// Processes whose executable is running FROM inside *dir*.
//
// Restart Manager reads the files a process holds open, which leaves it blind
// to the case that hurts most: a program installed inside its own data folder
// cannot be launched from a folder that will not rename, yet it may hold no
// data file open at all — so the query returns an empty list and the user is
// told nothing. One process enumeration answers it.
int processesRunningFrom(const QString& dir, QVector<LockingProcess>* procs)
{
    int found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Logger::warn(QStringLiteral("[processesRunningFrom] snapshot failed err=%1")
                         .arg(GetLastError()));
        return 0;
    }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const quint32 pid = pe.th32ProcessID;
            // The idle process and the system process have no image to inspect.
            if (pid == 0 || pid == 4)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h)
                continue;
            WCHAR buf[1024];
            DWORD sz = 1024;
            const bool got = QueryFullProcessImageNameW(h, 0, buf, &sz) != 0;
            CloseHandle(h);
            if (!got)
                continue;
            const QString image = QString::fromWCharArray(buf);
            if (!pathInsideDirectory(image, dir))
                continue;

            ++found;
            if (!procs)
                continue;
            if (containsPid(*procs, pid))
                continue;  // Restart Manager already named it; one row is enough
            procs->push_back(describeProcess(pid, QString()));
            Logger::info(QStringLiteral("[processesRunningFrom] %1 runs from inside %2")
                             .arg(image, dir));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Who is holding *dir* open. Two independent detectors, because either one on
// its own leaves a hole:
//   * Restart Manager knows which processes hold files open — but only the files
//     it was told about, and only files (a folder handle held by a process whose
//     working directory is there is invisible to it).
//   * the image scan sees a program running from inside the folder — which is
//     the strongest possible reason a folder will not rename, and the very case
//     Restart Manager misses.
// Held for both detectors: a process we must never close is never returned as
// closable, whichever one noticed it.
int processesLockingDir(const QString& dir, QVector<LockingProcess>* procs, int maxFiles)
{
    if (procs)
        procs->clear();

    const std::vector<QString> files = newestFilesUnder(dir, maxFiles);

    std::vector<std::wstring> native;
    native.reserve(files.size());
    for (const QString& f : files)
        native.push_back(QDir::toNativeSeparators(f).toStdWString());
    std::vector<LPCWSTR> paths;
    paths.reserve(native.size());
    for (const std::wstring& f : native)
        paths.push_back(f.c_str());

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = { 0 };
    const DWORD startRc = RmStartSession(&session, 0, key);
    if (startRc != ERROR_SUCCESS) {
        Logger::warn(QStringLiteral("[processesLockingDir] RmStartSession failed rc=%1").arg(startRc));
        // The image scan needs no session, so the query still has an answer.
        return processesRunningFrom(dir, procs);
    }
    int found = 0;
    const DWORD regRc = paths.empty()
                            ? ERROR_SUCCESS
                            : RmRegisterResources(session, static_cast<UINT>(paths.size()),
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
            const quint32 pid = info[i].Process.dwProcessId;
            if (procs && containsPid(*procs, pid))
                continue;  // Restart Manager reports one entry per file it holds
            const LockingProcess lp =
                describeProcess(pid, QString::fromWCharArray(info[i].strAppName));
            if (procs)
                procs->push_back(lp);
            ++found;
        }
    }
    RmEndSession(session);

    // Second, independent detector. A query that named nobody is not evidence
    // that nobody is there, and this is the cheapest way to make that gap
    // smaller — a program running from inside the folder is a certain blocker
    // even when it holds no data file open.
    processesRunningFrom(dir, procs);

    // The return value is the number of DISTINCT processes, so that it says the
    // same thing as the list the caller is handed. Restart Manager's raw count
    // is per file, which reads as "twelve programs" when it is one program
    // holding twelve files.
    return procs ? static_cast<int>(procs->size()) : found;
}
#else
bool relaunchAsAdmin() { return false; }
bool pathInsideDirectory(const QString&, const QString&) { return false; }
int processesLockingDir(const QString&, QVector<LockingProcess>*, int) { return 0; }
int processesRunningFrom(const QString&, QVector<LockingProcess>*) { return 0; }
#endif

// ---------------------------------------------------------------------------
// Closing a process that is holding a folder open
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

struct CloseEnumContext {
    DWORD pid = 0;
    int sent = 0;
};

BOOL CALLBACK closeEnumProc(HWND hwnd, LPARAM param)
{
    auto* ctx = reinterpret_cast<CloseEnumContext*>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != ctx->pid)
        return TRUE;
    // POSTED, not sent: the target handles it on its own message loop, so a
    // program that takes its time saving its work never blocks us.
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0))
        ++ctx->sent;
    return TRUE;
}

}  // namespace

int requestCloseProcess(quint32 pid)
{
    if (pid == 0)
        return 0;
    CloseEnumContext ctx;
    ctx.pid = static_cast<DWORD>(pid);
    EnumWindows(closeEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.sent;
}

bool processAlive(quint32 pid)
{
    if (pid == 0)
        return false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h)
        // Either already gone, or beyond our reach — and in the second case we
        // could not have closed it anyway, so "gone" is the honest answer.
        return false;
    // A process handle becomes signalled the moment the process exits, which
    // asks "is it dead yet?" without walking the process list.
    const DWORD rc = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return rc == WAIT_TIMEOUT;
}

bool terminateProcess(quint32 pid, quint32* winError)
{
    if (pid == 0)
        return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        if (winError)
            *winError = GetLastError();
        return false;
    }
    const bool ok = TerminateProcess(h, 1) != FALSE;
    if (!ok) {
        if (winError)
            *winError = GetLastError();
    } else {
        // Give it the moment it needs to actually disappear: checking liveness
        // while it is still tearing down would call a successful kill a failure.
        WaitForSingleObject(h, 5000);
    }
    CloseHandle(h);
    return ok;
}
#else
int requestCloseProcess(quint32) { return 0; }
bool processAlive(quint32) { return false; }
bool terminateProcess(quint32, quint32*) { return false; }
#endif

QVector<CloseOutcome> closeProcesses(const QVector<LockingProcess>& procs, int graceMs)
{
    QVector<CloseOutcome> out;
    out.reserve(procs.size());

    // Pass 1 — say please, to everyone at once. Doing it in one sweep means a
    // batch of five programs shuts down inside one grace period instead of five.
    QVector<int> waiting;
    for (int i = 0; i < procs.size(); ++i) {
        CloseOutcome o;
        o.pid = procs[i].pid;
        o.name = procs[i].name;

        if (!procs[i].safeToClose) {
            o.result = CloseOutcome::Refused;
            o.blockKey = procs[i].blockKey;
        } else if (!processAlive(procs[i].pid)) {
            o.result = CloseOutcome::Exited;
        } else {
            requestCloseProcess(procs[i].pid);
            waiting.push_back(i);   // result decided in pass 3
        }
        out.push_back(o);
    }

    // Pass 2 — one shared grace period. The loop is pumped in short slices so
    // the window keeps painting while a slow program saves, instead of looking
    // hung for the whole wait.
    for (int elapsed = 0; !waiting.isEmpty() && elapsed < graceMs; elapsed += 30) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        for (int k = waiting.size() - 1; k >= 0; --k) {
            if (!processAlive(procs[waiting[k]].pid))
                waiting.remove(k);
        }
        if (!waiting.isEmpty())
            QThread::msleep(30);
    }

    // Pass 3 — whatever is still standing ignored the request.
    for (int i : waiting) {
        out[i].result = (terminateProcess(procs[i].pid) && !processAlive(procs[i].pid))
                            ? CloseOutcome::Killed
                            : CloseOutcome::Survived;
    }
    return out;
}

QStringList respawnedAmong(const QVector<LockingProcess>& procs,
                           const QStringList& closedImages)
{
    QStringList out;
    if (closedImages.isEmpty())
        return out;

    // Identity across a restart is the image path, not the name and certainly
    // not the PID: an update can rename the executable's display name, and every
    // restart hands out a new number.
    auto normalize = [](const QString& raw) {
        return QDir::cleanPath(QDir::fromNativeSeparators(raw)).toLower();
    };
    QStringList closed;
    closed.reserve(closedImages.size());
    for (const QString& c : closedImages)
        closed << normalize(c);

    for (const LockingProcess& p : procs) {
        const QString key = normalize(p.exePath.isEmpty() ? p.name : p.exePath);
        if (key.isEmpty() || !closed.contains(key))
            continue;
        if (!out.contains(p.name))
            out << p.name;
    }
    return out;
}

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
