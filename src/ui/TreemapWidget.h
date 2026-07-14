#pragma once

#include <QWidget>
#include <QRectF>
#include <QColor>
#include <vector>
#include <memory>
#include <tuple>

#include "FileNode.h"

// A single cell in the squarified treemap.
struct TreemapCell {
    std::shared_ptr<FileNode> node;
    QRectF rect;
    QColor color;
    int depth;
};

// Squarified treemap of the children of the current node.
//
// TreemapWidget uses the squarified
// layout (Bruls / Houtman / van Wijk 2000) for good aspect ratios. Each cell
// is colored by file type, shows a label when large enough, supports hover
// highlighting and double-click navigation.
class TreemapWidget : public QWidget {
    Q_OBJECT
public:
    explicit TreemapWidget(QWidget* parent = nullptr);

    void setNode(std::shared_ptr<FileNode> node, bool showFiles = true, int maxDepth = 1);
    void clear();

signals:
    void cellDoubleClicked(std::shared_ptr<FileNode> node);
    void hovered(std::shared_ptr<FileNode> node);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    QSize sizeHint() const override;

private:
    std::shared_ptr<FileNode> m_node;
    std::vector<TreemapCell> m_cells;
    bool m_showFiles = true;
    int m_maxDepth = 1;
    int m_hoveredIndex = -1;

    void rebuild();
    std::vector<std::tuple<double, std::shared_ptr<FileNode>, int>> collect(
        std::shared_ptr<FileNode> node, int depth) const;
    void squarify(std::vector<std::tuple<double, std::shared_ptr<FileNode>, int>>& items,
                  double x, double y, double w, double h,
                  std::vector<TreemapCell>& out);
    int cellAt(const QPoint& pos) const;
};
