#pragma once

#include <QString>
#include <QVector>

#include "WinApi.h"

class QDialog;
class QWidget;

// The "these programs are using this folder" prompt, shared by both move paths
// (the app-data panel and the save-location dialog) so they behave identically.
//
// It exists because a failed move used to end in a dead end: the user was told
// the folder was busy but not by what, and had nothing to act on. This dialog
// names every process holding the folder open, says which of them the app may
// close and which it never will, and offers to close the closable ones so the
// move can carry on.
namespace LockerDialog {

// Object names the probe lab looks up, so the dialog can be checked without
// being shown (a modal window cannot be examined from the outside). The close
// button keeps the app-wide "primary" name, which is what the stylesheet keys
// its accent look off.
inline const char* kListName = "lockerList";
inline const char* kSkipName = "lockerSkip";
inline const char* kCancelName = "lockerCancel";
inline const char* kCloseName = "primary";

enum Answer {
    Cancel,            // stop here; touch nothing
    CloseAndContinue,  // close the closable ones, then retry the move
    Skip,              // leave this folder alone and carry on with the rest
};

// Fills *dlg* with the prompt but does not show it. ask() is this plus exec();
// splitting it out is what lets a test build the very same dialog and inspect
// it, which a modal window otherwise makes impossible.
//
// *intro* explains what just happened and what follows; it is the caller's
// sentence, because the two move paths fail in different words. *closeText* and
// *skipText* let each caller name its own consequence ("并继续搬移" vs "并重试
// 搬移"). When nothing in *procs* may be closed, the close button is left out
// and the answer can only be Skip or Cancel.
void populate(QDialog& dlg, const QVector<WinApi::LockingProcess>& procs,
              const QString& intro, const QString& closeText,
              const QString& skipText, bool allowSkip);

Answer ask(QWidget* parent, const QString& title, const QString& intro,
           const QVector<WinApi::LockingProcess>& procs,
           const QString& closeText, const QString& skipText, bool allowSkip);

// How many of *procs* the app is allowed to close, i.e. how many the close
// button would act on.
int closableCount(const QVector<WinApi::LockingProcess>& procs);

// One line for the log and for the caller's own follow-up message: what was
// closed, what had to be forced, and what was left alone.
QString outcomeText(const QVector<WinApi::CloseOutcome>& outcomes);

// True when at least one process actually went away — the only case in which
// retrying the move has a chance of getting further than it got last time.
bool anyClosed(const QVector<WinApi::CloseOutcome>& outcomes);

} // namespace LockerDialog
