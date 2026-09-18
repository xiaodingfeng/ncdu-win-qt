#include "FormatHelpers.h"

#include <QDir>
#include <QFileInfo>

#include "I18n.h"

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
// humanDuration - format milliseconds for the status bar
// ---------------------------------------------------------------------------

QString humanDuration(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    if (ms < 1000)
        return I18n::tr("time.ms", QMap<QString, QString>{{"ms", QString::number(ms)}});
    if (ms < 60000) {
        // One decimal is what makes a short scan informative. It is rounded
        // first, because 59 999 ms would otherwise print as "60.0 秒" — a value
        // that belongs in the minutes format, where it reads "1 分 00 秒".
        const int tenths = static_cast<int>((ms + 50) / 100);
        if (tenths >= 600) {
            return I18n::tr("time.min_sec", QMap<QString, QString>{
                {"min", QStringLiteral("1")},
                {"sec", QStringLiteral("00")}});
        }
        return I18n::tr("time.sec", QMap<QString, QString>{
            {"sec", QString::number(tenths / 10.0, 'f', 1)}});
    }
    if (ms < 3600000) {
        const int min = static_cast<int>(ms / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        return I18n::tr("time.min_sec", QMap<QString, QString>{
            {"min", QString::number(min)},
            {"sec", QString::number(sec).rightJustified(2, QLatin1Char('0'))}});
    }
    const int hour = static_cast<int>(ms / 3600000);
    const int min = static_cast<int>((ms % 3600000) / 60000);
    return I18n::tr("time.hour_min", QMap<QString, QString>{
        {"hour", QString::number(hour)},
        {"min", QString::number(min).rightJustified(2, QLatin1Char('0'))}});
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
