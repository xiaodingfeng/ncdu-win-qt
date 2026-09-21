// ui_render_probe (run6): renders the "software data folders" tab and the
// relocation dialog headlessly and saves them as PNGs, so the amber warning
// block (icon + wording) and the detected folder rows can be checked by eye
// instead of trusted. The panel is rendered once per language, because the
// wording is language-specific and the English one is easy to get wrong.
//
// It also checks MoveSelect::mayStartMove, the rule behind the move button.
// That part is logic rather than looks, and it is what decides whether an
// interrupted move can be finished or whether the user is pushed into the
// operation that undoes it.
//
// Nothing is shown: showEvent() would start a real scan (and the journal repair
// pass) against the live profile. Layout is activated by hand and the widgets
// are rendered with grab(), which needs no window.
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <cstdio>

#include "AppDataMovePanel.h"
#include "AppPathSyncDialog.h"
#include "FilterHeaderView.h"
#include "I18n.h"
#include "Logger.h"
#include "MoveSelect.h"

static FILE* gLog = nullptr;
static int gPass = 0, gFail = 0;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

// The amber block's own label, fetched by walking the bar it lives in.
static QLabel* warnLabelOf(AppDataMovePanel& panel)
{
    QWidget* bar = panel.findChild<QWidget*>(QStringLiteral("warnBar"));
    if (!bar)
        return nullptr;
    // The first label in the bar is the icon; the wording is the one with text.
    for (QLabel* l : bar->findChildren<QLabel*>()) {
        if (!l->text().isEmpty())
            return l;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Logger::init();
    gLog = fopen("ui_render_probe.log", "w");

    // persist=false throughout: rendering the tab in two languages must not
    // leave the app itself switched to whichever one ran last.
    I18n::setLanguage(QStringLiteral("zh"), false);

    AppDataMovePanel panel;
    panel.resize(980, 420);
    if (QLayout* lay = panel.layout())
        lay->activate();

    const QPixmap shot = panel.grab();
    const QString out = QDir::currentPath() + QStringLiteral("/ui_render_probe.png");
    const bool saved = !shot.isNull() && shot.save(out, "PNG");
    fprintf(gLog, "[INFO] rendered %dx%d -> %s\n", shot.width(), shot.height(),
            out.toUtf8().constData());

    CHECK(saved, "panel renders to a PNG");
    // The pixmap is in DEVICE pixels: a 2.25x display legitimately reports more.
    const QSize logical(qRound(shot.width() / shot.devicePixelRatio()),
                        qRound(shot.height() / shot.devicePixelRatio()));
    fprintf(gLog, "[INFO] logical size %dx%d (dpr %.2f)\n",
            logical.width(), logical.height(), shot.devicePixelRatio());
    CHECK(logical == QSize(980, 420), "rendered at the requested logical size");
    CHECK(QFileInfo(out).size() > 5000, "PNG has real content");

    // ---- the warning block --------------------------------------------------
    // The icon stays (it is the marker that lands in the folder); the wording
    // describes the FOLDER RULE, not the icon file.
    auto* warnIcon = panel.findChild<QLabel*>(QStringLiteral("warnIcon"));
    CHECK(warnIcon && !warnIcon->pixmap().isNull(), "warning block shows the marker icon");

    QLabel* warnLabel = warnLabelOf(panel);
    CHECK(warnLabel != nullptr, "warning block has a wording label");
    const QString zhWarn = warnLabel ? warnLabel->text() : QString();
    fprintf(gLog, "[INFO] zh warning: %s\n", zhWarn.toUtf8().constData());
    CHECK(!zhWarn.contains(QStringLiteral(".ico")),
          "Chinese warning does not describe the marker file");
    CHECK(zhWarn.contains(QStringLiteral("重命名")) && zhWarn.contains(QStringLiteral("同步")),
          "Chinese warning forbids renaming the destination (data desync)");

    const QString enWarn = I18n::trIn(QStringLiteral("en"), QStringLiteral("app_move.warn"));
    fprintf(gLog, "[INFO] en warning: %s\n", enWarn.toUtf8().constData());
    CHECK(!enWarn.contains(QStringLiteral(".ico")),
          "English warning does not describe the marker file");
    CHECK(enWarn.contains(QStringLiteral("rename"), Qt::CaseInsensitive)
              && enWarn.contains(QStringLiteral("sync"), Qt::CaseInsensitive),
          "English warning forbids renaming the destination (data desync)");

    // Render the English tab as well — the wording the user asked about.
    I18n::setLanguage(QStringLiteral("en"), false);
    panel.retranslate();
    if (QLayout* lay = panel.layout())
        lay->activate();
    const QString enOut = QDir::currentPath() + QStringLiteral("/ui_render_probe_en.png");
    CHECK(panel.grab().save(enOut, "PNG"), "panel renders to a PNG (English)");
    warnLabel = warnLabelOf(panel);
    const QString enWarnUi = warnLabel ? warnLabel->text() : QString();
    fprintf(gLog, "[INFO] en warning as rendered: %s\n", enWarnUi.toUtf8().constData());
    CHECK(enWarnUi == enWarn, "switching language re-renders the warning in English");
    I18n::setLanguage(QStringLiteral("zh"), false);   // leave the state as found

    CHECK(panel.findChild<QTreeWidget*>(QStringLiteral("moveTree")) != nullptr,
          "panel exposes its folder tree");

    // ---- the two columns that carry wording --------------------------------
    // The state and action columns are sized from the text that goes in them,
    // and a column a few pixels short does not look cramped — it clips a button
    // mid-word, which is what a fixed width does the moment the labels change
    // language. So both are measured here against the real labels, in both
    // languages, rather than eyeballed on a screenshot. Each language is measured
    // after a retranslate(), because that is the only thing that re-measures: a
    // column sized once for whichever language ran first is the bug this checks.
    auto checkColumnWidths = [&](const char* lang) -> int {
        auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("moveTree"));
        if (!tree) {
            CHECK(false, "the folder list has a header to size");
            return 0;
        }
        QHeaderView* hdr = tree->header();
        CHECK(hdr != nullptr, "the folder list has a header");

        // The widest thing the row button can say, measured on a button styled
        // exactly like that row's own.
        QPushButton sample;
        sample.setObjectName(QStringLiteral("ghost"));
        sample.setStyleSheet(QStringLiteral("font-size: 11px; padding: 1px 8px;"));
        sample.setFixedHeight(22);
        int widest = 0;
        QString widestKey;
        for (const char* key : {"app_move.restore", "app_move.abandon", "app_move.clean_residue"}) {
            sample.setText(I18n::tr(key));
            if (sample.sizeHint().width() > widest) {
                widest = sample.sizeHint().width();
                widestKey = QString::fromLatin1(key);
            }
        }

        // The longest state name, taken from the panel's own list.
        QFont f = tree->font();
        f.setPixelSize(12);
        f.setBold(true);
        const QFontMetrics fm(f);
        int longest = 0;
        QString longestKey;
        for (int s = 0; s < MoveSelect::kStateCount; ++s) {
            const QString key = AppDataMovePanel::stateKeyOf(s);
            const int w = fm.horizontalAdvance(I18n::tr(key));
            if (w > longest) {
                longest = w;
                longestKey = key;
            }
        }

        fprintf(gLog, "[INFO] %s: state column %d px (longest %s = %d px + %d funnel), "
                      "action column %d px (widest %s = %d px)\n",
                lang, hdr->sectionSize(3), longestKey.toUtf8().constData(), longest,
                FilterHeaderView::kFilterSlot, hdr->sectionSize(4),
                widestKey.toUtf8().constData(), widest);
        CHECK(hdr->sectionSize(3) >= longest + FilterHeaderView::kFilterSlot,
              "the state column holds its longest name and still leaves the funnel room");
        CHECK(hdr->sectionSize(4) >= widest,
              "the action column fits the widest button it can hold");
        return hdr->sectionSize(3);
    };
    I18n::setLanguage(QStringLiteral("en"), false);
    panel.retranslate();
    const int enStateCol = checkColumnWidths("en");
    I18n::setLanguage(QStringLiteral("zh"), false);
    panel.retranslate();
    const int zhStateCol = checkColumnWidths("zh");
    // English state names are the long ones, so the column has to come back down
    // when the wording does: a column that only ever grows is one that was sized
    // for the language that happened to be in force when the panel was built.
    CHECK(zhStateCol < enStateCol,
          "switching language re-measures the state column instead of keeping the wider one");

    // ---- the relocation tab, through the real dialog ------------------------
    // Not just a picture: the rows are listed, so "which folders are detected on
    // this machine" is part of the probe's verdict rather than a guess.
    // The dialog hosts the move panel too, so the tree is picked by name — a
    // bare findChild() returns whichever one was constructed first.
    AppPathSyncDialog paths;
    if (QLayout* lay = paths.layout())
        lay->activate();
    auto* tree = paths.findChild<QTreeWidget*>(QStringLiteral("syncTree"));
    CHECK(tree != nullptr, "relocation tab exposes its folder tree");
    int rows = 0;
    if (tree) {
        rows = tree->topLevelItemCount();
        for (int i = 0; i < rows; ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            fprintf(gLog, "[ROW] %s | %s\n",
                    item->text(0).toUtf8().constData(),
                    item->text(1).toUtf8().constData());
        }
    }
    fprintf(gLog, "[INFO] detected folder rows: %d\n", rows);
    CHECK(rows >= 6, "more than the five everyday folders are offered");

    const QPixmap pathsShot = paths.grab();
    const QString pathsOut = QDir::currentPath() + QStringLiteral("/ui_render_probe_paths.png");
    CHECK(!pathsShot.isNull() && pathsShot.save(pathsOut, "PNG"), "relocation tab renders to a PNG");
    fprintf(gLog, "[INFO] rendered -> %s\n", pathsOut.toUtf8().constData());

    // ---- which rows may start a move ----------------------------------------
    // The rule the panel's move button is wired to (MoveSelect.h), checked here
    // because it is the rule — not the button — that decides whether the user
    // can finish an interrupted move at all.
    CHECK(MoveSelect::mayStartMove(MoveSelect::kStateIdle, false, false),
          "a never-moved folder can be moved");
    CHECK(!MoveSelect::mayStartMove(MoveSelect::kStateMoved, true, true),
          "a relocated folder cannot be moved again");
    CHECK(!MoveSelect::mayStartMove(MoveSelect::kStateMoved, true, false),
          "a relocated folder whose target did not resolve is still not movable");
    CHECK(MoveSelect::mayStartMove(MoveSelect::kStateFailed, true, true),
          "an interrupted move can be started again, which is how it is finished");
    CHECK(MoveSelect::mayStartMove(MoveSelect::kStateFailed, false, false),
          "a folder that simply failed can be tried again");
    CHECK(!MoveSelect::mayStartMove(MoveSelect::kStateIdle, false, true),
          "a junction this app did not create is never moved");
    CHECK(!MoveSelect::mayStartMove(MoveSelect::kStateDone, true, true),
          "a finished move cannot be started again");
    // Distinct state values, not just distinct names. Two states sharing a
    // number would merge two different rows silently — the leftover-cleanup
    // state added on top of the others is the kind of thing that is easy to
    // assign over an existing one.
    CHECK(MoveSelect::kStateCleaning != MoveSelect::kStateIdle
              && MoveSelect::kStateCleaning != MoveSelect::kStateChecking
              && MoveSelect::kStateCleaning != MoveSelect::kStateFailed
              && MoveSelect::kStateCleaning != MoveSelect::kStateMoved,
          "the leftover-cleanup state has a value of its own");

    // ---- what a finished leftover cleanup leaves on the row -----------------
    // The cleanup borrows the "dropping a leftover" state while it runs, and a
    // job that does not hand it back leaves a row that says "working" for ever —
    // next to a message saying it is done.
    CHECK(MoveSelect::stateAfterCleanup(MoveSelect::kStateIdle, MoveSelect::kStateCleaning)
              == MoveSelect::kStateIdle,
          "a finished cleanup puts the row back the way it found it");
    CHECK(MoveSelect::stateAfterCleanup(MoveSelect::kStateFailed, MoveSelect::kStateCleaning)
              == MoveSelect::kStateFailed,
          "an interrupted move is still unfinished after its leftover is gone");
    CHECK(MoveSelect::stateAfterCleanup(MoveSelect::kStateIdle, MoveSelect::kStateRestoring)
              == MoveSelect::kStateRestoring,
          "no cleanup ran, so nothing about the row is touched");

    // ---- may a restore start emptying the original folder -------------------
    // The rule behind the restore's precheck (MoveSelect::restoreGate). It is
    // checked here because getting it wrong is not a cosmetic failure: starting
    // a restore while a program holds the folder empties it under the running
    // program, and the user is left with two half-copies and a folder Explorer
    // refuses to delete.
    CHECK(MoveSelect::restoreGate(/*blocked=*/false, /*closable=*/0, /*offered=*/false)
              == MoveSelect::kRestoreGo,
          "an untouched folder restores straight away");
    CHECK(MoveSelect::restoreGate(true, 2, false) == MoveSelect::kRestoreAsk,
          "a folder held by programs we may close gets one offer to close them");
    CHECK(MoveSelect::restoreGate(true, 0, false) == MoveSelect::kRestoreRefuse,
          "a folder held by programs we never close is refused, with nothing touched");
    CHECK(MoveSelect::restoreGate(true, 2, true) == MoveSelect::kRestoreRefuse,
          "the same program back after being closed is refused rather than asked again");
    CHECK(MoveSelect::restoreGate(false, 2, true) == MoveSelect::kRestoreGo,
          "a folder that is free again is restored");

    // ---- the state filter ---------------------------------------------------
    // The predicate behind the funnel. The empty selection is the case worth
    // checking: it is where the user ends up after unticking the last box, and a
    // filter that showed nothing there would read as a scan that lost the list.
    bool none[MoveSelect::kStateCount] = {};
    CHECK(MoveSelect::passesFilter(MoveSelect::kStateFailed, none),
          "no state selected shows every folder");

    bool failedOnly[MoveSelect::kStateCount] = {};
    failedOnly[MoveSelect::kStateFailed] = true;
    CHECK(MoveSelect::passesFilter(MoveSelect::kStateFailed, failedOnly),
          "a selected state passes the filter");
    CHECK(!MoveSelect::passesFilter(MoveSelect::kStateIdle, failedOnly),
          "an unselected state is filtered out");

    bool twoStates[MoveSelect::kStateCount] = {};
    twoStates[MoveSelect::kStateFailed] = true;
    twoStates[MoveSelect::kStateIdle] = true;
    CHECK(MoveSelect::passesFilter(MoveSelect::kStateIdle, twoStates)
              && MoveSelect::passesFilter(MoveSelect::kStateFailed, twoStates),
          "several states can be selected at once");
    CHECK(!MoveSelect::passesFilter(MoveSelect::kStateMoved, twoStates),
          "a state outside the selection stays filtered out");

    // Every state has to appear exactly once in the filter's order: a gap would
    // make a state unreachable from the panel, and a repeat would list it twice
    // with two ticks for the same thing.
    {
        int seen = 0;
        bool unique = true;
        for (int i = 0; i < MoveSelect::kFilterOrderCount; ++i) {
            const int s = MoveSelect::kFilterOrder[i];
            if (s < 0 || s >= MoveSelect::kStateCount || (seen & (1 << s))) {
                unique = false;
                break;
            }
            seen |= (1 << s);
        }
        CHECK(unique && seen == (1 << MoveSelect::kStateCount) - 1,
              "the filter lists every state exactly once");
    }

    fprintf(gLog, "[INFO] process exit is not the verdict for the PNG checks: %d failed\n", gFail);

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
