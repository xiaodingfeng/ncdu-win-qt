#pragma once

#include <QSettings>
#include <QStringList>

// Lowercase "DisplayName" of every installed program (64-bit, 32-bit and
// per-user uninstall keys). Used by the data-folder mover to label
// auto-discovered folders with the software they belong to.
inline QStringList installedProgramNames()
{
    static const char* kRoots[] = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };

    QStringList names;
    for (const char* root : kRoots) {
        QSettings reg(QString::fromLatin1(root), QSettings::NativeFormat);
        const QStringList groups = reg.childGroups();
        for (const QString& group : groups) {
            const QString name = reg.value(group + QStringLiteral("/DisplayName"))
                                     .toString().trimmed().toLower();
            if (!name.isEmpty())
                names << name;
        }
    }
    names.removeDuplicates();
    return names;
}
