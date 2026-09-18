#pragma once

#include <QString>
#include <QStringList>
#include <tuple>
#include <utility>

// WinApi - thin C++ wrapper around the Windows-specific operations used by
// NcduWin (recycle bin, permanent deletion, explorer reveal, default open,
// admin check, disk free space).
//
// All functions are no-ops / return failure defaults on non-Windows platforms
// so callers can include this header unconditionally.
namespace WinApi {

// Move a list of paths to the Windows recycle bin via SHFileOperationW.
// Returns true when the operation reports full success (no errors and no
// user-aborted operations). On non-Windows builds returns false.
bool sendToRecycleBin(const QStringList& paths);

// Permanently delete a list of paths (bypass the recycle bin), clearing the
// read-only attribute on each item first. Directories are removed recursively;
// reparse points (junctions / directory symlinks) are unlinked without
// following their target. Returns true only when every path was deleted.
//
// When *freedBytes is given it is accumulated with the size of every file as it
// is removed, so a caller can watch the work happen and show its progress. The
// counter only ever grows, which is what makes it safe to poll from the UI.
bool deletePermanent(const QStringList& paths, std::atomic<qint64>* freedBytes = nullptr);

// True when *path* is the Windows installation directory itself (usually
// C:\Windows). No direct action in the UI (move, delete, recycle) is allowed on
// that one path; what lives inside it stays reachable from the cleanup
// categories. Comparison is case-insensitive and ignores separators / trailing
// slashes.
bool isWindowsRoot(const QString& path);

// Rename / move a path within the same volume using MoveFileW. This is the
// atomic, instant Win32 rename — deliberately NOT QDir::rename(), which falls
// back to "copy everything, then delete the original" when the fast path is
// unavailable. That fallback would silently turn an instant rename into a
// multi-gigabyte copy. Returns false (leaving both paths untouched) when the
// target already exists or the two paths are on different volumes.
// *winError*, when given, receives the GetLastError() value on failure: 32 means
// a file inside the folder is open (the usual reason a rename of a live
// application's folder fails), 5 means access denied and 183 means the target
// already exists. The caller turns that into something the user can act on.
bool renamePath(const QString& from, const QString& to, quint32* winError = nullptr);

// True when *path* is a directory reparse point (a junction or a directory
// symlink). Cheap: one GetFileAttributes call, no handle needed.
bool isReparsePointAt(const QString& path);

// Total size of the files below *path*, never descending into reparse points
// (junctions / directory symlinks). Use this where the number must describe
// what a plain copy of the tree would move, rather than what the tree links to.
//
// When *visited is given every file adds its size to it the moment it is read,
// so a caller can poll "how much has been walked so far" while a huge tree is
// still being measured. The counter only ever grows.
qint64 dirSizeNoReparse(const QString& path, std::atomic<qint64>* visited = nullptr);

// Clean the CONTENTS of a directory (files and subdirectories) but keep the
// directory itself. Files that are locked/in-use are skipped silently.
// Returns (deletedCount, skippedCount). Use this for system directories
// (Temp, Logs, caches) that must not be removed themselves.
std::pair<int, int> cleanDirectoryContents(const QString& dirPath);

// Same as cleanDirectoryContents but moves the top-level children to the
// Recycle Bin instead of permanently deleting them (the directory itself is
// kept). Returns (movedCount, skippedCount). On non-Windows returns {0,0}.
std::pair<int, int> cleanDirectoryContentsToRecycleBin(const QString& dirPath);

// Reveal a file or folder in Windows Explorer (explorer /select,"path").
void revealInExplorer(const QString& path);

// Open a file or directory with its default associated application
// (ShellExecuteW "open").
void openPath(const QString& path);

// Runtime check: is the current process running elevated? The app manifest
// already requests requireAdministrator, so this is a defensive re-check.
bool isAdmin();

// Restart this program elevated ("runas"), preserving command-line arguments.
// Returns true when the elevated process was launched — the caller should then
// quit this instance. False when the user cancelled the UAC prompt or the
// launch failed.
bool relaunchAsAdmin();

// Names of the processes currently holding open handles on files under *dir*
// (Windows Restart Manager over up to *maxFiles* files, walking subdirectories).
// Returns how many locking processes were found and fills *procNames* (may be
// fewer than the return value when a name could not be resolved). An empty
// result on a failed rename does NOT mean "nobody holds it" — the handle may
// sit on a file beyond the sampling cap. A directory rename fails with
// ERROR_ACCESS_DENIED (5) exactly when such a handle exists, and no amount of
// elevation can close another process's handle — this is how the UI can say
// WHO is in the way.
int processesLockingDir(const QString& dir, QStringList* procNames, int maxFiles = 800);

// Drop the "do not delete / move / rename this folder" warning icon into *dir*.
// The icon is language-neutral; the FILE NAME is not — it is the sentence the
// user reads in Explorer, so it is taken from the active UI language
// (locales: app_move.marker_name). No-op when the folder is already marked in
// any shipped language, so switching language never leaves two icons behind.
// Returns false only when the file could not be created (read-only target).
bool writeDontDeleteMarker(const QString& dir);

// Every name the marker can have on disk, one per shipped language. Needed to
// tell "already marked, in the language that was active back then" apart from
// "not marked yet". The first entry is the active language's name.
QStringList dontDeleteMarkerNames();

// Query free / used / total bytes for the volume that contains *path* via
// GetDiskFreeSpaceExW. Returns (free, used, total); all zero on failure.
std::tuple<qint64, qint64, qint64> getDiskFreeSpace(const QString& path);

// Query the recycle bin on the volume containing *driveRoot* (e.g. "C:/").
// Returns (totalSize, itemCount); both zero on failure or non-Windows.
// Uses SHQueryRecycleBinW — the only reliable way to get the real recycle
// bin size, because $Recycle.Bin's per-SID subdirectories are not enumerable
// via QDir even with admin rights.
std::pair<qint64, qint64> queryRecycleBin(const QString& driveRoot);

// Empty the recycle bin on the volume containing *driveRoot*.
// Uses SHEmptyRecycleBinW. Returns true on success.
bool emptyRecycleBin(const QString& driveRoot);

} // namespace WinApi
