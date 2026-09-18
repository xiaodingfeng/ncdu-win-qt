// ui_render_probe (run6): renders the "software data folders" tab and the
// relocation dialog headlessly and saves them as PNGs, so the amber warning
// block (icon + wording) and the detected folder rows can be checked by eye
// instead of trusted. The panel is rendered once per language, because the
// wording is language-specific and the English one is easy to get wrong.
//
// Nothing is shown: showEvent() would start a real scan (and the journal repair
// pass) against the live profile. Layout is activated by hand and the widgets
// are rendered with grab(), which needs no window.
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QTreeWidget>
#include <cstdio>

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
    fprintf(gLog, "[INFO] process exit is not the verdict for the PNG checks: %d failed\n", gFail);

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
