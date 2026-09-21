// locker_dialog_probe (run8): builds the "these programs are using this folder"
// prompt headlessly and checks the parts that decide what happens to a real
// process — which rows are listed, whether the close button is offered at all,
// and what each button reports back. It also writes a PNG so the wording and the
// layout can be checked by eye.
//
// Nothing is shown: the dialog is modal, and showing it would block on a click
// nobody is there to make. Layout is activated by hand and the widget rendered
// with grab(), which needs no window.
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>

#include "I18n.h"
#include "LockerDialog.h"
#include "Logger.h"
#include "WinApi.h"

static FILE* gLog = nullptr;
static int gPass = 0, gFail = 0;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

static WinApi::LockingProcess proc(quint32 pid, const QString& name, bool safe,
                                   const QString& blockKey = QString())
{
    WinApi::LockingProcess p;
    p.pid = pid;
    p.name = name;
    p.exePath = QStringLiteral("C:/Program Files/") + name;
    p.safeToClose = safe;
    p.blockKey = blockKey;
    return p;
}

// The dialog's own list, by object name — a modal window cannot be poked at from
// the outside, which is exactly why it is built without being shown.
static QTreeWidget* listOf(QDialog& dlg)
{
    return dlg.findChild<QTreeWidget*>(QString::fromLatin1(LockerDialog::kListName));
}

static QPushButton* buttonOf(QDialog& dlg, const char* name)
{
    return dlg.findChild<QPushButton*>(QString::fromLatin1(name));
}

// The warning line is the only label whose colour is not the body colour.
static QString warnTextOf(QDialog& dlg)
{
    const QStringList all = { QStringLiteral("proc_close.warn_lost"),
                             QStringLiteral("proc_close.warn_none") };
    for (QLabel* l : dlg.findChildren<QLabel*>())
        for (const QString& key : all)
            if (l->text() == I18n::tr(key))
                return l->text();
    return QString();
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Logger::init();
    gLog = fopen("locker_dialog_probe.log", "w");

    // persist=false: probing must not leave the app itself in a language the
    // user did not choose.
    I18n::setLanguage(QStringLiteral("zh"), false);

    // ---------------------------------------------------------------------
    // The pure part first: how a close round is summarised.
    // ---------------------------------------------------------------------
    {
        WinApi::CloseOutcome a; a.pid = 1; a.name = QStringLiteral("TIM.exe");
        a.result = WinApi::CloseOutcome::Exited;
        WinApi::CloseOutcome b; b.pid = 2; b.name = QStringLiteral("微信");
        b.result = WinApi::CloseOutcome::Killed;
        WinApi::CloseOutcome c; c.pid = 3; c.name = QStringLiteral("explorer.exe");
        c.result = WinApi::CloseOutcome::Refused;
        const QVector<WinApi::CloseOutcome> round = { a, b, c };

        CHECK(LockerDialog::anyClosed(round), "anyClosed: a closed process counts");
        CHECK(LockerDialog::anyClosed({ a }), "anyClosed: one polite exit is enough");
        CHECK(!LockerDialog::anyClosed({ c }), "anyClosed: a refusal is not a close");

        WinApi::CloseOutcome stuck; stuck.pid = 4; stuck.name = QStringLiteral("stubborn.exe");
        stuck.result = WinApi::CloseOutcome::Survived;
        CHECK(!LockerDialog::anyClosed({ stuck }), "anyClosed: a survivor is not a close");

        const QString text = LockerDialog::outcomeText(round);
        CHECK(text.contains(QStringLiteral("TIM.exe")), "outcomeText names the closed program");
        CHECK(text.contains(QStringLiteral("微信")), "outcomeText names the forced one");
        CHECK(text.contains(QStringLiteral("explorer.exe")), "outcomeText names the refused one");
    }

    // ---------------------------------------------------------------------
    // Two closable programs and one that never may be: the common real case.
    // ---------------------------------------------------------------------
    const QVector<WinApi::LockingProcess> mixed = {
        proc(4242, QStringLiteral("TIM.exe"), true),
        proc(4243, QStringLiteral("WeChat.exe"), true),
        proc(1000, QStringLiteral("explorer.exe"), false,
             QStringLiteral("proc_close.block_system")),
    };
    CHECK(LockerDialog::closableCount(mixed) == 2,
          "closableCount: two of the three may be closed");

    {
        QDialog dlg;
        dlg.setWindowTitle(I18n::tr(QStringLiteral("app_move.title")));
        LockerDialog::populate(dlg, mixed,
                               I18n::tr(QStringLiteral("app_move.locked_intro"),
                                        QMap<QString, QString>{{"name", QStringLiteral("TIM")}}),
                               I18n::tr(QStringLiteral("app_move.btn_close_continue")),
                               I18n::tr(QStringLiteral("app_move.btn_skip_dir")),
                               /*allowSkip=*/true);
        dlg.adjustSize();
        if (QLayout* lay = dlg.layout())
            lay->activate();

        QTreeWidget* list = listOf(dlg);
        CHECK(list != nullptr, "the prompt has a list of programs");
        CHECK(list && list->topLevelItemCount() == 3, "one row per process");

        // The protected one has to be readable as such, not silently omitted:
        // the user is the only one who can deal with it.
        bool protectedShown = false;
        if (list) {
            for (int i = 0; i < list->topLevelItemCount(); ++i) {
                QTreeWidgetItem* it = list->topLevelItem(i);
                if (it->text(0) == QStringLiteral("explorer.exe")) {
                    protectedShown = it->text(2)
                        == I18n::tr(QStringLiteral("proc_close.block_system"));
                }
            }
        }
        CHECK(protectedShown, "a protected program is listed with the reason");

        // The PID is what the user needs to match a row against Task Manager.
        bool pidShown = false;
        if (list) {
            for (int i = 0; i < list->topLevelItemCount(); ++i)
                if (list->topLevelItem(i)->text(1) == QStringLiteral("4242"))
                    pidShown = true;
        }
        CHECK(pidShown, "each row carries its pid");

        QPushButton* close = buttonOf(dlg, LockerDialog::kCloseName);
        QPushButton* skip = buttonOf(dlg, LockerDialog::kSkipName);
        QPushButton* cancel = buttonOf(dlg, LockerDialog::kCancelName);
        CHECK(close != nullptr, "the close button is offered when something may be closed");
        CHECK(skip != nullptr, "the skip button is offered when the caller allows it");
        CHECK(cancel != nullptr, "cancelling is always possible");
        CHECK(!warnTextOf(dlg).isEmpty(), "the consequence of closing is spelled out");

        // What each button reports back: this is what the callers act on, so it
        // is the one thing that must not be wrong.
        if (close) {
            close->click();
            CHECK(dlg.result() == LockerDialog::CloseAndContinue,
                  "the close button reports CloseAndContinue");
        }
        if (skip) {
            skip->click();
            CHECK(dlg.result() == LockerDialog::Skip, "the skip button reports Skip");
        }
        if (cancel) {
            cancel->click();
            CHECK(dlg.result() == LockerDialog::Cancel, "the cancel button reports Cancel");
        }

        const QPixmap shot = dlg.grab();
        shot.save(QStringLiteral("locker_dialog_zh.png"));
        CHECK(!shot.isNull() && shot.width() > 400,
              "the prompt renders to a PNG for review");
    }

    // ---------------------------------------------------------------------
    // Nothing may be closed: the close button must not be there at all, or the
    // user would be promised something the app will refuse to do.
    // ---------------------------------------------------------------------
    {
        const QVector<WinApi::LockingProcess> allProtected = {
            proc(1000, QStringLiteral("explorer.exe"), false,
                 QStringLiteral("proc_close.block_system")),
            proc(1001, QStringLiteral("SearchIndexer.exe"), false,
                 QStringLiteral("proc_close.block_system")),
        };
        CHECK(LockerDialog::closableCount(allProtected) == 0,
              "closableCount: none of them may be closed");

        QDialog dlg;
        dlg.setWindowTitle(I18n::tr(QStringLiteral("app_move.title")));
        LockerDialog::populate(dlg, allProtected,
                               I18n::tr(QStringLiteral("app_sync.locked_intro"),
                                        QMap<QString, QString>{{"name", QStringLiteral("下载")}}),
                               I18n::tr(QStringLiteral("app_sync.btn_close_retry")),
                               QString(), /*allowSkip=*/false);
        dlg.adjustSize();
        if (QLayout* lay = dlg.layout())
            lay->activate();

        CHECK(buttonOf(dlg, LockerDialog::kCloseName) == nullptr,
              "no close button when nothing may be closed");
        CHECK(buttonOf(dlg, LockerDialog::kSkipName) == nullptr,
              "no skip button when the caller does not allow one");
        CHECK(buttonOf(dlg, LockerDialog::kCancelName) != nullptr,
              "only dismissing is left, and it is offered");
        CHECK(listOf(dlg) && listOf(dlg)->topLevelItemCount() == 2,
              "the protected programs are still named");

        // Rendered too. This is the screen for the case that used to be silent:
        // the folder really is held, nothing may be closed, and the user was
        // previously told only that "a program is using it". Showing the names
        // and the reason is the whole point of the dialog existing.
        const QPixmap shot = dlg.grab();
        CHECK(!shot.isNull() && shot.save(QStringLiteral("locker_dialog_noclose_zh.png")),
              "the nothing-may-be-closed prompt renders to a PNG for review");
        QLabel* warn = nullptr;
        for (QLabel* l : dlg.findChildren<QLabel*>()) {
            if (!l->text().isEmpty() && l->text() != I18n::tr(QStringLiteral("app_sync.locked_intro"),
                                                              QMap<QString, QString>{
                                                                  {"name", QStringLiteral("下载")}}))
                if (l->text().contains(QStringLiteral("手动")))
                    warn = l;
        }
        CHECK(warn != nullptr, "the nothing-may-be-closed prompt says what to do instead");
    }

    // ---------------------------------------------------------------------
    // The same prompt, wearing its other hat: the copy this app could not
    // delete after a restore. Same machinery, different words — the data is
    // already home, so what is at stake is a folder Explorer will not remove.
    // Rendered as well, because this screen is the whole answer to "the target
    // folder is still there and says a program is using it".
    // ---------------------------------------------------------------------
    {
        const QVector<WinApi::LockingProcess> holders = {
            proc(5150, QStringLiteral("Tencent.exe"), true, QString()),
            proc(5151, QStringLiteral("explorer.exe"), false,
                 QStringLiteral("proc_close.block_system")),
        };
        const QString residuePath = QStringLiteral("D:\\c-remove\\AppData\\Roaming\\Tencent");

        QDialog dlg;
        dlg.setWindowTitle(I18n::tr(QStringLiteral("app_move.title")));
        LockerDialog::populate(
            dlg, holders,
            I18n::tr(QStringLiteral("app_move.residue_intro"),
                     QMap<QString, QString>{{"name", QStringLiteral("Tencent")},
                                            {"path", residuePath}}),
            I18n::tr(QStringLiteral("app_move.btn_close_clean")),
            QString(), /*allowSkip=*/false);
        dlg.adjustSize();
        if (QLayout* lay = dlg.layout())
            lay->activate();

        CHECK(buttonOf(dlg, LockerDialog::kSkipName) == nullptr,
              "the leftover prompt offers no 'skip': there is no folder to skip");
        CHECK(buttonOf(dlg, LockerDialog::kCloseName) != nullptr,
              "the leftover prompt offers to close the holders");
        CHECK(buttonOf(dlg, LockerDialog::kCloseName)
                  && buttonOf(dlg, LockerDialog::kCloseName)->text()
                         == I18n::tr(QStringLiteral("app_move.btn_close_clean")),
              "the leftover prompt's close button says what it will actually do");

        // The path has to be on screen: it is the only way the user can tell
        // which folder is about to be deleted.
        bool pathShown = false;
        for (QLabel* l : dlg.findChildren<QLabel*>()) {
            if (l->text().contains(residuePath))
                pathShown = true;
        }
        CHECK(pathShown, "the leftover prompt names the folder to be removed");

        const QPixmap shot = dlg.grab();
        CHECK(!shot.isNull() && shot.save(QStringLiteral("locker_dialog_residue_zh.png")),
              "the leftover prompt renders to a PNG for review");
    }

    // ---------------------------------------------------------------------
    // The prompt the restore shows before it touches anything, when a program
    // is still using the folder it would have to empty. It is the same dialog
    // again, and its button says something else again ("并搬回" instead of
    // "并继续搬移" or "并清理"): three jobs, three promises, one widget.
    // ---------------------------------------------------------------------
    {
        const QVector<WinApi::LockingProcess> holders = {
            proc(6200, QStringLiteral("DJI Studio.exe"), true, QString()),
            proc(6201, QStringLiteral("explorer.exe"), false,
                 QStringLiteral("proc_close.block_system")),
        };

        QDialog dlg;
        dlg.setWindowTitle(I18n::tr(QStringLiteral("app_move.title")));
        LockerDialog::populate(
            dlg, holders,
            I18n::tr(QStringLiteral("app_move.restore_locked_intro"),
                     QMap<QString, QString>{{"name", QStringLiteral("DJI Studio")}}),
            I18n::tr(QStringLiteral("app_move.btn_close_restore")),
            QString(), /*allowSkip=*/false);
        dlg.adjustSize();
        if (QLayout* lay = dlg.layout())
            lay->activate();

        CHECK(buttonOf(dlg, LockerDialog::kCloseName) != nullptr
                  && buttonOf(dlg, LockerDialog::kCloseName)->text()
                         == I18n::tr(QStringLiteral("app_move.btn_close_restore")),
              "the restore prompt's close button says it will move the data back");

        const QPixmap shot = dlg.grab();
        CHECK(!shot.isNull() && shot.save(QStringLiteral("locker_dialog_restore_zh.png")),
              "the restore prompt renders to a PNG for review");
    }


    // A locale file older than the code — which is exactly what happens when the
    // build does not refresh the copy sitting next to the executable — renders
    // every key added since as its own name on screen. A check that compares
    // I18n::tr() against the widget cannot see that, because both sides are the
    // key; this one compares against the key directly, so it can.
    // ---------------------------------------------------------------------
    for (const char* key : {"app_move.title", "app_move.locked_intro",
                            "app_move.locked_fail_intro",
                            "app_move.btn_close_continue", "app_move.btn_skip_dir",
                            "app_sync.locked_intro", "app_sync.btn_close_retry",
                            "proc_close.can_close", "proc_close.block_system",
                            "proc_close.block_self", "proc_close.warn_lost",
                            "proc_close.col_action",
                            // The leftover-cleanup round: a restore that could not
                            // delete the copy it made. Every one of these is new
                            // code whose strings only ever appear on screen, so a
                            // locale file older than the build turns them into
                            // their own key names right in front of the user.
                            "app_move.state_cleaning", "app_move.restore_done_residue",
                            "app_move.clean_residue", "app_move.clean_residue_tip",
                            "app_move.clean_residue_confirm", "app_move.clean_residue_done",
                            "app_move.clean_residue_failed",
                            "app_move.clean_residue_failed_unknown",
                            "app_move.clean_residue_cancelled", "app_move.clean_residue_gone",
                            "app_move.residue_before_move", "app_move.residue_intro",
                            "app_move.btn_close_clean", "app_move.locked_noclose",
                            "app_move.locked_unknown",
                            // The state filter: the funnel, the panel it opens and
                            // the line a list shortened to nothing shows instead of
                            // its totals.
                            "app_move.filter_title", "app_move.filter_all",
                            "app_move.filter_item", "app_move.filter_join",
                            "app_move.filter_tip", "app_move.filter_active",
                            "app_move.filter_empty",
                            // The restore's own refusal: a restore that would have
                            // to empty the original folder while a program is still
                            // using it does not start, and these are the words that
                            // say why — the one thing the user can act on.
                            "app_move.restore_locked_intro", "app_move.btn_close_restore",
                            "app_move.closed_retry_back", "app_move.fail_restore_locked",
                            "app_move.fail_restore_locked_unknown",
                            "app_move.fail_restore_respawn", "app_move.fail_restore_nolink"}) {
        const QString text = I18n::tr(QString::fromLatin1(key));
        CHECK(!text.isEmpty() && text != QString::fromLatin1(key),
              QStringLiteral("i18n: %1 renders as a word, not as its key")
                  .arg(QString::fromLatin1(key)).toUtf8().constData());
    }

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    std::_Exit(gFail == 0 ? 0 : 1);
}
