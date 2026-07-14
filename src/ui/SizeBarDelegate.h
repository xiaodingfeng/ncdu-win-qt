#pragma once

#include <QStyledItemDelegate>

// SizeBarDelegate - paints a proportion bar in the "distribution" column of
// the file list.
//
// SizeBarDelegate paints a bar in the distribution column. Two custom roles carry
// the bar's fraction (0..1) and fill color. The delegate draws the standard
// item background first (so selection/hover states are preserved), then
// overlays an 8px-tall rounded bar centered in the cell.
class SizeBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // Fraction of the bar to fill (0.0 .. 1.0), stored as a double via QVariant.
    static constexpr int BAR_ROLE = Qt::UserRole + 1;
    // Fill color for the bar (hex string "#RRGGBB"), stored as a QString via QVariant.
    static constexpr int COLOR_ROLE = Qt::UserRole + 2;

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};
