#pragma once

#include <QAbstractButton>

class QPropertyAnimation;

// Sliding on/off switch with an animated knob. Used where a card's state reads
// better as "on / off" than as a command button.
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal knob READ knob WRITE setKnob)
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void refreshTheme();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    qreal knob() const { return m_knob; }
    void setKnob(qreal value);

    qreal m_knob = 0.0;   // 0 = off, 1 = on
    bool m_hover = false;
    QPropertyAnimation* m_anim = nullptr;
};