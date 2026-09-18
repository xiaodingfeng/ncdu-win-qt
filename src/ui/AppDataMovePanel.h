#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <vector>

class QProcess;
class QProgressBar;
class QComboBox;
class QTimer;

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
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onPathClicked(QTreeWidgetItem* item, int column);
    void onHeaderClicked(int column);
    void onSizeReady();
    void onDeleteFinished();
    void onVerifyReady();
    void onCopyProbeReady();
    void onCopyTick();
    void onJobTick();
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
    QString stateKeyOf(int state) const;
    bool relocated(const DataDir& d) const;
    QString dstOf(const QString& root, const QString& name) const;
    int queueIndexOf(int dirIndex) const;
    // The folders the user ticked that can actually be queued for a move now.
    std::vector<int> collectMoveSelection() const;

    void runMoveStep();
    void finishMove();
    void startVerify();
    void runRestoreStep();
    void finishRestore();
    void setBusy(bool busy);
    void startProc(const QString& program, const QStringList& args);
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

    // move job
    std::vector<int> m_moveQueue;  // indices into m_dirs
    int m_movePos = 0;
    int m_moveOk = 0;
    int m_moveFailed = 0;
    bool m_moveActive = false;  // a move is running; further folders may be queued
    // The target root the current/last batch was aimed at; after a successful
    // move it receives the "do not delete" warning icon (360-style).
    QString m_batchTargetRoot;
    QProcess* m_proc = nullptr;
    QFutureWatcher<bool>* m_deleteWatcher = nullptr;
    QFutureWatcher<VerifyResult>* m_verifyWatcher = nullptr;
    bool m_deleteOk = false;
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
