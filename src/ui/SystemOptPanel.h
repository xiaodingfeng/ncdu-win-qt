#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QElapsedTimer>
#include <QStringList>

class QProcess;
class QTimer;
class QHBoxLayout;
class QScrollArea;
class ToggleSwitch;

// Outcome rules for one external command. They live outside the class so the
// netsh parsing can be unit-tested without spawning a process.
//
// netsh is localised and `netsh int ip reset` exits non-zero as soon as one
// sub-section reports a denial, while every other sub-section prints a success
// line. The output is therefore the only reliable signal.
bool stepOutputShowsSuccess(const QStringList& markers, const QString& output);
// True when the output says the change is staged and a restart finishes it
// (netsh ip/ipv6/winsock reset all print such a line).
bool stepOutputAsksForReboot(const QString& output);
// The success lines `netsh ... reset` prints for one sub-section, in every
// language this app ships. Exposed so a test can assert the real list rather
// than a copy of it.
QStringList netshResetSuccessMarkers();
// Builds a safe SDDL for a service whose security descriptor a third-party
// blocker ("Windows Update Blocker" and friends) has rewritten: deny ACEs are
// dropped and the standard allow set for SYSTEM / Administrators / interactive
// users is guaranteed, so an elevated process can configure and start the
// service again. An empty input falls back to the standard set outright.
// Exposed for the same reason as the netsh helpers: pure string logic.
QString repairServiceSddl(const QString& currentSddl);

class SystemOptPanel : public QWidget {
    Q_OBJECT
public:
    explicit SystemOptPanel(QWidget* parent = nullptr);

    void retranslate();
    void refreshTheme();
    void checkAutoUpdateStatus();

signals:
    // Asks the main window to open the "move app save folders" dialog.
    void requestAppPathSync();

protected:
    // The guard runs whenever the card becomes visible: that is exactly the
    // moment a block another program left behind matters to the user.
    void showEvent(QShowEvent* e) override;

private slots:
    void onToggleAutoUpdate();
    void onNetEmergencyFix();
    void onFlushDns();
    void onResetStore();
    void onOpenAppSync();

private:
    // One external command. Commands run detached from any console window and
    // strictly one after another, so the card can report real progress instead
    // of flashing a cmd window at the user.
    struct OptStep {
        QString labelKey;
        QString program;
        QStringList args;
        int timeoutMs = 60000;
        // Idempotent housekeeping commands (stopping a service that is already
        // stopped, deleting a value that is not there) must not be reported as
        // failures.
        bool allowFailure = false;
        // Some tools (netsh ip reset) exit non-zero even when their output shows
        // every section was reset. Finding any of these markers in the output
        // counts as success. Several markers are needed because netsh is
        // localised: English prints "Ok!", Simplified Chinese prints "完成!".
        QStringList successMarkers;
    };

    void buildUI();
    // `actionSlot` receives the layout the card's action widget must be added to.
    // `badgeLabel`, when given, receives a small highlight pill next to the title.
    QFrame* createCard(const QString& iconText,
                       QLabel*& titleLabel,
                       QLabel*& descLabel,
                       QLabel*& statusLabel,
                       QHBoxLayout*& actionSlot,
                       QLabel** badgeLabel = nullptr);
    QPushButton* makeCardButton(const QString& textKey);

    // Runs `steps` in the background, driving `status` and locking `btnKey`'s
    // button until the last step is done. `btn` may be null (switch-based card).
    void startSteps(const QString& jobName,
                    const QList<OptStep>& steps,
                    QLabel* status,
                    QPushButton* btn,
                    const QString& btnKey);
    void runNextStep();
    void finishSteps();
    bool isBusy() const { return m_jobActive; }
    void setStatus(QLabel* status, const QString& text, const char* color);
    void setActionsEnabled(bool enabled);

    // ---- Windows Update guard ----------------------------------------------
    // Turning updates on is a standing wish, not a one-off command: another
    // program can flip the same switches back later. The preference saying "I
    // want updates on" is therefore persisted, and while it is set the panel
    // repairs such a block on its own.
    void loadGuardSetting(bool systemBlocked);
    void saveGuardSetting(bool enabled);
    // Runs the guard: repairs an update block, but only when the user asked for
    // updates to stay on, and never twice within the cooldown.
    void maybeGuardAutoUpdate();
    // Executes the update configuration change. `automatic` marks a job the
    // guard started, so its outcome is reported as a repair rather than as the
    // answer to a switch the user just flipped.
    void runUpdateSteps(bool enable, bool automatic);
    // The steps for turning updates on or off, built from what is actually
    // wrong right now, so nothing is touched that does not need touching.
    // Non-const because building the enable steps may apply an in-place repair
    // of a blocker-locked service descriptor (see resetServiceSecurityNative).
    QList<OptStep> buildUpdateSteps(bool enable);

    // Keep the card list where the user left it across a dialog and the job it
    // starts (see the .cpp for why the position would move otherwise).
    void rememberScroll();
    void restoreScroll();

    // Widgets
    QLabel* m_headerTitle = nullptr;
    QLabel* m_headerDesc = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    int m_scrollPos = -1;

    // Card 1: Windows Update
    QFrame* m_cardUpdate = nullptr;
    QLabel* m_titleUpdate = nullptr;
    QLabel* m_descUpdate = nullptr;
    QLabel* m_statusUpdate = nullptr;
    ToggleSwitch* m_switchUpdate = nullptr;
    bool m_autoUpdateEnabled = true;
    // "Keep updates on even if something else turns them off."
    bool m_guardEnabled = false;
    bool m_guardLoaded = false;      // the preference has been read at least once
    bool m_autoRepair = false;       // the running job was started by the guard
    QStringList m_autoRepairReasons; // what the guard found, for the report
    QElapsedTimer m_guardCooldown;

    // Card 2: Network Fix
    QFrame* m_cardNet = nullptr;
    QLabel* m_titleNet = nullptr;
    QLabel* m_descNet = nullptr;
    QLabel* m_statusNet = nullptr;
    QPushButton* m_btnNet = nullptr;

    // Card 3: DNS Flush
    QFrame* m_cardDns = nullptr;
    QLabel* m_titleDns = nullptr;
    QLabel* m_descDns = nullptr;
    QLabel* m_statusDns = nullptr;
    QPushButton* m_btnDns = nullptr;

    // Card 4: Store Reset
    QFrame* m_cardStore = nullptr;
    QLabel* m_titleStore = nullptr;
    QLabel* m_descStore = nullptr;
    QLabel* m_statusStore = nullptr;
    QPushButton* m_btnStore = nullptr;

    // Card 5: App save folder migration
    QFrame* m_cardSync = nullptr;
    QLabel* m_titleSync = nullptr;
    QLabel* m_badgeSync = nullptr;   // "key update in this release" pill
    QLabel* m_descSync = nullptr;
    QLabel* m_statusSync = nullptr;
    QPushButton* m_btnSync = nullptr;

    // Background job state
    QList<OptStep> m_steps;
    int m_stepIndex = 0;
    QStringList m_stepErrors;
    // Set when any step's output asked for a restart (netsh ip reset does), so
    // the completion message can say the change only applies after a reboot.
    bool m_rebootNeeded = false;
    // Set when a locked service descriptor could only be reset through its
    // registry blob (applies at the next boot), so the update card can say so.
    bool m_sddlResetReboot = false;
    QProcess* m_proc = nullptr;
    QTimer* m_stepTimer = nullptr;
    QLabel* m_jobStatus = nullptr;
    QPushButton* m_jobButton = nullptr;
    QString m_jobBtnKey;
    QString m_jobName;
    bool m_jobActive = false;
    bool m_jobTouchesUpdate = false;
};