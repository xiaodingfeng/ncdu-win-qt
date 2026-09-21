#pragma once

#include <QMainWindow>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QThread>
#include <QMap>
#include <QAction>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <functional>
#include <memory>
#include <vector>
#include <tuple>

#include "FileNode.h"
#include "DiskScanner.h"
#include "MftScanner.h"
#include "CleanupTarget.h"
#include "CleanupScanner.h"
#include "CleanupWorker.h"
#include "DuplicateScanner.h"
#include "WinApi.h"

// Forward declarations - implementations are created by other tasks.
class TreemapWidget;
class BreadcrumbBar;
class LegendBar;
class CleanupPanel;
class SystemOptPanel;
class SizeBarDelegate;
class AppDataMovePanel;
class AppPathSyncDialog;
class QFrame;
class QFile;
class QNetworkReply;
class AiService;
class AiAnalysisDialog;

// MainWindow - the application's primary window.
//
// MainWindow assembles the top bar
// (path selector + actions), breadcrumb + search toolbar, a splitter with the
// ncdu-style file list on the left and a tabbed right panel (treemap + cleanup)
// on the right, and a status bar. All user-facing strings go through I18n::tr
// so the language can be switched at runtime.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    // Scan
    void onScanClicked();
    void onBrowse();
    void onCancel();
    void onSkipToggled(bool checked);
    void onScanProgress(const QString& key, const QMap<QString, QString>& args);
    void onScanDone(std::shared_ptr<FileNode> root);
    void onScanError(const QString& key, const QMap<QString, QString>& args);

    // Navigation
    void onItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onHeaderClicked(int logicalIndex);
    void onContextMenu(const QPoint& pos);
    void onEscape();
    void onEnter();
    void onShowFilesToggled(bool checked);

    // Search
    void onSearchChanged(const QString& text);
    void onSearchDebounceTimeout();
    void populateSearchList();

    // Treemap
    void onTreemapHover(std::shared_ptr<FileNode> node);

    // Cleanup
    void onCleanupRescan();
    void onCleanTargets(const std::vector<std::tuple<QString, QString, QString>>& items);
    void onCleanupTargetScanned(CleanupTarget target);
    void onLargeFileFound(LargeFile lf);
    void onCleanupScanDone(std::vector<CleanupTarget> targets,
                           std::vector<LargeFile> largeFiles,
                           qint64 totalSize, int totalCount,
                           qint64 largeTotal);
    void onDupScanProgress(int phase, int processed, int total);
    void onDupScanDone(std::vector<DuplicateGroup> groups,
                       qint64 totalWasted, int totalFiles);
    void onCleanupProgress(const QString& label);
    void onCleanupItemDone(const QString& key, int deleted, int skipped, qint64 freed);
    void onCleanupFinished(int totalDeleted, int totalSkipped, qint64 totalFreed,
                           int totalItems,
                           std::vector<CleanupWorker::ItemRef> successItems,
                           std::vector<CleanupWorker::ItemRef> failedItems);

    // AI analysis
    void onAiSettings();
    void onAiAnalyzeCurrent();
    void onAiQa();
    void runAiAnalysis(const QString& subject, const QString& prompt);
    void runAiFollowUp(const QJsonArray& messages);

private:
    // ---- State ----
    std::shared_ptr<FileNode> m_root;
    std::shared_ptr<FileNode> m_current;
    QThread* m_scanner = nullptr;
    CleanupScanner* m_cleanupScanner = nullptr;
    DuplicateScanner* m_dupScanner = nullptr;
    CleanupWorker* m_cleanupWorker = nullptr;
    QString m_searchText;
    QString m_searchQueryPending;
    std::vector<std::shared_ptr<FileNode>> m_searchResults;
    bool m_inSearchMode = false;
    QTimer* m_searchDebounceTimer = nullptr;
    QFutureWatcher<std::vector<std::shared_ptr<FileNode>>>* m_searchWatcher = nullptr;
    bool m_showFiles = true;
    bool m_skipHeavyDirs = true;
    bool m_evictionEnabled = true;
    int m_sortCol = 2;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
    QString m_diskFreeText;
    QString m_lastScanPath;
    bool m_scanLowMemory = false;   // set when a scanner emits a low-memory warning
    // Wall-clock time the last scan actually took, measured from the moment the
    // scanner thread was started to the moment its tree arrived. Shown next to
    // the scan summary in the status bar; -1 while no scan has completed.
    QElapsedTimer m_scanClock;
    qint64 m_lastScanMs = -1;
    std::vector<CleanupTarget> m_cleanupTargets;
    std::vector<LargeFile> m_largeFiles;
    std::vector<DuplicateGroup> m_duplicateGroups;

    // ---- UI controls ----
    QComboBox* m_pathCombo = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QPushButton* m_browseBtn = nullptr;
    QPushButton* m_upBtn = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QLineEdit* m_searchBox = nullptr;
    QCheckBox* m_skipCheckbox = nullptr;
    BreadcrumbBar* m_breadcrumb = nullptr;
    QTreeWidget* m_tree = nullptr;
    TreemapWidget* m_treemap = nullptr;
    LegendBar* m_legend = nullptr;
    CleanupPanel* m_cleanupPanel = nullptr;
    SystemOptPanel* m_systemOptPanel = nullptr;
    QTabWidget* m_rightTabs = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_hoverLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QProgressBar* m_progress = nullptr;

    // Auto-update toast (bottom-right notification).
    QFrame* m_updateToast = nullptr;
    QLabel* m_updateToastTitle = nullptr;
    QLabel* m_updateToastBody = nullptr;
    QProgressBar* m_updateToastProgress = nullptr;
    QLabel* m_updateToastStatus = nullptr;
    QPushButton* m_updateToastInstallBtn = nullptr;
    QPushButton* m_updateToastCloseBtn = nullptr;
    QNetworkReply* m_updateDownloadReply = nullptr;
    std::unique_ptr<QFile> m_updateDownloadFile;
    QString m_updateRemoteVer;

    // Destination of the last successful "move to another drive" operation,
    // reused as the suggested folder when syncing app save paths afterwards.
    QString m_lastMoveTarget;

    // The "move an application's data folder" tab, created on first use and owned
    // by this window rather than by the dialog that shows it: a copy can take
    // longer than the dialog stays open, and closing the dialog must not kill it.
    AppDataMovePanel* m_movePanel = nullptr;

    // The save-location dialog is shown non-modally, so at most one may exist at
    // a time; this is the one currently up (null once it is closed). QPointer
    // because the dialog deletes itself when it is closed.
    QPointer<AppPathSyncDialog> m_syncDialog;

    // Menu actions (kept for retranslation).
    QMap<QString, QAction*> m_actions;

    QNetworkAccessManager* m_nam = nullptr;

    // AI service (optional analysis feature).
    AiService* m_ai = nullptr;
    QPointer<AiAnalysisDialog> m_aiDialog;
    QString m_aiModel;   // last model used for analysis (reused for follow-ups)

    // Pre-flight state (see guardThen). One guarded operation at a time: two
    // probes in flight would attach one answer to the other one's action.
    QFutureWatcher<QVector<WinApi::LockingProcess>>* m_guardWatcher = nullptr;
    std::function<void()> m_guardAction;
    QStringList m_guardPaths;
    // Image paths of the programs a close round got rid of: the only way to tell
    // "a program is holding it" apart from "a program restarts itself", which is
    // the one case where closing them again cannot possibly help.
    QStringList m_guardClosedImages;
    bool m_guardOffered = false;
    bool m_guardFatal = true;

    // ---- Helpers ----
    void buildUI();
    void buildMenu();
    void wireSignals();
    void populatePathCombo();
    void setHeaderLabels();
    void startScan(const QString& path);
    void navigateTo(std::shared_ptr<FileNode> node);
    void evictOffPathSubtrees();
    void tryEvictSubtree(std::shared_ptr<FileNode>& node);
    void goUp();
    void refresh();
    void populateList(std::shared_ptr<FileNode> node);
    std::vector<std::shared_ptr<FileNode>> sortChildren(
        const std::vector<std::shared_ptr<FileNode>>& children) const;
    QString buildRowTooltip(const std::shared_ptr<FileNode>& node) const;
    void updateStatusForCurrent();
    void updateDiskFreeLabel(const QString& path);
    void updateScannedStatus();
    void reflectSortIndicator();
    void retranslateUI();
    void switchLanguage(const QString& code);
    void switchTheme(const QString& code);
    void refreshTheme();

    // Context-menu / actions
    void openPath(const QString& path);
    void revealInExplorer(const QString& path);
    void copyPath(const QString& path);
    void moveToOtherDrive(const std::shared_ptr<FileNode>& node);
    void showAppPathSync();
    void showProperties(const std::shared_ptr<FileNode>& node);
    void showAbout();
    void showThemeCustomize();
    void checkForUpdate(bool silent = false);
    void openHomepage();
    void showSkippedMsg();

    // Auto-update toast.
    void buildUpdateToast();
    void showUpdateToast(const QString& remoteVer);
    void repositionUpdateToast();
    void applyUpdateToastStyle();
    void closeUpdateToast();
    void retranslateUpdateToast();
    void downloadAndInstall();
    void onUpdateDownloadReady();
    void onUpdateDownloadProgress(qint64 received, qint64 total);
    void onUpdateDownloadFinished();

    // Deletion
    void collectDeletable(std::vector<std::shared_ptr<FileNode>>& deletable,
                          std::vector<std::shared_ptr<FileNode>>& rejected) const;
    void formatNames(const std::vector<std::shared_ptr<FileNode>>& nodes,
                     QString& names, QString& more) const;
    // Asks, then hands over to doRecycle once the pre-flight has cleared the way.
    void recycleSelected();
    void doRecycle(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void deletePermanentSelected();
    // The guarded entry point, kept as such so the Recycle Bin's fallback is
    // checked over as well; the work itself is in doDeletePermanent.
    void deletePermanentAsync(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void doDeletePermanent(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void afterDelete(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void recomputeSizes(std::shared_ptr<FileNode> node);
    std::shared_ptr<FileNode> findNodeByPath(const QString& path) const;
    bool pruneMissingFromNode(const std::shared_ptr<FileNode>& node);
    // Bring the tree back in line with the disk for what an operation touched:
    // drop the rows that are gone, re-measure the ones that are not. The node
    // form is the one to reach for — the caller already holds them, and there is
    // then nothing that can fail to resolve.
    void resyncNodesFromDisk(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void resyncPathsFromDisk(const QStringList& paths);
    void syncTreeAfterCleanup(const std::vector<CleanupWorker::ItemRef>& items);

    // The pre-flight every destructive operation runs before it touches the disk
    // (see OpGuard). Fatal refuses to touch anything when the programs holding
    // the paths cannot be closed; advisory names them and carries on.
    void guardThen(const QStringList& paths, bool fatal, std::function<void()> action);
    void runGuardProbe();
    void onGuardProbed();
    void finishGuard(bool proceed);

    // The move itself, and the cleanup run itself: both start only after the
    // pre-flight above has had its say.
    void startSafeMove(const std::shared_ptr<FileNode>& node, const QString& targetDir);
    void startCleanup(const std::vector<std::tuple<QString, QString, QString>>& items,
                      CleanupWorker::DeleteMode mode);

    // Cleanup
    void startCleanupScan(const QString& scanPath);

    // AI helpers
    QString buildDirPrompt(const std::shared_ptr<FileNode>& node) const;
    QString buildQaSystemPrompt() const;
};
