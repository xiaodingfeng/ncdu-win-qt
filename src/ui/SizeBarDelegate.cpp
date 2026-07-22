#include "SizeBarDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QWidget>
#include <QRectF>
#include <QColor>
#include <QVariant>

#include "Style.h"

void SizeBarDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const
{
    // Check if this cell has bar data at all.
    QVariant barData = index.data(BAR_ROLE);
    if (!barData.isValid()) {
        // No bar data — draw normally with text (used in search mode for path column).
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
    opt.text.clear();  // let the style draw background/selection without text
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    // Fraction (0..1) and fill color from custom roles.
    double fraction = barData.toDouble();
    QString colorHex = index.data(COLOR_ROLE).toString();
    if (colorHex.isEmpty())
        colorHex = QString::fromLatin1(C::PRIMARY());
    if (fraction <= 0.0)
        return;

    QRectF r = QRectF(option.rect).adjusted(4, 0, -4, 0);
    const int barH = 8;
    qreal barY = r.y() + (r.height() - barH) / 2.0;

    painter->save();
    painter->setPen(Qt::NoPen);

    // Track / background of the bar.
    painter->setBrush(QColor(QString::fromLatin1(C::SURFACE_ALT())));
    painter->drawRoundedRect(QRectF(r.x(), barY, r.width(), barH), 4, 4);

    // Filled portion (at least 3px so tiny slices are still visible).
    int fillW = std::max(3, static_cast<int>(r.width() * fraction));
    painter->setBrush(QColor(colorHex));
    painter->drawRoundedRect(QRectF(r.x(), barY, fillW, barH), 4, 4);

    painter->restore();
}
