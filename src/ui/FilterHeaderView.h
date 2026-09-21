#pragma once

#include <QHeaderView>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

// One line of the state filter: a state value, the name it shows, and how many
// folders in the list are currently in it.
struct StateFilterEntry {
    int state = 0;
    QString label;
    int count = 0;
};

// The panel the funnel opens. It stays open while the user ticks states — a
// QMenu closes on every tick, so choosing three states would mean opening it
// three times, which is the one thing "filter by several states" must not feel
// like. Ticking is immediate: the list behind the panel updates as it is
// ticked, and there is no "apply" to forget.
//
// One panel per opening, filled once: the counts and the wording both depend on
// what the list holds at that moment, and a panel that has already been on
// screen brings its own geometry with it when it is filled a second time.
class StateFilterPopup : public QWidget {
    Q_OBJECT
public:
    explicit StateFilterPopup(QWidget* parent = nullptr);

    void showFor(const QVector<StateFilterEntry>& entries,
                 const QSet<int>& selected,
                 const QPoint& globalPos);

signals:
    void filterChanged(const QSet<int>& selected);

protected:
    // Closes on a click outside or on Esc — and takes itself with it. The panel
    // is created for one look at the filter, not kept around.
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void clearEntries();
    void emitFromBoxes();

    QLabel* m_title = nullptr;
    QWidget* m_listHost = nullptr;
    QVBoxLayout* m_listLay = nullptr;
    QPushButton* m_allBtn = nullptr;
    QVector<QCheckBox*> m_boxes;
    QVector<int> m_states;
};

// A header that can filter as well as sort. Sorting stays exactly where it was
// — a click anywhere on the section orders by it — and filtering gets its own
// small target at the right edge of one section, so the two can never fight
// over the same click. That is the whole reason for this subclass: a funnel
// that opened the filter from the middle of the section would make the state
// column unsortable, and "sort by state" is worth keeping.
class FilterHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit FilterHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr);

    // The strip the funnel is drawn in, at the right edge of its section. Public
    // because the section has to be wide enough for its own name AND this strip,
    // and whoever sets that width has to be able to ask.
    static constexpr int kFilterSlot = 22;

    // Which section carries the funnel (-1 for none).
    void setFilterSection(int section);
    // Highlighted while rows are being hidden, so a list that is not showing
    // everything says so without the panel having to be open.
    void setFilterActive(bool active);
    bool isFilterActive() const { return m_filterActive; }

signals:
    // The user hit the funnel on *section*. What that opens is the panel's
    // business: this widget knows about clicks, not about folder states.
    void filterRequested(int section, const QPoint& globalPos);

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // The funnel's own strip, in header coordinates. Used both to draw it and
    // to decide whether a press meant it.
    QRect funnelRect(int section) const;

    int m_filterSection = -1;
    bool m_filterActive = false;
    bool m_funnelPressed = false;
};
