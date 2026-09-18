#pragma once

#include <QMessageBox>
#include <QPushButton>

#include "I18n.h"

// Qt's built-in message-box buttons ("Yes", "No", "OK") are not translated in
// this build, so a Chinese UI would still show English buttons. Every
// confirmation in the app goes through these helpers instead, which build their
// buttons from our locale.
namespace Dialogs {

// Yes / No confirmation. Returns true only when Yes was clicked — Escape, the
// close button and "No" all count as a refusal.
inline bool confirm(QWidget* parent, const QString& title, const QString& text,
                    const QString& yesText, const QString& noText)
{
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, parent);
    QPushButton* yes = box.addButton(yesText, QMessageBox::YesRole);
    box.addButton(noText, QMessageBox::NoRole);
    box.setDefaultButton(yes);
    box.exec();
    return box.clickedButton() == yes;
}

inline bool confirm(QWidget* parent, const QString& title, const QString& text)
{
    return confirm(parent, title, text, I18n::tr("button.yes"), I18n::tr("button.no"));
}

// Notice with a single close button.
inline void info(QWidget* parent, const QString& title, const QString& text)
{
    QMessageBox box(QMessageBox::Information, title, text, QMessageBox::NoButton, parent);
    box.addButton(I18n::tr("button.ok"), QMessageBox::AcceptRole);
    box.exec();
}

inline void warn(QWidget* parent, const QString& title, const QString& text)
{
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::NoButton, parent);
    box.addButton(I18n::tr("button.ok"), QMessageBox::AcceptRole);
    box.exec();
}

} // namespace Dialogs
