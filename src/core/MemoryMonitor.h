#pragma once

// MemoryMonitor — lightweight physical-RAM pressure probe for scanners.
//
// Scanners poll isLowMemory() at loop boundaries (every N records / per
// directory) and gracefully degrade when available RAM drops below a
// threshold. The mechanism is a synchronous GlobalMemoryStatusEx poll:
// cheap, no extra threads, no notifications to wire up. Threshold is
// max(200 MB, 5% of total physical RAM) — conservative to avoid
// premature degradation while still catching real pressure before the
// OS starts thrashing or killing the process.

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <QtGlobal>

namespace MemoryMonitor {

// Total physical RAM in bytes (cached after first call).
inline qint64 totalBytes()
{
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    return static_cast<qint64>(m.ullTotalPhys);
}

// Available physical RAM in bytes at the moment of the call.
inline qint64 availableBytes()
{
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    return static_cast<qint64>(m.ullAvailPhys);
}

// Threshold below which isLowMemory() returns true.
// max(200 MB, 5% of total physical RAM).
inline qint64 thresholdBytes()
{
    const qint64 total = totalBytes();
    const qint64 fivePct = total / 20;
    return std::max<qint64>(200LL * 1024 * 1024, fivePct);
}

// Returns true when available physical RAM has dropped below the threshold.
// Scanners should stop adding new entries/directories and finish with what
// they have already collected (partial result beats a crash).
inline bool isLowMemory()
{
    return availableBytes() < thresholdBytes();
}

} // namespace MemoryMonitor
