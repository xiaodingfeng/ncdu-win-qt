#pragma once

#include <QString>
#include <QStringList>
#include <tuple>

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

} // namespace WinApi
