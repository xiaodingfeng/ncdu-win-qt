// lang_refresh_probe (run7): the language switch has to reach windows that are
// ALREADY open, not only the ones built afterwards.
//
// The bug this exists for: the "save location" dialog and the software-data
// panel are both long-lived (the dialog is modeless and reused, the panel is
// owned by the main window and merely borrowed), so after a language change
// they kept every label they had when they were first built — most visibly the
// whole "user data folders" tab.
//
// What is checked:
//   A. widget level — the real dialog, the real panel and the real AI window are
//      built in Chinese, the language is switched, the same calls the main
//      window makes are issued, and then EVERY user-visible string in those
//      windows is walked: not one may still contain a Chinese character. Text
//      that is data (paths, folder names discovered on this machine) is listed
//      but excluded, because it is not the app's to translate.
//   B. structure — every cached widget the main window holds (QPointer<...>
//      members) must be retranslated from retranslateUI(). New ones are covered
//      automatically: a cached dialog that forgets to relabel itself fails here.
//
// Nothing is shown (showEvent would start a real scan); layouts are activated by
// hand and the windows are rendered with grab(), which needs no window.
#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTreeWidget>
#include <cstdio>

#include "AiAnalysisDialog.h"
#include "AppDataMovePanel.h"
#include "AppPathSyncDialog.h"
#include "I18n.h"
#include "Logger.h"

static FILE* gLog = nullptr;
static int gPass = 0, gFail = 0;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

// Chinese ideographs / kana. Deliberately not "any non-ASCII": the em-dash used
// as the missing-path placeholder and the arrow glyphs in a few labels are fine
// in both languages, and flagging them would drown the real hits.
static bool hasCJK(const QString& s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if ((u >= 0x3400 && u <= 0x9FFF)      // CJK unified + extension A
            || (u >= 0xF900 && u <= 0xFAFF)   // compatibility ideographs
            || (u >= 0x3040 && u <= 0x30FF))  // hiragana / katakana
            return true;
    }
    return false;
}

// Every string a user can read in *root*. Data (tree/table row contents, combo
// entries) is logged separately by the caller so it cannot hide a real miss.
static QStringList textOf(QWidget* root, QWidget* w)
{
    QStringList out;
    if (w == root && !root->windowTitle().isEmpty())
        out << QStringLiteral("windowTitle: ") + root->windowTitle();
    if (auto* l = qobject_cast<QLabel*>(w)) {
        if (!l->text().isEmpty())
            out << QStringLiteral("QLabel(%1): %2").arg(l->objectName(), l->text());
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        if (!b->text().isEmpty())
            out << QStringLiteral("Button(%1): %2").arg(b->objectName(), b->text());
        if (!b->toolTip().isEmpty())
            out << QStringLiteral("ButtonTip(%1): %2").arg(b->objectName(), b->toolTip());
    }
    if (auto* g = qobject_cast<QGroupBox*>(w)) {
        if (!g->title().isEmpty())
            out << QStringLiteral("GroupBox: ") + g->title();
    }
    if (auto* e = qobject_cast<QLineEdit*>(w)) {
        if (!e->placeholderText().isEmpty())
            out << QStringLiteral("Placeholder: ") + e->placeholderText();
    }
    if (auto* t = qobject_cast<QTabWidget*>(w)) {
        for (int i = 0; i < t->count(); ++i)
            out << QStringLiteral("Tab: ") + t->tabText(i);
    }
    if (auto* tr = qobject_cast<QTreeWidget*>(w)) {
        if (QTreeWidgetItem* h = tr->headerItem()) {
            for (int i = 0; i < h->columnCount(); ++i)
                out << QStringLiteral("Header(%1): %2").arg(tr->objectName(), h->text(i));
        }
    }
    return out;
}

static QStringList visibleStrings(QWidget* root)
{
    QStringList out = textOf(root, root);
    for (QWidget* w : root->findChildren<QWidget*>())
        if (!w->isHidden())
            out += textOf(root, w);
    return out;
}

// Returns the offending strings, logs each one. Widgets that Qt has already
// taken out of the layout are skipped: a rebuild (refreshTable) deletes the old
// per-row buttons with deleteLater(), so between the rebuild and the next event
// loop the discarded copies are still children of the viewport — invisible, and
// on their way out. Reporting those would drown the real misses.
static QStringList cjkOffenders(QWidget* root, const char* what)
{
    QStringList bad;
    const QStringList all = visibleStrings(root);
    for (const QString& s : all) {
        if (hasCJK(s)) {
            bad << s;
            fprintf(gLog, "[CJK] %s -> %s\n", what, s.toUtf8().constData());
        }
    }
    return bad;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Logger::init();
    gLog = fopen("lang_refresh_probe.log", "w");

    // persist=false: switching languages in a probe must not change the app's
    // own setting.
    I18n::setLanguage(QStringLiteral("zh"), false);

    // The same ownership shape the main window uses: the panel belongs to the
    // caller and is only lent to the dialog.
    auto* panel = new AppDataMovePanel;
    AppPathSyncDialog paths(nullptr, QString(), panel);
    AiAnalysisDialog ai;

    auto activate = [](QWidget* w) {
        if (QLayout* l = w->layout())
            l->activate();
    };
    paths.resize(820, 560);
    activate(&paths);
    activate(&ai);

    // ---- control: in Chinese these windows really are full of Chinese -------
    const QStringList zhStrings = visibleStrings(&paths);
    int zhCjk = 0;
    for (const QString& s : zhStrings)
        if (hasCJK(s))
            ++zhCjk;
    fprintf(gLog, "[INFO] zh: %d strings, %d containing CJK\n", zhStrings.size(), zhCjk);
    CHECK(zhCjk >= 10, "control: the Chinese build really is Chinese");

    const QString zhShot = QDir::currentPath() + QStringLiteral("/lang_refresh_before.png");
    CHECK(paths.grab().save(zhShot, "PNG"), "rendered the dialog (before the switch)");

    // ---- switch language exactly the way the main window does ---------------
    I18n::setLanguage(QStringLiteral("en"), false);
    paths.retranslate();      // the dialog, which forwards to the borrowed panel
    panel->retranslate();     // and the panel on its own (it can be shown alone)
    ai.retranslate();
    activate(&paths);
    activate(&ai);

    const QString enShot = QDir::currentPath() + QStringLiteral("/lang_refresh_after.png");
    CHECK(paths.grab().save(enShot, "PNG"), "rendered the dialog (after the switch)");

    // A rebuild retires the old per-row buttons with deleteLater(); flush those
    // before walking the widget tree so the scan only sees what is on screen.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // ---- A. no readable string may still be Chinese -------------------------
    const QStringList bad = cjkOffenders(&paths, "paths dialog");
    for (const QString& s : bad)
        fprintf(gLog, "       >> %s\n", s.toUtf8().constData());
    CHECK(bad.isEmpty(), "no Chinese left in the relocation dialog");

    // …and the same for the buttons the rows actually carry, read off the live
    // item widget instead of through the widget walk.
    int rowButtons = 0, rowButtonsEn = 0;
    if (auto* tree = paths.findChild<QTreeWidget*>(QStringLiteral("syncTree"))) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (auto* b = qobject_cast<QAbstractButton*>(
                    tree->itemWidget(tree->topLevelItem(i), 2))) {
                ++rowButtons;
                if (!hasCJK(b->text()))
                    ++rowButtonsEn;
            }
        }
    }
    fprintf(gLog, "[INFO] row buttons: %d, of them without CJK: %d\n", rowButtons, rowButtonsEn);
    CHECK(rowButtons > 0 && rowButtons == rowButtonsEn,
          "every row's action button is relabelled");

    const QStringList badPanel = cjkOffenders(panel, "move panel");
    for (const QString& s : badPanel)
        fprintf(gLog, "       >> %s\n", s.toUtf8().constData());
    CHECK(badPanel.isEmpty(), "no Chinese left in the software-data panel");

    const QStringList badAi = cjkOffenders(&ai, "AI dialog");
    CHECK(badAi.isEmpty(), "no Chinese left in the AI dialog");

    // Spot check on the block the user actually complained about.
    QString warnText;
    if (QWidget* bar = panel->findChild<QWidget*>(QStringLiteral("warnBar"))) {
        for (QLabel* l : bar->findChildren<QLabel*>())
            if (!l->text().isEmpty())
                warnText = l->text();
    }
    CHECK(warnText == I18n::trIn(QStringLiteral("en"), QStringLiteral("app_move.warn")),
          "the amber warning block was relabelled");

    // ---- data is not translated, and that is expected -----------------------
    // Logged, not asserted: these come from the disk (paths, folder names).
    if (auto* tree = paths.findChild<QTreeWidget*>(QStringLiteral("syncTree"))) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            fprintf(gLog, "[ROW] %s | %s\n", tree->topLevelItem(i)->text(0).toUtf8().constData(),
                    tree->topLevelItem(i)->text(1).toUtf8().constData());
        }
    }

    // ---- B. every cached widget is reachable from retranslateUI -------------
    // The main window holds the long-lived widgets; anything it caches must be
    // relabelled there, because nothing else ever reconstructs it. Reading the
    // header keeps this honest for widgets added later: a cached dialog that
    // forgets to relabel itself shows up here without anyone remembering to
    // extend the probe.
    const QString srcDir = QStringLiteral(NCDU_SRC_DIR);
    QFile hf(srcDir + QStringLiteral("/ui/MainWindow.h"));
    QFile cf(srcDir + QStringLiteral("/ui/MainWindow.cpp"));
    QStringList cached, notRetranslated;
    if (hf.open(QIODevice::ReadOnly) && cf.open(QIODevice::ReadOnly)) {
        const QString header = QString::fromUtf8(hf.readAll());
        const QString impl = QString::fromUtf8(cf.readAll());
        QRegularExpression re(QStringLiteral("QPointer<\\s*\\w+\\s*>\\s+(\\w+)\\s*;"));
        auto it = re.globalMatch(header);
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            cached << name;
            if (!impl.contains(name + QStringLiteral("->retranslate()")))
                notRetranslated << name;
        }
    } else {
        fprintf(gLog, "[WARN] cannot read MainWindow.h/.cpp under %s\n",
                srcDir.toUtf8().constData());
    }
    for (const QString& n : cached)
        fprintf(gLog, "[CACHED] %s\n", n.toUtf8().constData());
    fprintf(gLog, "[INFO] cached widgets: %d, missing a retranslate(): %d\n",
            cached.size(), notRetranslated.size());
    CHECK(!cached.isEmpty(), "main window holds cached widgets (this audit is not vacuous)");
    for (const QString& n : notRetranslated)
        fprintf(gLog, "       >> %s is cached but never retranslated\n", n.toUtf8().constData());
    CHECK(notRetranslated.isEmpty(), "every cached widget is retranslated on a language change");

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
