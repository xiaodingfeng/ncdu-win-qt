#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "WinApi.h"

// The pre-flight every destructive operation in this app runs before it touches
// the disk, and the wording of what it found.
//
// The software-data panel has asked "who is holding this folder, and may we
// close them?" from the start. Deleting, recycling, moving and cleaning from the
// rest of the app did not, and those are the places where the omission costs the
// most: a delete with one file held open loses everything else and keeps the
// folder, and a delete that fails says only "failed". These are the pieces those
// paths share, in one place so they cannot drift apart.
//
// Stateless on purpose: no widget, no dialog, no member. The dialog is
// LockerDialog; the flow that drives it lives in MainWindow, next to the
// progress bar and the user's answer.
namespace OpGuard {

// True when every program holding the folder open is one this app never closes
// — a system or security one. Restart Manager reports every process with a
// handle on a file in the tree, and a handle that merely READS (an indexer
// walking the folder, a scanner between two passes, Explorer painting a
// thumbnail) does not keep a folder from being deleted or replaced. Interrupting
// the user over a set of processes that nothing can be done about would make the
// prompt meaningless, so when those are the ONLY holders the job walks on and
// their names go to the log.
bool onlyBackgroundHolders(const QVector<WinApi::LockingProcess>& procs);

// The display names in *procs*, deduplicated, in the order they arrived.
QStringList namesOf(const QVector<WinApi::LockingProcess>& procs);

// True when at least one holder is worth stopping for: somebody other than a
// system program has the folder open, i.e. somebody the user can act on.
bool blocksOperation(const QVector<WinApi::LockingProcess>& procs);

// Who is holding any of *paths* open, as one deduplicated list. *paths* is
// narrowed first — duplicates and paths already covered by a broader one
// ("delete C:\A and C:\A\B") each cost a full tree walk — and paths that are not
// there are skipped, because a folder that has already gone cannot be holding
// anything open.
//
// This is the slow half of the pre-flight: Restart Manager registers the newest
// files under every path. Call it off the UI thread.
QVector<WinApi::LockingProcess> probe(const QStringList& paths, int maxFiles = 800);

// One line of the result list: what became of this item, and, when it is still
// there, why. *recycled* picks the wording for a Recycle-Bin run, where nothing
// has been freed yet — the files are in the bin, not gone.
QString resultLine(const WinApi::DeleteResult& r, bool recycled);

// True when every item either went away or was already gone, so there is nothing
// to tell the user beyond the status line.
bool allClean(const QVector<WinApi::DeleteResult>& results);

// The body of the result dialog: a header counting what went through, then one
// line per item that needs the user's attention. Items that simply worked are
// counted, not listed. Only ever called when !allClean().
QString resultBody(const QVector<WinApi::DeleteResult>& results, bool recycled);

// The paths in *results* that are still on disk — the rows the tree has to keep,
// with a size re-measured against the disk, rather than drop.
QStringList survivorsOf(const QVector<WinApi::DeleteResult>& results);

// The paths in *results* that are gone: the rows the tree has to drop. Includes
// the ones that were never there, since those rows are stale either way.
QStringList vanishedOf(const QVector<WinApi::DeleteResult>& results);

// The paths that were asked to go away and were not there in the first place.
// Worth saying out loud before anything else happens: the user is looking at a
// list that no longer matches the disk.
QStringList missingOf(const QVector<WinApi::DeleteResult>& results);

// The reason as a sentence, for the callers that have a reason without a
// DeleteResult — the cleanup worker reports its own.
QString reasonSentence(WinApi::DeleteReason reason, quint32 winError);

// What a set of results adds up to.
struct Tally {
    int ok = 0;      // items that went away, or were already gone
    int bad = 0;     // items that need the user's attention
    qint64 freed = 0;
};
Tally tally(const QVector<WinApi::DeleteResult>& results);

// True when the only thing wrong is that the items were already gone, i.e.
// there is nothing left to try. Decides whether the Recycle Bin's "delete them
// for real instead?" offer is worth making at all.
bool onlyAlreadyGone(const QVector<WinApi::DeleteResult>& results);

// The bullet list on its own, for callers that write their own headline (the
// Recycle-Bin fallback prompt does). Empty when everything worked.
QString resultLines(const QVector<WinApi::DeleteResult>& results, bool recycled);

} // namespace OpGuard
