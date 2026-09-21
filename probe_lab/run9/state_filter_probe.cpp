// state_filter_probe (run9): the state filter — the funnel in the header and
// the panel it opens.
//
// Both parts are pure UI, and both have a failure mode that looks like nothing
// at all from the outside: a funnel whose target overlaps the sort area turns
// every filter click into a re-sort, and a panel that closes on every tick makes
// "filter by several states" impossible without saying so. Neither shows up in a
// screenshot, which is why they are clicked here instead of being looked at.
//
// The panel is a Qt::Popup, so it is built and shown without a window to click
// in: grabbing it needs a real widget, not a real user.
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QHeaderView>
#include <QLayout>
#include <QMap>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>

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

// A press/release pair through the viewport, which is where a header really
// receives its mouse input (QAbstractItemView forwards it from there).
static void clickAt(QHeaderView* header, const QPoint& pos)
{
    QWidget* vp = header->viewport();
    const QPointF local(pos);
    const QPointF global(vp->mapToGlobal(pos));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &press);
    QApplication::sendEvent(vp, &release);
}

// The checkbox carrying a state's name, found by its text rather than by order:
// the order is the panel's to choose, and a test that hard-codes it would fail
// for the wrong reason the day a state is added.
static QCheckBox* boxFor(QWidget& popup, const QString& token)
{
    for (QCheckBox* box : popup.findChildren<QCheckBox*>()) {
        if (box->text().contains(token))
            return box;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Logger::init();
    gLog = fopen("state_filter_probe.log", "w");

    // persist=false: probing must not leave the app in a language nobody chose.
    I18n::setLanguage(QStringLiteral("zh"), false);

    // ---------------------------------------------------------------------
    // The header: sorting stays where it was, filtering gets its own target.
    // ---------------------------------------------------------------------
    QTreeWidget tree;
    tree.setHeaderLabels({QStringLiteral("name"), QStringLiteral("path"),
                          QStringLiteral("size"), QStringLiteral("state"),
                          QStringLiteral("action")});
    auto* header = new FilterHeaderView(Qt::Horizontal, &tree);
    tree.setHeader(header);
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->resizeSection(0, 120);
    header->resizeSection(1, 160);
    header->resizeSection(2, 100);
    header->resizeSection(3, 132);
    header->resizeSection(4, 56);
    tree.resize(700, 200);
    tree.show();
    if (QLayout* lay = tree.layout())
        lay->activate();

    const int stateCol = 3;
    header->setFilterSection(stateCol);

    int filterClicks = 0;
    int sortClicks = 0;
    int lastSortColumn = -1;
    QObject::connect(header, &FilterHeaderView::filterRequested, &tree,
                     [&filterClicks](int, const QPoint&) { ++filterClicks; });
    QObject::connect(header, &QHeaderView::sectionClicked, &tree,
                     [&sortClicks, &lastSortColumn](int column) {
                         ++sortClicks;
                         lastSortColumn = column;
                     });

    const int colLeft = header->sectionViewportPosition(stateCol);
    const int colWidth = header->sectionSize(stateCol);
    const int midY = header->height() / 2;
    // The funnel sits in the last 22px of its section; 11px in from the right
    // edge is the middle of that target.
    const QPoint funnelPoint(colLeft + colWidth - 11, midY);
    const QPoint titlePoint(colLeft + colWidth / 2, midY);

    fprintf(gLog, "[INFO] state column: x=%d w=%d; funnel point %d, title point %d\n",
            colLeft, colWidth, funnelPoint.x(), titlePoint.x());

    clickAt(header, funnelPoint);
    CHECK(filterClicks == 1, "clicking the funnel asks for the filter");
    CHECK(sortClicks == 0, "and does not also re-sort the list");

    clickAt(header, titlePoint);
    CHECK(sortClicks == 1 && lastSortColumn == stateCol,
          "clicking the column's own title still sorts by it");
    CHECK(filterClicks == 1, "and does not open the filter instead");

    // A funnel in a column with nothing to filter would be a target that cannot
    // be told apart from the sort area on the very next column.
    header->setFilterSection(-1);
    clickAt(header, funnelPoint);
    CHECK(filterClicks == 1, "no funnel, no filter: the whole section sorts");

    // ---------------------------------------------------------------------
    // The panel: several states, in one visit.
    //
    // One panel per opening, the way the list opens it: it lives on the heap and
    // is never freed here, because a panel takes itself away when it is hidden
    // and this probe ends by going away. Filling a panel that is already on
    // screen is the shape that fails — it keeps the geometry it was given the
    // first time — so every opening below is a new panel.
    // ---------------------------------------------------------------------
    auto* popup = new StateFilterPopup;
    QSet<int> selection;
    QObject::connect(popup, &StateFilterPopup::filterChanged, popup,
                     [&selection](const QSet<int>& s) { selection = s; });

    QVector<StateFilterEntry> entries;
    auto entry = [](int state, const QString& stateName, const QString& count) {
        return StateFilterEntry{state,
                                I18n::tr(QStringLiteral("app_move.filter_item"),
                                         QMap<QString, QString>{{"state", stateName},
                                                                {"count", count}}),
                                1};
    };
    entries << entry(MoveSelect::kStateFailed,
                     I18n::tr(QStringLiteral("app_move.state_failed")), QStringLiteral("3"))
            << entry(MoveSelect::kStateIdle,
                     I18n::tr(QStringLiteral("app_move.state_idle")), QStringLiteral("5"));

    popup->showFor(entries, QSet<int>(), QPoint(120, 120));
    CHECK(popup->findChildren<QCheckBox*>().size() == 2,
          "the panel lists one box per state offered");
    CHECK(!popup->isHidden(), "the panel opens");

    QCheckBox* failedBox = boxFor(*popup, I18n::tr(QStringLiteral("app_move.state_failed")));
    QCheckBox* idleBox = boxFor(*popup, I18n::tr(QStringLiteral("app_move.state_idle")));
    CHECK(failedBox != nullptr && idleBox != nullptr,
          "each state is listed under its own name");
    CHECK(failedBox && failedBox->text().contains(QStringLiteral("3")),
          "the panel says how many folders are in each state");
    CHECK(failedBox && !failedBox->isChecked(),
          "an unfiltered panel opens with nothing ticked");

    if (failedBox)
        failedBox->setChecked(true);
    CHECK(selection == QSet<int>{MoveSelect::kStateFailed},
          "ticking a state reports exactly that state");

    // The point of the panel: the second tick does not need a second visit.
    if (idleBox)
        idleBox->setChecked(true);
    CHECK(selection.size() == 2 && selection.contains(MoveSelect::kStateIdle)
              && selection.contains(MoveSelect::kStateFailed),
          "a second state is added to the first, still in the same panel");
    CHECK(!popup->isHidden(), "the panel stays open while states are ticked");

    auto* host = popup->findChild<QWidget*>(QStringLiteral("stateFilterEntries"));
    CHECK(host != nullptr, "the panel keeps its entries in a list of their own");
    if (host) {
        fprintf(gLog, "[INFO] panel %dx%d, entries host %dx%d, hint %dx%d\n",
                popup->width(), popup->height(), host->width(), host->height(),
                popup->sizeHint().width(), popup->sizeHint().height());
        // A screenshot cannot tell "no states to offer" apart from "the list was
        // never given the room", and the second one is a filter that cannot be
        // set at all.
        CHECK(host->height() >= host->minimumSizeHint().height() && host->height() > 20,
              "the list has room for the states it holds");
    }

    const QString shot = QDir::currentPath() + QStringLiteral("/state_filter_probe.png");
    const QPixmap pix = popup->grab();
    CHECK(!pix.isNull() && pix.width() > 80 && pix.save(shot, "PNG"),
          "the panel renders to a PNG");
    fprintf(gLog, "[INFO] rendered -> %s (%dx%d)\n", shot.toUtf8().constData(),
            pix.width(), pix.height());

    // The next opening is a panel of its own, and it has to show the filter that
    // is in force — otherwise looking at the filter would be what clears it.
    auto* reopened = new StateFilterPopup;
    reopened->showFor(entries, selection, QPoint(120, 120));
    QCheckBox* reopenedFailed =
        boxFor(*reopened, I18n::tr(QStringLiteral("app_move.state_failed")));
    CHECK(reopenedFailed != nullptr && reopenedFailed->isChecked(),
          "a panel opened again shows the filter that is in force");
    CHECK(reopened->findChildren<QCheckBox*>().size() == 2,
          "and lists every entry it was given, once");
    auto* reopenedHost = reopened->findChild<QWidget*>(QStringLiteral("stateFilterEntries"));
    if (reopenedHost) {
        fprintf(gLog, "[INFO] reopened panel %dx%d, entries host %dx%d\n",
                reopened->width(), reopened->height(), reopenedHost->width(),
                reopenedHost->height());
        CHECK(reopenedHost->height() >= reopenedHost->minimumSizeHint().height()
                  && reopenedHost->height() > 20,
              "with room to draw them");
    }

    auto* allBtn = popup->findChild<QPushButton*>(QStringLiteral("ghost"));
    CHECK(allBtn != nullptr, "the panel offers a way back to the whole list");
    if (allBtn) {
        CHECK(allBtn->text() == I18n::tr(QStringLiteral("app_move.filter_all")),
              "and says so in the current language");
        allBtn->click();
        CHECK(selection.isEmpty(), "which clears the filter");
    }

    // English, because the panel is wording: a key rendered as a key would look
    // exactly like a translated one to every check above. The entries are built
    // again the way the list builds them, in the language now in force.
    I18n::setLanguage(QStringLiteral("en"), false);
    auto* enPopup = new StateFilterPopup;
    QVector<StateFilterEntry> enEntries;
    enEntries << entry(MoveSelect::kStateFailed,
                       I18n::tr(QStringLiteral("app_move.state_failed")), QStringLiteral("3"));
    enPopup->showFor(enEntries, QSet<int>(), QPoint(120, 120));
    const int enBoxes = enPopup->findChildren<QCheckBox*>().size();
    QCheckBox* enBox = enPopup->findChildren<QCheckBox*>().value(0);
    const QString enLabel = enBox ? enBox->text() : QString();
    fprintf(gLog, "[INFO] en entry: %s (%d box(es))\n", enLabel.toUtf8().constData(), enBoxes);
    CHECK(enBoxes == 1, "a new panel lists the entries it was given");
    CHECK(enLabel == I18n::tr(QStringLiteral("app_move.filter_item"),
                              QMap<QString, QString>{{"state", I18n::tr(QStringLiteral("app_move.state_failed"))},
                                                     {"count", QStringLiteral("3")}}),
          "in the language in force, count included");
    CHECK(enLabel.startsWith(QStringLiteral("Incomplete")),
          "which in English is English");
    CHECK(!enLabel.contains(QStringLiteral("app_move.")),
          "no entry is a raw key name");
    auto* enAll = enPopup->findChild<QPushButton*>(QStringLiteral("ghost"));
    CHECK(enAll && enAll->text() == I18n::trIn(QStringLiteral("en"),
                                               QStringLiteral("app_move.filter_all")),
          "the panel is English after a language switch");
    const QString enShot = QDir::currentPath() + QStringLiteral("/state_filter_probe_en.png");
    CHECK(enPopup->grab().save(enShot, "PNG"), "the English panel renders to a PNG");
    I18n::setLanguage(QStringLiteral("zh"), false);   // leave the state as found

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
