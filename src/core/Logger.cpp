#include "Logger.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QStandardPaths>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

#include <exception>
#include <cstdlib>

namespace {

QMutex g_mutex;
QFile g_logFile;
QString g_logPath;

void writeLine(const QString& level, const QString& msg)
{
    // Build the line outside the lock (cheap), then write under lock.
    QString line = QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
                   + " [" + level + "] " + msg;

    QMutexLocker locker(&g_mutex);
    if (!g_logFile.isOpen())
        return;
    QTextStream ts(&g_logFile);
    ts << line << "\n";
    ts.flush();
}

#ifdef _WIN32
// "Qt6Widgets.dll+0x1a2b3" — an address on its own says nothing about who
// faulted, and a crash log that cannot be acted on is not worth writing.
QString describeAddress(void* addr)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod)
        && mod) {
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
        QString name = n ? QString::fromWCharArray(buf, static_cast<int>(n)) : QString();
        const int slash = name.lastIndexOf(QLatin1Char('\\'));
        if (slash >= 0)
            name = name.mid(slash + 1);
        return QStringLiteral("%1+0x%2")
            .arg(name)
            .arg(reinterpret_cast<quintptr>(addr) - reinterpret_cast<quintptr>(mod),
                 0, 16);
    }
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(addr), 0, 16);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;

    QString msg = QString("SEH CRASH: code=0x%1 addr=%2")
                      .arg((quint64)code, 8, 16, QChar('0'))
                      .arg(describeAddress(addr));

    // Add extra info for common access violations.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR flags = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR vaddr = ep->ExceptionRecord->ExceptionInformation[1];
        msg += QString(" (access violation: %1 at 0x%2)")
                   .arg(flags == 0 ? "read" : (flags == 1 ? "write" : "execute"))
                   .arg((quint64)vaddr, 16, 16, QChar('0'));
    }

    writeLine("CRIT", msg);

    // The frames say which call was running when it happened, which is what
    // turns "it crashed" into something that can be looked up.
    void* frames[32] = {};
    const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
        writeLine("CRIT", QString("  #%1 %2").arg(i).arg(describeAddress(frames[i])));

    return EXCEPTION_EXECUTE_HANDLER;  // let the process terminate
}
#endif

void terminateHandler()
{
    QString msg = "std::terminate called";
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
        msg = QString("uncaught exception: %1").arg(e.what());
    } catch (...) {
        msg = "uncaught unknown exception";
    }
    writeLine("CRIT", msg);
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    Q_UNUSED(ctx);
    QString level;
    switch (type) {
        case QtDebugMsg:    level = "DBG";  break;
        case QtInfoMsg:     level = "INFO"; break;
        case QtWarningMsg:  level = "WARN"; break;
        case QtCriticalMsg: level = "CRIT"; break;
        case QtFatalMsg:    level = "FATAL"; break;
    }
    writeLine(level, msg);
}

}  // namespace

namespace Logger {

void init()
{
    QMutexLocker locker(&g_mutex);

    // Prefer a writable location: next to the executable.
    QString dir = QCoreApplication::applicationDirPath();
    g_logPath = dir + "/ncduwin.log";
    g_logFile.setFileName(g_logPath);
    if (!g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        // Fallback to temp directory.
        g_logPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                    + "/ncduwin.log";
        g_logFile.setFileName(g_logPath);
        if (!g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return;
    }

    // Write a header so we know which run produced the log.
    {
        QTextStream ts(&g_logFile);
        ts << "=== NcduWin log started "
           << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
           << " ===\n";
        ts.flush();
    }

    // Install global handlers (outside the lock to avoid re-entrancy).
    locker.unlock();

    qInstallMessageHandler(qtMessageHandler);
    std::set_terminate(terminateHandler);

#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif
}

void info(const QString& msg)  { writeLine("INFO", msg); }
void warn(const QString& msg)  { writeLine("WARN", msg); }
void error(const QString& msg) { writeLine("ERR",  msg); }

QString logPath()
{
    QMutexLocker locker(&g_mutex);
    return g_logPath;
}

}  // namespace Logger
