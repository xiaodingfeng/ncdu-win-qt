#include "FormatHelpers.h"

#include <QDir>
#include <QFileInfo>

// ---------------------------------------------------------------------------
// humanSize - format bytes to human-readable string
// ---------------------------------------------------------------------------

QString humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("-") + humanSize(-bytes);
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");

    static const char* const units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    for (int i = 0; i < 5; ++i) {
        value /= 1024.0;
        if (value < 1024.0)
            return QString::number(value, 'f', 1) + QStringLiteral(" ") +
                   QString::fromLatin1(units[i]);
    }
    // Past PiB: report in EiB.
    return QString::number(value, 'f', 1) + QStringLiteral(" EiB");
}

// ---------------------------------------------------------------------------
// humanCount - format count to human-readable string
// ---------------------------------------------------------------------------

QString humanCount(int n)
{
    if (n < 1000)
        return QString::number(n);
    if (n < 1000000)
        return QString::number(static_cast<double>(n) / 1000.0, 'f', 1) +
               QStringLiteral("k");
    return QString::number(static_cast<double>(n) / 1000000.0, 'f', 1) +
           QStringLiteral("M");
}

// ---------------------------------------------------------------------------
// listDrives - enumerate available drives (Windows)
// ---------------------------------------------------------------------------

QStringList listDrives()
{
    QStringList drives;
    for (char c = 'A'; c <= 'Z'; ++c) {
        const QString path = QChar(c) + QStringLiteral(":\\");
        if (QFileInfo::exists(path))
            drives << path;
    }
    return drives;
}

// ---------------------------------------------------------------------------
// getHomeDir - get user home directory
// ---------------------------------------------------------------------------

QString getHomeDir()
{
    return QDir::homePath();
}
