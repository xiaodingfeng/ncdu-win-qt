#include "LockerDialog.h"

#include <QBrush>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "I18n.h"
#include "Style.h"

namespace LockerDialog {

namespace {

// One row per process: what the user knows it as, the number that identifies it
// exactly, and what is going to happen to it. The full path goes in the row's
// tooltip rather than in a column, because the paths are long, similar-looking
// and only needed when two entries share a name.
void fillList(QTreeWidget* tree, const QVector<WinApi::LockingProcess>& procs)
{
    for (const WinApi::LockingProcess& p : procs) {
        auto* item = new QTreeWidgetItem(tree);
        item->setText(0, p.name);
        item->setText(1, QString::number(p.pid));
        item->setText(2, p.safeToClose
                            ? I18n::tr(QStringLiteral("proc_close.can_close"))
                            : I18n::tr(p.blockKey));
        // The colour is the whole message: green means "we will handle it",
        // muted means "this one is yours".
        item->setForeground(2, QBrush(QColor(QString::fromLatin1(
            p.safeToClose ? C::ACCENT() : C::TEXT_MUTED()))));
        if (!p.exePath.isEmpty()) {
            item->setToolTip(0, p.exePath);
            item->setToolTip(1, p.exePath);
            item->setToolTip(2, p.exePath);
        }
    }
}

} // namespace

int closableCount(const QVector<WinApi::LockingProcess>& procs)
{
    int n = 0;
    for (const WinApi::LockingProcess& p : procs)
        if (p.safeToClose)
            ++n;
    return n;
}

void populate(QDialog& dlg, const QVector<WinApi::LockingProcess>& procs,
              const QString& intro, const QString& closeText,
              const QString& skipText, bool allowSkip)
{
    const int closable = closableCount(procs);

    dlg.setModal(true);
    dlg.setMinimumWidth(520);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(24, 20, 24, 16);
    lay->setSpacing(10);

    auto* head = new QLabel(intro, &dlg);
    head->setWordWrap(true);
    head->setStyleSheet(QStringLiteral("font-size: 13px; color: %1;")
                            .arg(QString::fromLatin1(C::FG())));
    lay->addWidget(head);

    auto* tree = new QTreeWidget(&dlg);
    tree->setObjectName(QString::fromLatin1(kListName));
    tree->setColumnCount(3);
    tree->setHeaderLabels({I18n::tr(QStringLiteral("proc_close.col_proc")),
                           I18n::tr(QStringLiteral("proc_close.col_pid")),
                           I18n::tr(QStringLiteral("proc_close.col_action"))});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->setStyleSheet(QStringLiteral("font-size: 12px;"));
    fillList(tree, procs);
    // Tall enough to show a typical handful outright, capped so a runaway list
    // cannot push the buttons off the screen (the list scrolls instead). The
    // measurements come from the widget itself rather than from a guessed row
    // height: both the header and the rows grow with the font and the scaling.
    const int rows = qBound(2, static_cast<int>(procs.size()), 7);
    const int headerH = tree->header()->sizeHint().height();
    const int rowH = procs.isEmpty() ? 26 : qMax(20, tree->sizeHintForRow(0));
    tree->setFixedHeight(headerH + rows * rowH + 4);
    lay->addWidget(tree);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    tree->header()->resizeSection(1, 72);
    tree->header()->resizeSection(
        2, QFontMetrics(tree->font()).horizontalAdvance(
               I18n::tr(QStringLiteral("proc_close.block_unknown"))) + 28);

    auto* warn = new QLabel(closable > 0
                                ? I18n::tr(QStringLiteral("proc_close.warn_lost"))
                                : I18n::tr(QStringLiteral("proc_close.warn_none")),
                            &dlg);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
                            .arg(QString::fromLatin1(
                                closable > 0 ? C::WARNING() : C::DANGER())));
    lay->addWidget(warn);

    lay->addSpacing(4);
    auto* btns = new QHBoxLayout;
    btns->setSpacing(8);
    btns->addStretch(1);

    auto* cancel = new QPushButton(I18n::tr(QStringLiteral("button.cancel")), &dlg);
    cancel->setObjectName(QString::fromLatin1(kCancelName));
    cancel->setCursor(Qt::PointingHandCursor);
    QObject::connect(cancel, &QPushButton::clicked, &dlg,
                     [&dlg]() { dlg.done(Cancel); });
    btns->addWidget(cancel);

    if (allowSkip) {
        auto* skip = new QPushButton(skipText, &dlg);
        skip->setObjectName(QString::fromLatin1(kSkipName));
        skip->setCursor(Qt::PointingHandCursor);
        QObject::connect(skip, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(Skip); });
        btns->addWidget(skip);
        if (closable == 0)
            skip->setDefault(true);
    }
    if (closable > 0) {
        auto* close = new QPushButton(closeText, &dlg);
        // "primary" is the app-wide name the stylesheet gives its accent look to.
        close->setObjectName(QString::fromLatin1(kCloseName));
        close->setCursor(Qt::PointingHandCursor);
        close->setDefault(true);
        QObject::connect(close, &QPushButton::clicked, &dlg,
                         [&dlg]() { dlg.done(CloseAndContinue); });
        btns->addWidget(close);
    }
    lay->addLayout(btns);

    // The height comes from the layout; the width is only widened past the
    // minimum, so a long program path in a tooltip still gets a comfortable row.
    dlg.adjustSize();
    dlg.resize(qMax(600, dlg.width()), dlg.height());
}

Answer ask(QWidget* parent, const QString& title, const QString& intro,
           const QVector<WinApi::LockingProcess>& procs,
           const QString& closeText, const QString& skipText, bool allowSkip)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    populate(dlg, procs, intro, closeText, skipText, allowSkip);

    // Escape and the title bar's close button mean Cancel, which is what done()
    // reports for anything it was not told about — so a dismissed dialog never
    // acts on a process.
    return static_cast<Answer>(dlg.exec());
}

QString outcomeText(const QVector<WinApi::CloseOutcome>& outcomes)
{
    QStringList exited, killed, survived, refused;
    for (const WinApi::CloseOutcome& o : outcomes) {
        switch (o.result) {
        case WinApi::CloseOutcome::Exited:  exited << o.name; break;
        case WinApi::CloseOutcome::Killed:  killed << o.name; break;
        case WinApi::CloseOutcome::Survived: survived << o.name; break;
        case WinApi::CloseOutcome::Refused: refused << o.name; break;
        }
    }
    QStringList lines;
    if (!exited.isEmpty())
        lines << I18n::tr(QStringLiteral("proc_close.done_closed"),
                          QMap<QString, QString>{{"procs", exited.join(QStringLiteral("、"))}});
    if (!killed.isEmpty())
        lines << I18n::tr(QStringLiteral("proc_close.done_killed"),
                          QMap<QString, QString>{{"procs", killed.join(QStringLiteral("、"))}});
    if (!survived.isEmpty())
        lines << I18n::tr(QStringLiteral("proc_close.done_survived"),
                          QMap<QString, QString>{{"procs", survived.join(QStringLiteral("、"))}});
    if (!refused.isEmpty())
        lines << I18n::tr(QStringLiteral("proc_close.done_refused"),
                          QMap<QString, QString>{{"procs", refused.join(QStringLiteral("、"))}});
    return lines.join(QLatin1Char('\n'));
}

bool anyClosed(const QVector<WinApi::CloseOutcome>& outcomes)
{
    for (const WinApi::CloseOutcome& o : outcomes)
        if (o.result == WinApi::CloseOutcome::Exited || o.result == WinApi::CloseOutcome::Killed)
            return true;
    return false;
}

} // namespace LockerDialog
