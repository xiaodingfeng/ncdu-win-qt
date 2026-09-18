#pragma once

#include <QString>
#include <QStringList>

// Formatting helpers for human-readable size, count, and drive listing.
// human_count, list_drives, and get_home_dir functions.

// Format a byte count as a human-readable string (B/KiB/MiB/GiB/TiB/PiB/EiB).
// Values below 1024 are shown as integers in B; larger values use 1 decimal.
QString humanSize(qint64 bytes);

// Format an item count: <1000 as integer, <1M as "X.Xk", else "X.XM".
QString humanCount(int n);

// Format a duration in milliseconds for the status bar: "820 ms", "3.4 s",
// "1 min 12 s", "1 h 05 min". Localised unit suffixes are supplied by the
// caller through I18n, so this returns digits plus a fixed short unit.
QString humanDuration(qint64 ms);

// Enumerate available Windows drive root paths (e.g. "C:\", "D:\").
QStringList listDrives();

// Return the user's home directory path.
QString getHomeDir();
