#include "TreemapWidget.h"

#include <QPainter>
#include <QBrush>
#include <QPen>
#include <QFont>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QEvent>
#include <QSizePolicy>
#include <QRectF>
#include <QColor>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

#include "Style.h"
#include "FormatHelpers.h"
#include "I18n.h"

// --------------------------------------------------------------------------- //
// Squarified treemap layout (file-local helpers)
// --------------------------------------------------------------------------- //
namespace {

using Item = std::tuple<double, std::shared_ptr<FileNode>, int>;

// Minimum cell size to draw a label.
constexpr int MIN_LABEL_W = 40;
constexpr int MIN_LABEL_H = 18;

// Worst aspect ratio of a row of areas laid along `side`.
// Calculate worst aspect ratio for a row.
double worstAspect(const std::vector<double>& row, double side)
{
    if (row.empty() || side <= 0.0)
        return std::numeric_limits<double>::infinity();
    double s = 0.0;
    for (double a : row)
        s += a;
    if (s <= 0.0)
        return std::numeric_limits<double>::infinity();
    double longSide = s / side;
    if (longSide <= 0.0)
        return std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (double a : row) {
        if (a <= 0.0)
            return std::numeric_limits<double>::infinity();
        double itemSide = a / longSide;
        double r = std::max(itemSide / longSide, longSide / itemSide);
        if (r > worst)
            worst = r;
    }
    return worst;
}

// Remaining rectangle after laying out a row.
struct RemRect { double x, y, w, h; };

// Place a row in the squarified treemap layout.
// Returns the remaining (x, y, w, h). `horizontal` means w >= h: the row spans
// the full width and items are arranged left-to-right.
RemRect layoutRow(const std::vector<Item>& row,
                  double x, double y, double w, double h,
                  bool horizontal, std::vector<TreemapCell>& out)
{
    double rowArea = 0.0;
    for (const auto& t : row)
        rowArea += std::get<0>(t);
    if (rowArea <= 0.0)
        return {x, y, w, h};

    if (horizontal) {
        double rowH = (w > 0.0) ? rowArea / w : 0.0;
        if (rowH <= 0.0)
            return {x, y, w, h};
        double cx = x;
        for (const auto& t : row) {
            double v = std::get<0>(t);
            const auto& node = std::get<1>(t);
            int depth = std::get<2>(t);
            double cellW = (rowH > 0.0) ? v / rowH : 0.0;
            QRectF rect(cx, y, cellW, rowH);
            out.push_back(TreemapCell{
                node, rect,
                QColor(typeColor(node->name, node->isDir(), node->isHidden)),
                depth});
            cx += cellW;
        }
        return {x, y + rowH, w, h - rowH};
    } else {
        double rowW = (h > 0.0) ? rowArea / h : 0.0;
        if (rowW <= 0.0)
            return {x, y, w, h};
        double cy = y;
        for (const auto& t : row) {
            double v = std::get<0>(t);
            const auto& node = std::get<1>(t);
            int depth = std::get<2>(t);
            double cellH = (rowW > 0.0) ? v / rowW : 0.0;
            QRectF rect(x, cy, rowW, cellH);
            out.push_back(TreemapCell{
                node, rect,
                QColor(typeColor(node->name, node->isDir(), node->isHidden)),
                depth});
            cy += cellH;
        }
        return {x + rowW, y, w - rowW, h};
    }
}

// Recursive squarify. `items` is the full area list; `start` is the offset into
// it for the current recursion.
void squarifyRecurse(const std::vector<Item>& items, size_t start,
                     double x, double y, double w, double h,
                     std::vector<TreemapCell>& out)
{
    if (start >= items.size() || w <= 0.0 || h <= 0.0)
        return;

    // Single item fills the remaining rectangle.
    if (items.size() - start == 1) {
        const auto& t = items[start];
        double a = std::get<0>(t);
        if (a > 0.0) {
            const auto& node = std::get<1>(t);
            int depth = std::get<2>(t);
            QRectF rect(x, y, w, h);
            out.push_back(TreemapCell{
                node, rect,
                QColor(typeColor(node->name, node->isDir(), node->isHidden)),
                depth});
        }
        return;
    }

    double side = std::min(w, h);
    bool horizontal = w >= h;

    std::vector<Item> row;
    std::vector<double> rowAreas;
    size_t i = start;
    while (i < items.size()) {
        double a = std::get<0>(items[i]);
        if (a <= 0.0) {
            ++i;
            continue;
        }
        if (row.empty()) {
            row.push_back(items[i]);
            rowAreas.push_back(a);
            ++i;
            continue;
        }
        double curWorst = worstAspect(rowAreas, side);
        std::vector<double> withNew = rowAreas;
        withNew.push_back(a);
        double newWorst = worstAspect(withNew, side);
        if (newWorst <= curWorst) {
            row.push_back(items[i]);
            rowAreas.push_back(a);
            ++i;
        } else {
            break;
        }
    }

    if (row.empty()) {
        // First item had non-positive area; skip it and recurse on the rest.
        squarifyRecurse(items, start + 1, x, y, w, h, out);
        return;
    }

    RemRect rem = layoutRow(row, x, y, w, h, horizontal, out);
    squarifyRecurse(items, i, rem.x, rem.y, rem.w, rem.h, out);
}

}  // namespace

// --------------------------------------------------------------------------- //
// TreemapWidget
// --------------------------------------------------------------------------- //
TreemapWidget::TreemapWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_Hover, true);
}

void TreemapWidget::setNode(std::shared_ptr<FileNode> node, bool showFiles, int maxDepth)
{
    m_node = node;
    m_showFiles = showFiles;
    m_maxDepth = std::max(1, maxDepth);
    m_hoveredIndex = -1;
    rebuild();
    update();
}

void TreemapWidget::clear()
{
    m_node.reset();
    m_cells.clear();
    m_hoveredIndex = -1;
    update();
}

// ---- layout ---------------------------------------------------------------
void TreemapWidget::rebuild()
{
    m_cells.clear();
    if (!m_node)
        return;
    int w = std::max(1, width() - 8);
    int h = std::max(1, height() - 8);
    auto items = collect(m_node, 0);
    if (items.empty())
        return;
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) {
                  return std::get<0>(a) > std::get<0>(b);
              });
    squarify(items, 4.0, 4.0, static_cast<double>(w), static_cast<double>(h), m_cells);
}

std::vector<std::tuple<double, std::shared_ptr<FileNode>, int>>
TreemapWidget::collect(std::shared_ptr<FileNode> node, int depth) const
{
    std::vector<Item> out;
    if (!node)
        return out;
    if (depth >= m_maxDepth)
        return out;
    for (const auto& c : node->children) {
        if (!c)
            continue;
        if (c->size <= 0)
            continue;
        if (!m_showFiles && c->nodeType == NodeType::File)
            continue;
        out.emplace_back(static_cast<double>(c->size), c, depth);
    }
    return out;
}

void TreemapWidget::squarify(std::vector<Item>& items,
                             double x, double y, double w, double h,
                             std::vector<TreemapCell>& out)
{
    if (items.empty() || w <= 0.0 || h <= 0.0)
        return;
    double total = 0.0;
    for (const auto& t : items)
        total += std::get<0>(t);
    if (total <= 0.0)
        return;
    double scale = (w * h) / total;
    std::vector<Item> areas;
    areas.reserve(items.size());
    for (const auto& t : items) {
        double v = std::max(0.0, std::get<0>(t)) * scale;
        areas.emplace_back(v, std::get<1>(t), std::get<2>(t));
    }
    squarifyRecurse(areas, 0, x, y, w, h, out);
}

// ---- painting -------------------------------------------------------------
void TreemapWidget::paintEvent(QPaintEvent* /*e*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Background card.
    p.setBrush(QBrush(QColor(QString::fromLatin1(C::SURFACE))));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 10, 10);

    if (m_cells.empty()) {
        p.setPen(QColor(QString::fromLatin1(C::TEXT_MUTED)));
        QFont emptyFont(QString::fromLatin1(DEFAULT_FAMILY), 11);
        p.setFont(emptyFont);
        p.drawText(rect(), Qt::AlignCenter, I18n::tr("treemap.empty"));
        return;
    }

    QFont labelFont(QString::fromLatin1(DEFAULT_FAMILY), 9);
    labelFont.setWeight(QFont::DemiBold);
    QFont smallFont(QString::fromLatin1(DEFAULT_FAMILY), 8);

    for (int idx = 0; idx < static_cast<int>(m_cells.size()); ++idx) {
        const TreemapCell& cell = m_cells[idx];
        const QRectF& r = cell.rect;
        if (r.width() < 2 || r.height() < 2)
            continue;
        QColor color = cell.color;
        if (idx == m_hoveredIndex)
            color = color.lighter(115);
        p.setBrush(QBrush(color));
        p.setPen(QPen(QColor(QString::fromLatin1(C::SURFACE)), 2));
        p.drawRoundedRect(r, 4, 4);

        // Label: only when the cell is large enough.
        if (r.width() >= MIN_LABEL_W && r.height() >= MIN_LABEL_H) {
            p.setPen(QColor(QString::fromLatin1(C::SURFACE)));
            QString name = cell.node->name;
            if (name.length() > 18)
                name = name.left(17) + QStringLiteral("\u2026");
            p.setFont(labelFont);
            p.drawText(QRectF(r.x() + 6, r.y() + 4, r.width() - 12, 16),
                       Qt::AlignLeft | Qt::AlignVCenter, name);
            if (r.height() >= 34) {
                p.setFont(smallFont);
                p.drawText(QRectF(r.x() + 6, r.y() + 20, r.width() - 12, 14),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           humanSize(cell.node->size));
            }
        }
    }

    // Hover outline (drawn last so it sits on top).
    if (m_hoveredIndex >= 0 && m_hoveredIndex < static_cast<int>(m_cells.size())) {
        QRectF r = m_cells[m_hoveredIndex].rect;
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(QString::fromLatin1(C::TEXT)), 1.5));
        p.drawRoundedRect(r, 4, 4);
    }
    p.end();
}

// ---- interaction ----------------------------------------------------------
int TreemapWidget::cellAt(const QPoint& pos) const
{
    for (int i = 0; i < static_cast<int>(m_cells.size()); ++i) {
        if (m_cells[i].rect.contains(pos.x(), pos.y()))
            return i;
    }
    return -1;
}

void TreemapWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    rebuild();
    update();
}

void TreemapWidget::mouseMoveEvent(QMouseEvent* e)
{
    int idx = cellAt(e->position().toPoint());
    if (idx != m_hoveredIndex) {
        m_hoveredIndex = idx;
        update();
        if (idx >= 0 && idx < static_cast<int>(m_cells.size()))
            emit hovered(m_cells[idx].node);
        else
            emit hovered(nullptr);
    }
}

void TreemapWidget::leaveEvent(QEvent* /*e*/)
{
    if (m_hoveredIndex != -1) {
        m_hoveredIndex = -1;
        update();
        emit hovered(nullptr);
    }
}

void TreemapWidget::mousePressEvent(QMouseEvent* e)
{
    QWidget::mousePressEvent(e);
}

void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        int idx = cellAt(e->position().toPoint());
        if (idx >= 0 && idx < static_cast<int>(m_cells.size())) {
            const auto& node = m_cells[idx].node;
            if (node && node->isDir())
                emit cellDoubleClicked(node);
        }
    }
}

QSize TreemapWidget::sizeHint() const
{
    return QSize(400, 260);
}
