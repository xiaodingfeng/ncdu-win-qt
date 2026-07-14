#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <vector>
#include <memory>

#include "FileNode.h"

// BreadcrumbBar - clickable breadcrumb path from root to the current node.
//
// BreadcrumbBar shows a chain of
// buttons separated by "›" glyphs; clicking a button emits navigated() with
// the corresponding FileNode so the host can navigate the tree.
class BreadcrumbBar : public QWidget {
    Q_OBJECT
public:
    explicit BreadcrumbBar(QWidget* parent = nullptr);

    // Set the current node and rebuild the breadcrumb chain. Passing a null
    // shared_ptr clears the bar.
    void setNode(std::shared_ptr<FileNode> node);

signals:
    // Emitted when the user clicks a breadcrumb button.
    void navigated(std::shared_ptr<FileNode> node);

private:
    QHBoxLayout* m_layout;
    std::shared_ptr<FileNode> m_current;
    std::vector<QPushButton*> m_buttons;

    void rebuild();
};
