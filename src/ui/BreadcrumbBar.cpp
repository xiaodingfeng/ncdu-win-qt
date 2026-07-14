#include "BreadcrumbBar.h"

#include <QLabel>
#include <QLayoutItem>

#include <algorithm>

BreadcrumbBar::BreadcrumbBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("breadcrumb");
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(2);
}

void BreadcrumbBar::setNode(std::shared_ptr<FileNode> node)
{
    m_current = std::move(node);
    rebuild();
}

void BreadcrumbBar::rebuild()
{
    // Remove and delete every widget currently in the layout.
    while (m_layout->count() > 0) {
        QLayoutItem* item = m_layout->takeAt(0);
        if (item == nullptr)
            break;
        QWidget* w = item->widget();
        if (w)
            w->deleteLater();
        delete item;
    }
    m_buttons.clear();

    if (!m_current)
        return;

    // Build the chain from root to the current node.
    std::vector<std::shared_ptr<FileNode>> chain;
    {
        std::shared_ptr<FileNode> cur = m_current;
        while (cur) {
            chain.push_back(cur);
            cur = cur->parent.lock();
        }
    }
    std::reverse(chain.begin(), chain.end());

    for (size_t i = 0; i < chain.size(); ++i) {
        const std::shared_ptr<FileNode>& n = chain[i];
        if (i > 0) {
            auto* sep = new QLabel(QStringLiteral("\u203A"));  // ›
            sep->setObjectName("crumb-sep");
            m_layout->addWidget(sep);
        }
        auto* btn = new QPushButton(n->name.isEmpty() ? n->path : n->name);
        btn->setObjectName("crumb");
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(n->path);
        // Capture the shared_ptr by value so the node stays alive for the
        // lifetime of the lambda.
        std::shared_ptr<FileNode> nn = n;
        connect(btn, &QPushButton::clicked, this, [this, nn]() {
            emit navigated(nn);
        });
        m_layout->addWidget(btn);
        m_buttons.push_back(btn);
    }
    m_layout->addStretch(1);
}
