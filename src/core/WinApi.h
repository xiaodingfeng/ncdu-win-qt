#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
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

// ---------------------------------------------------------------------------
// Who is holding a folder, and whether we may close them
// ---------------------------------------------------------------------------

// One process with an open handle on a file inside a folder we want to move —
// or one that is simply running from inside it. Restart Manager is the only API
// that answers "who holds this file", and it hands back a PID; the display name
// and the image path are resolved here so the user is shown something they
// recognise instead of a bare number. A folder that refuses to rename is often
// blocked by the second kind, which Restart Manager cannot see at all.
struct LockingProcess {
    quint32 pid = 0;
    QString name;      // application name, falling back to the executable name
    QString exePath;   // full path of the running image; empty when unresolvable
    // False when this process must never be closed by us. A move blocked by one
    // of those can only ask the user to deal with it.
    bool safeToClose = false;
    // Why it is not closable, as an I18n key (empty when safe). Held as a key
    // rather than as finished text so a language switch re-renders it.
    QString blockKey;
};

// Is *file* that folder, or anywhere beneath it? Case-insensitive and
// boundary-aware ("C:\Data2" is not inside "C:\Data"), normalising first and
// touching no file system, so the probe lab can check the awkward spellings.
bool pathInsideDirectory(const QString& file, const QString& dir);

// Who is holding *dir* open: the number of distinct processes found, with one
// entry each in *procs* (Restart Manager reports one entry per file a process
// holds, which is folded down to one per process).
//
// Two independent detectors, because either one alone leaves a hole. Restart
// Manager knows which processes hold files open, but only the files it was told
// about — the *maxFiles* newest are registered, since a program holding a file
// is holding one it just wrote — and only files, never a folder handle. The
// image scan then catches a program running from inside the folder, which is
// the strongest possible reason it will not rename.
//
// An empty result is therefore much stronger evidence than it used to be, but
// still not proof: a folder handle held by a process whose working directory is
// here is invisible to both.
int processesLockingDir(const QString& dir, QVector<LockingProcess>* procs,
                        int maxFiles = 800);

// Processes whose executable is running from inside *dir*. Entries are appended
// to *procs* (deduplicated by PID against what is already there) and the number
// found is returned. Kept separate from processesLockingDir so the narrow
// question stays answerable on its own.
int processesRunningFrom(const QString& dir, QVector<LockingProcess>* procs);

// The executable names this program refuses to close, lower case. Terminating
// any of them can take the session (or the machine) down, and none of them is
// ever the reason a user's data folder stays locked for long.
QStringList neverCloseNames();

// True when *nameOrPath* names one of those. Accepts either a bare file name or
// a full path, and either the friendly name or the executable name, because
// Restart Manager reports the former and the guard list holds the latter.
bool isNeverCloseName(const QString& nameOrPath);

// Ask *pid* to quit the way clicking its window's close button would: WM_CLOSE
// to every top-level window it owns. Returns how many windows were notified.
// This is the polite first step — it is what lets a program save its work.
int requestCloseProcess(quint32 pid);

// True while the process still exists. A process we are not allowed to open
// reads as gone, which is the safe answer: we could not have closed it anyway.
bool processAlive(quint32 pid);

// Force it. Only ever called for a process that ignored the polite request.
bool terminateProcess(quint32 pid, quint32* winError = nullptr);

// What happened to one process during one close round.
struct CloseOutcome {
    enum Result {
        Exited,    // gone: closed itself, or was already gone
        Killed,    // ignored the request and was terminated
        Refused,   // never touched (see *blockKey*)
        Survived,  // still running even after the forced attempt
    };
    quint32 pid = 0;
    QString name;
    Result result = Refused;
    QString blockKey;   // I18n key when result == Refused
};

// Closes every process in *procs*: request first, let them all shut down
// together for up to *graceMs*, then force whatever is left. Entries whose
// safeToClose is false come back as Refused and are never touched.
//
// The wait pumps the event loop in short slices, so the window keeps painting
// while a slow program saves its work instead of looking hung.
QVector<CloseOutcome> closeProcesses(const QVector<LockingProcess>& procs,
                                     int graceMs = 4000);

// Which of *procs* are programs from *closedImages*: ones a previous round in
// this batch got rid of and which are holding the folder again. That is a
// program that restarts itself — a helper process, a tray agent, a service —
// and it is the one case where closing again cannot possibly help, so the UI
// says so instead of asking a second time.
//
// *closedImages* holds image paths; the match is on the image path for both
// sides (a name is only used when a process has no readable path), because that
// is what survives a restart. Returns the display names, deduplicated.
QStringList respawnedAmong(const QVector<LockingProcess>& procs,
                           const QStringList& closedImages);

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
