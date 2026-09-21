#include "FilterHeaderView.h"

#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include "I18n.h"
#include "Style.h"

namespace {

// The mark itself, drawn around the centre of the strip the header reserves for
// it (FilterHeaderView::kFilterSlot). A funnel rather than the
// plain triangle a sort indicator uses: on a header that already carries one of
// those, a second triangle reads as a second sort, not as a filter.
constexpr qreal kFunnelMouth = 10.0;   // how wide it opens
constexpr qreal kFunnelNeck = 2.4;     // ...and the stem it narrows into
constexpr qreal kFunnelTop = -4.0;
constexpr qreal kFunnelShoulder = 0.5;
constexpr qreal kFunnelBottom = 4.5;

}  // namespace

// --------------------------------------------------------------------------- //
// The filter panel
// --------------------------------------------------------------------------- //
StateFilterPopup::StateFilterPopup(QWidget* parent)
    : QWidget(parent)
{
    // A popup, so a click anywhere else closes it without a modal grip on the
    // rest of the window — the list it filters stays usable while it is open.
    setWindowFlags(Qt::Popup);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName(QStringLiteral("stateFilterPopup"));
    setStyleSheet(QStringLiteral(
        "#stateFilterPopup { background: %1; border: 1px solid %2; border-radius: 8px; }")
        .arg(QString::fromLatin1(C::SURFACE()), QString::fromLatin1(C::BORDER())));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 8);
    outer->setSpacing(4);

    m_title = new QLabel(this);
    m_title->setText(I18n::tr(QStringLiteral("app_move.filter_title")));
    m_title->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 600; color: %1;")
                               .arg(QString::fromLatin1(C::TEXT_MUTED())));
    outer->addWidget(m_title);

    m_listHost = new QWidget(this);
    // Named so the probe lab can measure the list instead of guessing from a
    // screenshot: a panel that renders without its entries looks exactly like a
    // panel with nothing to offer.
    m_listHost->setObjectName(QStringLiteral("stateFilterEntries"));
    m_listLay = new QVBoxLayout(m_listHost);
    m_listLay->setContentsMargins(0, 0, 0, 0);
    m_listLay->setSpacing(2);
    outer->addWidget(m_listHost);

    m_allBtn = new QPushButton(I18n::tr(QStringLiteral("app_move.filter_all")), this);
    m_allBtn->setObjectName(QStringLiteral("ghost"));
    m_allBtn->setCursor(Qt::PointingHandCursor);
    m_allBtn->setFixedHeight(22);
    m_allBtn->setStyleSheet(QStringLiteral("font-size: 11px; padding: 1px 8px;"));
    // "All" is the way back out of a filter, and the only one: unticking every
    // box does the same thing, but a user who filtered the list down to one
    // state should not have to work out that unticking it is how to see the
    // rest again.
    connect(m_allBtn, &QPushButton::clicked, this, [this]() {
        for (QCheckBox* box : m_boxes) {
            const QSignalBlocker blocker(box);
            box->setChecked(false);
        }
        emitFromBoxes();
    });
    outer->addWidget(m_allBtn);
}

void StateFilterPopup::clearEntries()
{
    // Deleted outright rather than handed to the event loop: an orphaned box is
    // a visible widget with no parent, so it would stay on screen as a small
    // floating window until the loop got round to it — and this runs while the
    // panel is being reopened, with the user looking straight at it.
    for (QCheckBox* box : m_boxes)
        delete box;
    m_boxes.clear();
    m_states.clear();
}

void StateFilterPopup::emitFromBoxes()
{
    QSet<int> out;
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i]->isChecked())
            out.insert(m_states[i]);
    }
    emit filterChanged(out);
}

void StateFilterPopup::showFor(const QVector<StateFilterEntry>& entries,
                               const QSet<int>& selected,
                               const QPoint& globalPos)
{
    clearEntries();
    m_title->setText(I18n::tr(QStringLiteral("app_move.filter_title")));
    m_allBtn->setText(I18n::tr(QStringLiteral("app_move.filter_all")));

    for (const StateFilterEntry& e : entries) {
        auto* box = new QCheckBox(e.label, m_listHost);
        // Set before the connection, so filling the panel in cannot look like
        // the user ticking boxes.
        box->setChecked(selected.contains(e.state));
        box->setCursor(Qt::PointingHandCursor);
        connect(box, &QCheckBox::toggled, this, [this](bool) { emitFromBoxes(); });
        m_listLay->addWidget(box);
        m_boxes.push_back(box);
        m_states.push_back(e.state);
    }

    m_listLay->activate();
    m_listHost->adjustSize();
    adjustSize();
    QPoint pos = globalPos;
    // Qt::Popup closes itself on any click outside, so the only thing to get
    // right here is staying on screen: opened near the right or bottom edge a
    // panel would otherwise open off it.
    if (QScreen* scr = screen()) {
        const QRect avail = scr->availableGeometry();
        pos.setX(qBound(avail.left(), pos.x(), qMax(avail.left(), avail.right() - width())));
        pos.setY(qBound(avail.top(), pos.y(), qMax(avail.top(), avail.bottom() - height())));
    }
    move(pos);
    show();
    raise();
}

void StateFilterPopup::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    deleteLater();
}

void StateFilterPopup::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyPressEvent(event);
}

// --------------------------------------------------------------------------- //
// The header
// --------------------------------------------------------------------------- //
FilterHeaderView::FilterHeaderView(Qt::Orientation orientation, QWidget* parent)
    : QHeaderView(orientation, parent)
{
}

void FilterHeaderView::setFilterSection(int section)
{
    if (m_filterSection == section)
        return;
    const int previous = m_filterSection;
    m_filterSection = section;
    if (previous >= 0)
        updateSection(previous);
    if (section >= 0)
        updateSection(section);
    viewport()->update();
}

void FilterHeaderView::setFilterActive(bool active)
{
    if (m_filterActive == active)
        return;
    m_filterActive = active;
    if (m_filterSection >= 0)
        updateSection(m_filterSection);
}

QRect FilterHeaderView::funnelRect(int section) const
{
    return QRect(sectionViewportPosition(section) + sectionSize(section) - kFilterSlot,
                 viewport()->rect().top(), kFilterSlot, viewport()->height());
}

void FilterHeaderView::paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const
{
    QHeaderView::paintSection(painter, rect, logicalIndex);
    if (logicalIndex != m_filterSection)
        return;

    // Drawn rather than lettered: a funnel stays legible at header size in every
    // language, and a word here would push the column's own name out of it.
    const QRect slot = funnelRect(logicalIndex);
    const QPointF c(slot.center().x() + 0.5, slot.center().y() + 0.5);
    QPainterPath path;
    path.moveTo(c.x() - kFunnelMouth / 2.0, c.y() + kFunnelTop);
    path.lineTo(c.x() + kFunnelMouth / 2.0, c.y() + kFunnelTop);
    path.lineTo(c.x() + kFunnelNeck / 2.0, c.y() + kFunnelShoulder);
    path.lineTo(c.x() + kFunnelNeck / 2.0, c.y() + kFunnelBottom);
    path.lineTo(c.x() - kFunnelNeck / 2.0, c.y() + kFunnelBottom);
    path.lineTo(c.x() - kFunnelNeck / 2.0, c.y() + kFunnelShoulder);
    path.closeSubpath();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QString::fromLatin1(m_filterActive ? C::PRIMARY() : C::TEXT_MUTED())));
    painter->drawPath(path);
    painter->restore();
}

void FilterHeaderView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_filterSection >= 0) {
        const int index = logicalIndexAt(event->pos());
        if (index == m_filterSection && funnelRect(m_filterSection).contains(event->pos())) {
            // Swallowed on purpose: the press never reaches the header's own
            // sort handling, so one click cannot both open the filter and
            // reorder the list.
            m_funnelPressed = true;
            emit filterRequested(index, viewport()->mapToGlobal(event->pos()));
            return;
        }
    }
    QHeaderView::mousePressEvent(event);
}

void FilterHeaderView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_funnelPressed) {
        m_funnelPressed = false;
        return;
    }
    QHeaderView::mouseReleaseEvent(event);
}
