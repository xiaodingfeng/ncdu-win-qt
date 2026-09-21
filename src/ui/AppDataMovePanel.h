#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "MoveSelect.h"
#include "WinApi.h"

class QProcess;
class QProgressBar;
class QComboBox;
class QTimer;
class FilterHeaderView;
class StateFilterPopup;
struct StateFilterEntry;

// Moves any application's data folder (AppData / LocalAppData / Documents) to a
// different drive and leaves a directory junction behind, so software that has
// no "change the save folder" setting can still be relocated. Works for every
// installed application instead of a fixed list.
class AppDataMovePanel : public QWidget {
    Q_OBJECT
public:
    explicit AppDataMovePanel(QWidget* parent = nullptr);
    // A move keeps running while the tab is out of sight, which means the panel
    // can be destroyed in the middle of one (the window is being closed).
    // Teardown has to deal with the helper process before anything else goes.
    ~AppDataMovePanel() override;

    void retranslate();
    void refreshTheme();

    // The target folder the user picked last time (empty when they never picked
    // one). Kept so both this panel's picker and the save-location dialog open
    // where the user left off instead of at "My Computer" every time.
    QString lastTargetRoot() const { return m_lastTargetRoot; }
    // Where a "pick a target drive" dialog would open: the last folder used, or
    // this user's home folder the first time. Public so the save-location dialog
    // and this panel cannot disagree about it.
    QString pickerStartDir() const;
    // Remembers a target folder this panel did not pick itself (the
    // save-location dialog picks roots too), so the next picker opens there.
    void rememberTargetRoot(const QString& root);

    // Outcome of the "is the copy complete?" check. Both numbers are measured
    // after the copy finished, never reused from the scan. Public because the
    // worker-thread helper that fills it in is a free function.
    struct VerifyResult {
        qint64 src = -1;
        qint64 dst = -1;
    };

    // The I18n key of a row state. Public because the state column is sized from
    // the longest of these names, and a check that the column really holds them
    // has to walk the same list rather than its own copy of it.
    static QString stateKeyOf(int state);

    // What the worker that measures both trees reports while it works. Shared
    // with that worker rather than owned by the panel alone: a worker still
    // walking a tree when the panel is destroyed must not write into memory the
    // panel has already released.
    struct WalkProgress {
        std::atomic<qint64> srcBytes{0};  // files seen under the original
        std::atomic<qint64> dstBytes{0};  // ...and under the copy
        std::atomic<int> stage{0};        // 0 = the original, 1 = the copy
    };

protected:
    // The full scan walks every AppData folder, so it only starts when the tab
    // is actually opened instead of when the dialog is built.
    void showEvent(QShowEvent* event) override;

private slots:
    void onScan();
    void onMoveSelected();
    void onRestore(int row);
    // Drops a leftover copy this app failed to delete (see DataDir::residueTarget):
    // asks who is holding it, offers to close them, and deletes it for real.
    void onCleanResidue(int row);
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onPathClicked(QTreeWidgetItem* item, int column);
    void onHeaderClicked(int column);
    void onSizeReady();
    void onDeleteFinished();
    void onVerifyReady();
    void onCopyProbeReady();
    void onCopyTick();
    void onJobTick();
    void onLockProbeReady();
    void onScopeChanged(int index);
    void onAddScope();
    void onRemoveScope();
    void hideProgressWhenIdle();

private:
    struct DataDir {
        QString path;        // current location on disk
        QString name;        // folder name / matched application name
        QString matchedApp;  // DisplayName of the installed program, if matched
        // Where the junction points to, once the folder has been relocated.
        // Non-empty means the folder only exists on the other drive.
        QString linkTarget;
        // Destination picked for the move that is queued or running for this
        // folder. Kept per folder rather than in one shared field, so further
        // folders can be queued onto a different destination while a move is
        // already running.
        QString moveTarget;
        // "<path>.ncduwin-old": the original, renamed aside once the copy has
        // been verified. The rename is atomic, which is what lets the original
        // stay intact until the junction is actually in place.
        QString oldPath;
        // A copy of this folder's data that this app put on another drive and
        // then could not delete — because a program still had a file inside it
        // open. The data itself is home: this is only the leftover, and the
        // folder it names is one this app is responsible for. Keeping the record
        // is the whole point: it is what turns "a folder with that name already
        // exists" into "that is our own leftover, here is how to clear it".
        QString residueTarget;
        qint64 size = -1;    // -1 while still unknown
        int state = 0;       // see kState* in the .cpp
        // How far this folder's relocation got, as recorded in the journal
        // (kPhase*, 0 = nothing in flight). Recovery replays from here.
        int journalPhase = 0;
        bool checked = false;  // survives a re-sort / rebuild of the list
        // True only when this app recorded the relocation (this session or the
        // journal). "Restore" deletes the folder on the other drive, so it is
        // offered for our own moves only — never for a link somebody else made.
        bool knownMove = false;
        // Why the last attempt on this folder failed, held as an I18n key plus
        // its arguments rather than as finished text, so switching language
        // re-renders it instead of leaving the old language behind. Empty when
        // nothing has failed. Shown in the progress row and as the row's
        // tooltip — a failed move that says only "失败" leaves the user with
        // nothing to act on.
        QString failKey;
        QMap<QString, QString> failArgs;
        // True once this session has offered to close the programs holding this
        // folder, and the user said yes. Without it a folder that is held open
        // by something we cannot close would ask again after every failed
        // retry, turning one stubborn process into an endless loop of prompts.
        bool closeAttempted = false;
        // Image paths of the programs we actually got rid of when that offer
        // was accepted. A program that shows up holding the folder again is one
        // that restarted itself (a helper process, a tray agent, a service), and
        // that is a different answer for the user than "some program is using
        // it": no number of further close rounds will ever finish the move.
        // Identified by image path rather than by name, because that is what
        // survives a restart.
        QStringList closedImages;
    };

    // One repair the recovery pass wants to perform on an interrupted move.
    struct RepairItem {
        int dirIndex = -1;   // index into m_dirs, -1 when there is no row
        QString linkAt;      // create the junction here (empty = not needed)
        QString linkTo;      // ...pointing there
        QStringList drop;    // delete these once the link is in place
        // The entry only recorded a copy that never became a relocation: drop it
        // and put the folder back to "movable" instead of "needs attention".
        bool forget = false;
    };

    void buildUI();
    void collectCandidates();
    void startNextSizeJob();
    void refreshRow(int row);
    void refreshSummary();

    // Scan scope: which folders the scan walks. Defaults to the current user's
    // data folders; the user can point it at other directories instead.
    QStringList defaultScanRoots() const;
    void loadScopeSettings();
    void saveScopeSettings();
    void rebuildScopeCombo();

    // Scan progress: the size pass is the slow part of a scan, so the bar
    // reports how many folders have been measured so far.
    void beginScanProgress();
    void updateScanProgress();
    void finishScanProgress();

    // The list is sortable, so a tree row is not the same as the index of its
    // folder: m_order maps row -> index into m_dirs.
    void rebuildTree();
    void applySort();
    void setSort(int column, Qt::SortOrder order);
    bool rowLessThan(const DataDir& a, const DataDir& b) const;
    int rowOfDir(int dirIndex) const;
    // Widths of the two columns whose content is wording rather than data, both
    // measured from the labels that actually go in them in the current language.
    // Fixed numbers cannot work: "状态" and "清理残留" are one width in Chinese
    // and a very different one in English, and a column that is a few pixels
    // short does not look cramped, it clips a button mid-word.
    int stateColumnWidth() const;
    int actionColumnWidth() const;
    void applyColumnWidths();
    bool relocated(const DataDir& d) const;

    // ---- state filter ----
    // The state a row is really showing. "Waiting behind the move that is
    // running" belongs to the queue rather than to the folder, so it is worked
    // out here instead of being stored — and the filter has to see the very
    // same answer the row does, or it would hide rows that are on screen.
    int effectiveState(int dirIndex) const;
    bool passesStateFilter(int dirIndex) const;
    bool stateFilterActive() const;
    QSet<int> stateFilterSelection() const;
    // What the filter panel lists: every state that is actually in the list,
    // with how many folders are in it, plus any state the filter is already on
    // (a selection that quietly disappeared could not be switched off again).
    QVector<StateFilterEntry> stateFilterEntries() const;
    QString stateFilterTooltip() const;
    void onStateFilterRequested(const QPoint& globalPos);
    // Re-derives the visible rows from the selection. The list itself is left
    // untouched: rows are hidden, never dropped, so nothing else that indexes a
    // row by its folder has to learn about the filter.
    void applyStateFilter();
    QString dstOf(const QString& root, const QString& name) const;
    int queueIndexOf(int dirIndex) const;
    // The folders the user ticked that can actually be queued for a move now.
    std::vector<int> collectMoveSelection() const;

    void runMoveStep();
    void finishMove();
    void startVerify();
    void runRestoreStep();
    void finishRestore();
    // What a restore does once the original folder is known to be free: the
    // junction goes first, then whatever occupies the original path is measured
    // against the copy, and an empty path is simply copied into. Split out
    // because the precheck arrives here through its own detour — the question
    // "who is holding this folder" has to be answered before any of it starts.
    void startRestoreHandover(int dirIndex);
    // Remembers which programs this panel actually got rid of, by image path, so
    // a later round can tell "some program holds it" apart from "this program
    // starts itself again". Shared by the move's precheck and the restore's.
    void rememberClosedImages(DataDir& d, const QVector<WinApi::CloseOutcome>& outcomes);
    // Why a restore stopped because the original folder is still held: the named
    // holders when there are any, and the vaguer wording when there are none.
    // Both are actionable — "close DJI Studio" and "retry after a reboot" are
    // things a user can do, "the restore failed" is not.
    void failRestoreLocked(int dirIndex, const QVector<WinApi::LockingProcess>& procs);
    // The restore got the data home, but the copy on the other drive would not
    // go away. One offer to close whatever is holding it, then a second delete
    // attempt; either way the folder is recorded as this app's own leftover
    // rather than being rounded up to "done".
    void settleResidue();
    // The leftover-cleanup job: find who is holding the folder, offer to close
    // them, then delete. Its own small state machine because it is neither a
    // move nor a restore — nothing is copied, and the only thing at stake is
    // disk space and a folder name we want back.
    void startResidueCleanup(int dirIndex);
    void runCleanStep();
    void finishClean();
    void setBusy(bool busy);
    void startProc(const QString& program, const QStringList& args);

    // Starts the "who is holding this folder open?" query for the folder the job
    // is on. Runs on a worker thread — walking a large tree to sample it is IO,
    // and the UI thread has a progress bar to paint.
    void startLockProbe(const QString& dir);
    // What to do once the user has been told which programs hold the folder.
    enum LockerAnswer {
        LockerProceed,      // they are gone (or were never a problem): carry on
        LockerSkipFolder,   // leave this folder alone, continue with the rest
        LockerAbortBatch,   // stop the whole batch here
    };
    // Puts the prompt for the last probe's result on screen and, when the user
    // agrees, closes the closable programs. Never called before anything has
    // been copied, except by the failed hand-over, which has already copied.
    LockerAnswer askAboutLockers(int dirIndex, const QString& introKey);
    // *freed* (optional) is handed to the worker so it can accumulate the bytes
    // it removes; the shared_ptr keeps that counter alive for as long as the
    // worker may still be using it, independent of this panel's lifetime.
    void deleteTreeAsync(const QStringList& paths, int nextPhase,
                         const std::shared_ptr<std::atomic<qint64>>& freed = {});

    // Progress of the folder the job is working on right now: one bar for the
    // whole folder, split into weighted phases (copy -> verify -> hand over ->
    // drop the original) so it keeps moving through the steps that have no
    // percentage of their own, and never jumps backwards.
    void beginJobProgress(const DataDir& d);
    void setJobStep(int step);
    void markJobComplete();
    void endJobProgress();
    void updateJobProgress();
    int jobPerMille() const;

    // Records why the folder at *dirIndex* failed, keeps that reason for the row
    // to show, and puts it on the progress row (with the bar coloured as an
    // error) so the user is told what went wrong where the progress was, instead
    // of being handed a row that only says "failed". Logged as it is set.
    void failMove(int dirIndex, const QString& key, const QMap<QString, QString>& args = {});
    // Relays a helper process's own words (robocopy / mklink / rmdir) into the
    // log, so a failure that only the helper knows about still leaves a trace.
    QString logProcResult(const char* what);

    // A message for the user that must not be lost when the tab is closed: shown
    // right away while the panel is on screen, otherwise the next time it opens
    // (a modal box popping up over another window is worse than a short delay).
    void notify(const QString& titleKey, const QString& text);

    // Journal: where each relocated folder went, so a move that was interrupted
    // (crash, power loss, failed link) can still be finished or undone.
    void saveJournal();
    void removeJournalEntry(const QString& path);
    void loadJournal();
    // Replays every interrupted relocation recorded in the journal until the
    // folder and its data agree again. Never deletes the only surviving copy.
    void startRepair();
    void repairNext();
    void finishRepair();

    std::vector<DataDir> m_dirs;
    std::vector<int> m_order;
    QStringList m_installedNames;
    // "<name>.ncduwin-old" folders found by the scan. They are kept out of the
    // list and handed to the recovery pass instead.
    QStringList m_orphanOlds;
    int m_sizeJobRow = -1;  // index into m_dirs
    int m_sortColumn = 2;   // "占用空间"
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;

    // Which states the list is limited to, indexed by state VALUE (they run
    // 0..kStateCount-1 without a gap). All false means no filter, which is the
    // reason this is a fixed array rather than a QSet: "nothing selected" has
    // to be a state of its own, telling "show everything" apart from "show
    // nothing".
    std::array<bool, MoveSelect::kStateCount> m_stateFilter{};

    // move job
    std::vector<int> m_moveQueue;  // indices into m_dirs
    int m_movePos = 0;
    int m_moveOk = 0;
    int m_moveFailed = 0;
    // Folders the user told us to leave alone because a program was using them.
    // Counted apart from failures: nothing went wrong, the user chose to wait.
    int m_moveSkipped = 0;
    bool m_moveActive = false;  // a move is running; further folders may be queued
    // The target root the current/last batch was aimed at; after a successful
    // move it receives the "do not delete" warning icon (360-style).
    QString m_batchTargetRoot;
    QProcess* m_proc = nullptr;
    QFutureWatcher<bool>* m_deleteWatcher = nullptr;
    QFutureWatcher<VerifyResult>* m_verifyWatcher = nullptr;
    bool m_deleteOk = false;
    // The processes the last probe found holding the folder open, and the worker
    // that found them. The probe result is read by whoever launched it, which is
    // either the pre-copy check or a failed hand-over.
    QFutureWatcher<QVector<WinApi::LockingProcess>>* m_lockWatcher = nullptr;
    QVector<WinApi::LockingProcess> m_lockProcs;
    // True when the last helper process could not be launched at all. Its exit
    // code is meaningless in that case (it reads as 0 = success).
    bool m_procFailedToStart = false;
    int m_movePhase = 0;  // step counter shared by the move and the restore job
    // Where the shared verify worker hands control back when it finishes. The
    // worker serves the move, the restore and the restore's own assessment, so
    // the step it resumes is recorded when it is launched rather than guessed.
    int m_verifyNext = 0;

    // restore job
    int m_restoreRow = -1;  // index into m_dirs
    bool m_restoreActive = false;
    bool m_restoreFailed = false;
    // Path of a leftover the restore could not delete. Non-empty turns the
    // closing message from "restored" into "restored, and here is what is still
    // on the other drive" — which is the difference between a user who knows
    // what happened and one who finds out from Explorer.
    QString m_restoreResidue;
    // How many times the restore has offered to close the programs holding the
    // original folder. One offer, then a refusal: a program that is holding the
    // folder again after being closed is one that restarts itself, and asking a
    // second time would end exactly where the first round did.
    int m_restoreAskRound = 0;

    // leftover-cleanup job
    int m_cleanRow = -1;  // index into m_dirs
    bool m_cleanActive = false;
    // What the row was before the cleanup borrowed its state. The cleanup shows
    // "dropping a leftover" while it runs, and a finished job must hand the row
    // back rather than leave it looking busy for ever.
    int m_cleanPrevState = MoveSelect::kStateIdle;
    // Why the cleanup gave up, as an I18n key plus arguments, so the row can say
    // what stands in the way instead of only that it failed.
    QString m_cleanFailKey;
    QMap<QString, QString> m_cleanFailArgs;

    // recovery pass
    QVector<RepairItem> m_repairs;
    int m_repairPos = 0;
    int m_repairStep = 0;
    int m_repairLinked = 0;
    int m_repairDropped = 0;
    bool m_repairActive = false;

    QLabel* m_tipLabel = nullptr;
    // Amber warning block at the top of the tab: the marker icon + the wording.
    // The icon is the very file a finished move drops into the destination, so
    // the user recognises it on the drive before ever seeing it on the disk.
    QWidget* m_warnBar = nullptr;
    QLabel* m_warnIcon = nullptr;
    QLabel* m_warnLabel = nullptr;   // "prefer per-app moves" warning, amber
    QLabel* m_summaryLabel = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QPushButton* m_moveBtn = nullptr;
    QTreeWidget* m_tree = nullptr;
    // The tree's own header, kept typed so its funnel's highlight and tooltip
    // can be driven from here.
    FilterHeaderView* m_header = nullptr;
    QFutureWatcher<qint64>* m_sizeWatcher = nullptr;
    bool m_busy = false;
    bool m_scanned = false;

    // Scan scope widgets
    QLabel* m_scopeLabel = nullptr;
    QComboBox* m_scopeCombo = nullptr;
    QPushButton* m_scopeAddBtn = nullptr;
    QPushButton* m_scopeRemoveBtn = nullptr;
    // Directories to scan right now (what the selected scope expands to).
    QStringList m_scanRoots;
    // User-added directories, kept across sessions (the default is implicit).
    QStringList m_customRoots;
    // Which scope entry is selected (0 = the built-in default).
    int m_scopeIndex = 0;
    // Target root of the last move, reused the next time a target is picked.
    QString m_lastTargetRoot;

    // Progress row, shared by the scan and the copy. The bar itself is the app's
    // slim text-less style, so the wording lives in m_scanStatus next to it.
    QWidget* m_progressRow = nullptr;
    QLabel* m_scanStatus = nullptr;
    QProgressBar* m_progress = nullptr;
    QTimer* m_progressHideTimer = nullptr;
    int m_sizeTotal = 0;
    int m_sizeDone = 0;
    bool m_scanning = false;

    // Job progress: one bar for the folder being moved, shared by every phase of
    // it. m_jobStep says which phase, and the counters it reads come either from
    // the destination sampler (copy) or from the worker's shared counters
    // (verify, cleanup) — never from the UI thread doing its own measuring.
    bool m_jobActive = false;
    // Set when the job stopped because something went wrong. The progress row
    // then stays on screen showing the reason instead of being overwritten by the
    // next tick or hidden as if the job had finished.
    bool m_jobFailed = false;
    int m_jobStep = 0;        // kJob* in the .cpp
    qint64 m_jobTotal = -1;   // bytes this folder holds, from the scan
    qint64 m_jobDone = 0;     // bytes confirmed at the destination
    int m_jobPerMille = 0;    // highest value drawn so far for this folder
    QElapsedTimer m_jobClock;
    std::shared_ptr<WalkProgress> m_walk;
    std::shared_ptr<std::atomic<qint64>> m_freed;
    // Cheap, IO-free refresh of the wording (elapsed time, phase name).
    QTimer* m_jobTimer = nullptr;
    // Low-rate sampler of the destination: seeing the copy grow is what gives the
    // copy phase its percentage.
    QTimer* m_copyTimer = nullptr;
    QFutureWatcher<qint64>* m_copyProbeWatcher = nullptr;
    qint64 m_copyProbeStartMs = 0;  // when the walk now in flight was started

    // Held back until the panel is on screen again (see notify()).
    QString m_pendingNoticeTitle;
    QString m_pendingNotice;
};
