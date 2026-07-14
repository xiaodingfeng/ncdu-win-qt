#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <vector>

// LegendBar - horizontal legend showing file-type colors.
//
// LegendBar builds a row of
// swatch + label pairs from legendItems() in Style.h. Labels are translated
// via I18n::tr using the keys from legendItems(); call refreshLabels() after
// a language switch to re-translate.
class LegendBar : public QWidget {
    Q_OBJECT
public:
    explicit LegendBar(QWidget* parent = nullptr);

    // Re-translate all labels after a language switch. Rebuilds the bar so
    // labels pick up new translations.
    void refreshLabels();

private:
    QHBoxLayout* m_layout;
    std::vector<QWidget*> m_items;
};
