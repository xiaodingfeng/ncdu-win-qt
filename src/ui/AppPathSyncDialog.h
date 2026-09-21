#pragma once

#include <QDialog>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <vector>

class QTabWidget;
class AppDataMovePanel;

// One save folder offered by the dialog: a library folder Windows itself owns
// (Downloads, Documents, Desktop, Pictures, Videos) together with the registry
// value that defines where it lives.
struct AppSaveInfo {
    QString id;           // stable identifier, also selects the registry value
    QString nameKey;      // I18n key for the display name
    QString folder;       // language-independent subfolder name (batch move)
    QString currentPath;  // where the folder lives right now
};

// Lets the user move Windows' own library folders to another drive, and hosts
// the "move an application's data folder" tab.
class AppPathSyncDialog : public QDialog {
    Q_OBJECT
public:
    // *movePanel* is optional and stays owned by the caller. It must be passed in
    // once a move may already be running: a long copy has to survive the dialog
    // being closed, and a panel created here would be destroyed with it.
    explicit AppPathSyncDialog(QWidget* parent = nullptr,
                               const QString& suggestedPath = QString(),
                               AppDataMovePanel* movePanel = nullptr);
    ~AppPathSyncDialog() override;

    // Re-labels everything this dialog owns, including the borrowed move panel.
    // The dialog is modeless and reused, so a language change must reach an
    // already-open window instead of waiting for it to be reopened — and the
    // panel would never be rebuilt at all, because it belongs to the caller.
    void retranslate();

private slots:
    void onChangeSingleApp(int row);
    void onBatchSetTargetFolder();
    void onRescan();
    void onPathClicked(QTreeWidgetItem* item, int column);

private:
    void buildUI();
    void detectAllApps();
    void refreshTable();
    bool updateAppPath(const QString& appId, const QString& newPath);
    // Moves the contents of every (old -> new) pair in the background, with a
    // progress dialog. What Explorer does when a library folder's location
    // changes: same-volume moves are renames, cross-volume ones are copies
    // whose originals are removed file by file as soon as each is safely on
    // the new drive.
    // Moves *pairs* on a worker thread with a modal progress dialog. When the
    // move finishes (and was not cancelled) *doneNotice* is shown — passing it
    // in lets the caller skip the pre-move "changed OK" popup, so choosing
    // "move files" goes straight into the progress. *markerDir* receives the
    // "do not delete" warning icon after the move.
    //
    // *closeRound* counts how many times this move has already been re-run after
    // the user closed the programs holding files back; it stops the retry from
    // becoming a loop (see kMaxCloseRounds).
    void moveContentsAsync(const QVector<QPair<QString, QString>>& pairs,
                           const QString& doneNotice = QString(),
                           const QString& markerDir = QString(),
                           int closeRound = 0);
    // What a finished round has to say: the completion summary when everything
    // moved, or the programs still holding the leftovers — with the offer to
    // close them and carry on, which is the only way forward while the location
    // has already changed underneath the files.
    void reportMoveOutcome(const QVector<QPair<QString, QString>>& pairs,
                           const QString& doneNotice, const QString& markerDir,
                           int closeRound);

    QString m_suggestedPath;
    std::vector<AppSaveInfo> m_apps;

    QTreeWidget* m_tree = nullptr;
    QLabel* m_descLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QPushButton* m_batchBtn = nullptr;
    QPushButton* m_rescanBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QTabWidget* m_tabs = nullptr;
    AppDataMovePanel* m_movePanel = nullptr;
    // True when the panel was created here (nobody passed one in), in which case
    // it belongs to this dialog and goes away with it.
    bool m_ownsMovePanel = false;
    // Where a caller-owned panel is handed back to once the dialog closes.
    QWidget* m_movePanelOwner = nullptr;
};
