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
#include <QFutureWatcher>
#include <memory>
#include <vector>
#include <tuple>

#include "FileNode.h"
#include "DiskScanner.h"
#include "MftScanner.h"
#include "CleanupTarget.h"
#include "CleanupScanner.h"
#include "CleanupWorker.h"

// Forward declarations - implementations are created by other tasks.
class TreemapWidget;
class BreadcrumbBar;
class LegendBar;
class CleanupPanel;
class SizeBarDelegate;

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
    void collectSearchResults(const std::shared_ptr<FileNode>& node, const QString& query,
                              std::vector<std::shared_ptr<FileNode>>& results, int& limit) const;
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
    void onCleanupProgress(const QString& label);
    void onCleanupItemDone(const QString& key, int deleted, int skipped, qint64 freed);
    void onCleanupFinished(int totalDeleted, int totalSkipped, qint64 totalFreed,
                           int totalItems,
                           std::vector<CleanupWorker::ItemRef> successItems,
                           std::vector<CleanupWorker::ItemRef> failedItems);

private:
    // ---- State ----
    std::shared_ptr<FileNode> m_root;
    std::shared_ptr<FileNode> m_current;
    QThread* m_scanner = nullptr;
    CleanupScanner* m_cleanupScanner = nullptr;
    CleanupWorker* m_cleanupWorker = nullptr;
    QString m_searchText;
    QString m_searchQueryPending;
    std::vector<std::shared_ptr<FileNode>> m_searchResults;
    bool m_inSearchMode = false;
    QTimer* m_searchDebounceTimer = nullptr;
    QFutureWatcher<std::vector<std::shared_ptr<FileNode>>>* m_searchWatcher = nullptr;
    bool m_showFiles = true;
    bool m_skipHeavyDirs = true;
    int m_sortCol = 2;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
    QString m_diskFreeText;
    QString m_lastScanPath;
    std::vector<CleanupTarget> m_cleanupTargets;
    std::vector<LargeFile> m_largeFiles;

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
    QTabWidget* m_rightTabs = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_hoverLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QProgressBar* m_progress = nullptr;

    // Menu actions (kept for retranslation).
    QMap<QString, QAction*> m_actions;

    // ---- Helpers ----
    void buildUI();
    void buildMenu();
    void wireSignals();
    void populatePathCombo();
    void setHeaderLabels();
    void startScan(const QString& path);
    void navigateTo(std::shared_ptr<FileNode> node);
    void goUp();
    void refresh();
    void toggleShowFiles();
    void populateList(std::shared_ptr<FileNode> node);
    std::vector<std::shared_ptr<FileNode>> sortChildren(
        const std::vector<std::shared_ptr<FileNode>>& children) const;
    QString buildRowTooltip(const std::shared_ptr<FileNode>& node) const;
    void updateStatusForCurrent();
    void updateDiskFreeLabel(const QString& path);
    void reflectSortIndicator();
    void retranslateUI();
    void switchLanguage(const QString& code);

    // Context-menu / actions
    void openPath(const QString& path);
    void revealInExplorer(const QString& path);
    void copyPath(const QString& path);
    void showProperties(const std::shared_ptr<FileNode>& node);
    void showAbout();
    void openHomepage();
    void openSourceUrl();
    void showSkippedMsg();

    // Deletion
    void collectDeletable(std::vector<std::shared_ptr<FileNode>>& deletable,
                          std::vector<std::shared_ptr<FileNode>>& rejected) const;
    void formatNames(const std::vector<std::shared_ptr<FileNode>>& nodes,
                     QString& names, QString& more) const;
    void recycleSelected();
    void deletePermanentSelected();
    void deletePermanentAsync(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void afterDelete(const std::vector<std::shared_ptr<FileNode>>& nodes);
    void recomputeSizes(std::shared_ptr<FileNode> node);
    std::shared_ptr<FileNode> findNodeByPath(const QString& path) const;
    bool pruneMissingFromNode(const std::shared_ptr<FileNode>& node);
    void syncTreeAfterCleanup(
        const std::vector<CleanupWorker::ItemRef>& successItems);

    // Cleanup
    void startCleanupScan(const QString& scanPath);
};
