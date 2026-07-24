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
bool deletePermanent(const QStringList& paths);

// Clean the CONTENTS of a directory (files and subdirectories) but keep the
// directory itself. Files that are locked/in-use are skipped silently.
// Returns (deletedCount, skippedCount). Use this for system directories
// (Temp, Logs, caches) that must not be removed themselves.
std::pair<int, int> cleanDirectoryContents(const QString& dirPath);

// Reveal a file or folder in Windows Explorer (explorer /select,"path").
void revealInExplorer(const QString& path);

// Open a file or directory with its default associated application
// (ShellExecuteW "open").
void openPath(const QString& path);

// Runtime check: is the current process running elevated? The app manifest
// already requests requireAdministrator, so this is a defensive re-check.
bool isAdmin();

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
