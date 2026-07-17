#pragma once

#include <QString>
#include <QMap>

// Global application logger.
//
// Writes timestamped, leveled messages to a single log file (ncduwin.log)
// next to the executable. Thread-safe. Captures:
//   - Direct calls via Logger::info / warn / error
//   - Qt messages (qDebug / qWarning / qCritical) via qInstallMessageHandler
//   - Windows SEH crashes (access violations, etc.) via SetUnhandledExceptionFilter
//   - C++ uncaught exceptions via std::set_terminate
//
// The log file is truncated on each application start so it stays small.
namespace Logger {

// Open the log file and install global handlers. Call once at the very
// start of main(), before any Qt objects are created.
void init();

// Direct logging API.
void info(const QString& msg);
void warn(const QString& msg);
void error(const QString& msg);

// Returns the absolute path to the log file.
QString logPath();

}  // namespace Logger
