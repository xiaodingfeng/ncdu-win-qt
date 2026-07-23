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
    void onLfTypeFilterChanged(int index);
    void onDupTypeFilterChanged(int index);

private:
    QTreeWidget* m_catTree = nullptr;
    QTreeWidget* m_lfTree = nullptr;
    QTreeWidget* m_dupTree = nullptr;
    QTabWidget* m_tabs = nullptr;
    QPushButton* m_cleanBtn = nullptr;
    QPushButton* m_rescanBtn = nullptr;
    QPushButton* m_catSelBtn = nullptr;
    QPushButton* m_lfSelBtn = nullptr;
    QPushButton* m_dupSelBtn = nullptr;
    QComboBox* m_lfTypeFilter = nullptr;
    QComboBox* m_dupTypeFilter = nullptr;
    QLabel* m_lfTypeLabel = nullptr;
    QLabel* m_dupTypeLabel = nullptr;
    QProgressBar* m_scanProgress = nullptr;
    QLabel* m_selectedLabel = nullptr;
    QLabel* m_totalLabel = nullptr;
    bool m_cleaning = false;
    std::vector<CleanupTarget> m_targets;
    std::vector<LargeFile> m_largeFiles;
    std::vector<DuplicateGroup> m_duplicateGroups;
    qint64 m_freeBytes = 0;
    qint64 m_totalBytes = 0;

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
    std::vector<std::tuple<QString, QString, QString>> getCheckedTargets() const;
    std::vector<std::tuple<QString, QString, QString>> getCheckedLargeFiles() const;
    std::vector<std::tuple<QString, QString, QString>> getCheckedDuplicates() const;
    qint64 getCheckedTotalSize() const;
    QString largeFileWarning(const QString& level) const;
};
