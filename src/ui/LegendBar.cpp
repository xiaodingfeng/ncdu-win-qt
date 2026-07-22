#include "LegendBar.h"

#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QColor>
#include <QLayoutItem>

#include "Style.h"
#include "I18n.h"

namespace {

// Small colored square swatch for the legend.
// Legend swatch widget.
class Swatch : public QWidget {
public:
    explicit Swatch(const QString& color, QWidget* parent = nullptr)
        : QWidget(parent), m_color(color)
    {
        setFixedSize(10, 10);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(m_color));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(rect(), 3, 3);
    }

private:
    QColor m_color;
};

} // namespace

LegendBar::LegendBar(QWidget* parent)
    : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(14);

    for (const auto& [key, color] : legendItems()) {
        auto* swatch = new Swatch(color);
        m_layout->addWidget(swatch);
        auto* txt = new QLabel(I18n::tr(key));
        txt->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                               .arg(QString::fromLatin1(C::TEXT_SEC())));
        m_layout->addWidget(txt);
        m_items.push_back(txt);
    }
    m_layout->addStretch(1);
}

void LegendBar::refreshTheme()
{
    // Swatch colors and label colors both derive from the active palette, so
    // a full rebuild picks up the new theme.
    refreshLabels();
}

void LegendBar::refreshLabels()
{
    // Clear existing widgets.
    while (m_layout->count() > 0) {
        QLayoutItem* item = m_layout->takeAt(0);
        if (item == nullptr)
            break;
        QWidget* w = item->widget();
        if (w)
            w->deleteLater();
        delete item;
    }
    m_items.clear();

    for (const auto& [key, color] : legendItems()) {
        auto* swatch = new Swatch(color);
        m_layout->addWidget(swatch);
        auto* txt = new QLabel(I18n::tr(key));
        txt->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                               .arg(QString::fromLatin1(C::TEXT_SEC())));
        m_layout->addWidget(txt);
        m_items.push_back(txt);
    }
    m_layout->addStretch(1);
}
