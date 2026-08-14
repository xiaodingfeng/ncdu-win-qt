#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QComboBox>
#include <QTabWidget>
#include <vector>
#include <memory>
#include <tuple>

#include "CleanupTarget.h"
#include "CleanupWorker.h"
#include "DuplicateScanner.h"

// CleanupPanel - panel showing cleanup targets by S/A/B/C/D safety level and
// large files.
//
// CleanupPanel provides the disk cleanup UI. Uses a QTabWidget with
// two tabs (Categories + Large Files), a header with title and free-space
// summary, a scan progress bar (hidden by default), and a bottom bar with the
// selected-size label, "rescan" and "clean" buttons.
//
// Signals
//   cleanRequested      - emitted with (type, key, path) tuples for checked items
//   rescanRequested     - emitted when the user clicks "rescan"
//   pathRevealRequested - emitted when the user clicks a large-file path column
class CleanupPanel : public QWidget {
    Q_OBJECT
public:
    explicit CleanupPanel(QWidget* parent = nullptr);

    void startScanProgress();
    void stopScanProgress();
    void addTarget(const CleanupTarget& target);
    void addLargeFile(const LargeFile& lf);
    void addDuplicateGroup(const DuplicateGroup& group);

    // Duplicate-scan progress UI (status label + determinate progress bar in
    // the Duplicates tab). Driven by DuplicateScanner::progress signals.
    void setDupScanStarted();
    void setDupScanProgress(int phase, int processed, int total);
    void setDupScanDone(int groups, int files);
    void loadTargets(const std::vector<CleanupTarget>& targets,
                     const std::vector<LargeFile>& largeFiles,
                     qint64 freeBytes, qint64 totalBytes);
    void loadDuplicates(const std::vector<DuplicateGroup>& groups);
    void removeCleanedItems(const std::vector<CleanupWorker::ItemRef>& items);
    void setCleaning(bool cleaning);
    void updateFreeSpace(qint64 freeBytes, qint64 totalBytes);
    void retranslate();
    void refreshTheme();

signals:
    void cleanRequested(const std::vector<std::tuple<QString, QString, QString>>& items);
    void rescanRequested();
    void pathRevealRequested(const QString& path);
    void aiAnalyzeRequested(const QString& subject, const QString& prompt);

private slots:
    void onCatItemChanged();
    void onLfItemChanged();
    void onLfItemClicked(QTreeWidgetItem* item, int column);
    void onCatSelectAllToggled(bool checked);
    void onLfSelectAllToggled(bool checked);
    void onCleanClicked();
    void onDupItemChanged();
    void onDupItemClicked(QTreeWidgetItem* item, int column);
    void onDupSelectAllToggled(bool checked);
    void onDupSmartSelectClicked();
    void onLfTypeFilterChanged(int index);
    void onDupTypeFilterChanged(int index);
    void onCatItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onAiContextMenu(QTreeWidget* tree, const QPoint& pos);
    void onAiAnalyzeSelected();

private:
    QTreeWidget* m_catTree = nullptr;
    QTreeWidget* m_lfTree = nullptr;
    QTreeWidget* m_dupTree = nullptr;
    QTabWidget* m_tabs = nullptr;
    QPushButton* m_cleanBtn = nullptr;
    QPushButton* m_rescanBtn = nullptr;
    QPushButton* m_aiBtn = nullptr;
    QPushButton* m_catSelBtn = nullptr;
    QPushButton* m_lfSelBtn = nullptr;
    QPushButton* m_dupSelBtn = nullptr;
    QPushButton* m_dupSmartBtn = nullptr;
    QComboBox* m_lfTypeFilter = nullptr;
    QComboBox* m_dupTypeFilter = nullptr;
    QLabel* m_lfTypeLabel = nullptr;
    QLabel* m_dupTypeLabel = nullptr;
    QProgressBar* m_scanProgress = nullptr;
    QLabel* m_selectedLabel = nullptr;
    QLabel* m_totalLabel = nullptr;
    QLabel* m_dupStatus = nullptr;        // dup-scan phase/progress/summary text
    QProgressBar* m_dupProgress = nullptr; // determinate bar for dup scan phases 2/3
    bool m_cleaning = false;
    std::vector<CleanupTarget> m_targets;
    std::vector<LargeFile> m_largeFiles;
    std::vector<DuplicateGroup> m_duplicateGroups;
    qint64 m_freeBytes = 0;
    qint64 m_totalBytes = 0;
    int m_dupResultGroups = 0;  // last dup-scan group count (for retranslate)
    int m_dupResultFiles = 0;   // last dup-scan file count (for retranslate)
    bool m_dupScanning = false; // true while a dup scan is running
    bool m_dupScanCompleted = false; // true once a dup scan has finished

    void buildUI();
    QTreeWidget* makeTree(const QStringList& headers, bool col3Fixed = false);
    void applyTargetTranslation(QTreeWidgetItem* item, const CleanupTarget& target);
    void applyLargeFileWarning(QTreeWidgetItem* item, const QString& level);
    void updateSummary(qint64 freeBytes, qint64 totalBytes);
    void updateSelectedLabel();
    void updateTotalLabel();
    void applyLfTypeFilter();
    void applyDupTypeFilter();
    void repopulateTypeFilters();
    void retranslateDupStatus();
    std::vector<std::tuple<QString, QString, QString>> getCheckedTargets() const;
    std::vector<std::tuple<QString, QString, QString>> getCheckedLargeFiles() const;
    std::vector<std::tuple<QString, QString, QString>> getCheckedDuplicates() const;
    qint64 getCheckedTotalSize() const;
    QString largeFileWarning(const QString& level) const;
    QString buildCleanupItemPrompt(const QString& category, const QString& path,
                                   qint64 size, int items, const QString& level,
                                   const QString& remark) const;
    QString buildCleanupBatchPrompt() const;
};
