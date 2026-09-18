#include "ToggleSwitch.h"

#include <QPainter>
#include <QPropertyAnimation>

#include "Style.h"

namespace {
constexpr int kWidth = 46;
constexpr int kHeight = 24;
constexpr int kMargin = 3;
} // namespace

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_anim = new QPropertyAnimation(this, "knob", this);
    m_anim->setDuration(150);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);

    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_anim->stop();
        m_anim->setStartValue(m_knob);
        m_anim->setEndValue(on ? 1.0 : 0.0);
        m_anim->start();
    });
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(kWidth, kHeight);
}

void ToggleSwitch::setKnob(qreal value)
{
    m_knob = value;
    update();
}

void ToggleSwitch::refreshTheme()
{
    update();
}

void ToggleSwitch::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ToggleSwitch::leaveEvent(QEvent* event)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const qreal r = height() / 2.0;
    const QRectF track(0.5, 0.5, width() - 1.0, height() - 1.0);

    QColor trackColor;
    if (!isEnabled())
        trackColor = QColor(QString::fromLatin1(C::BORDER_LIGHT()));
    else if (isChecked())
        trackColor = QColor(QString::fromLatin1(m_hover ? C::PRIMARY_HOVER() : C::PRIMARY()));
    else
        trackColor = QColor(QString::fromLatin1(m_hover ? C::BORDER() : C::SURFACE_DEEP()));

    p.setPen(Qt::NoPen);
    p.setBrush(trackColor);
    p.drawRoundedRect(track, r, r);

    // Knob travels between the two end positions; the animation drives m_knob.
    const qreal knobR = r - kMargin;
    const qreal cx = r + m_knob * (width() - 2.0 * r);
    const QPointF center(cx, height() / 2.0);

    p.setBrush(QColor(QString::fromLatin1(isEnabled() ? C::SURFACE() : C::BORDER_LIGHT())));
    p.drawEllipse(center, knobR, knobR);
}