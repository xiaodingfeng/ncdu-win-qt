// processesLockingDir (Windows Restart Manager wrapper) harness.
// The probe locks its OWN fixture file, so the locking process it must find is
// the probe itself — a fully self-contained end-to-end check of the RM path.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdio>
#include <windows.h>
#include "WinApi.h"
#include "Logger.h"

static int gPass = 0, gFail = 0;
static FILE* gLog = nullptr;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Logger::init();   // diagnostics from processesLockingDir land in ncduwin.log
    gLog = fopen("lockproc_probe.log", "w");

    const QString base = QCoreApplication::applicationDirPath() + "/fixture";

    // Scenario A: a file held open with no sharing -> the dir counts as locked
    // and the probe's own exe name must appear among the lockers.
    const QString dirA = base + "/locked";
    QDir().mkpath(dirA + "/nested");
    {
        QFile f(dirA + "/nested/hold.txt");
        f.open(QIODevice::WriteOnly);
        f.write("held");
    }
    HANDLE h = CreateFileW((const wchar_t*)QDir::toNativeSeparators(dirA + "/nested/hold.txt").utf16(),
                           GENERIC_READ, 0 /*no sharing -> exclusive*/, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(gLog, "[SKIP] could not lock fixture file: %lu\n", GetLastError());
        fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
        fclose(gLog);
        return 0;
    }

    QStringList procs;
    const int n = WinApi::processesLockingDir(dirA, &procs);
    fprintf(gLog, "[INFO] found %d locking process(es): %s\n", n,
            procs.join(QStringLiteral(", ")).toUtf8().constData());
    CHECK(n > 0, "locked dir reports at least one process");
    CHECK(!procs.isEmpty(), "process names resolved");
    bool self = false;
    for (const QString& p : procs)
        if (p.contains(QStringLiteral("lockproc"), Qt::CaseInsensitive))
            self = true;
    CHECK(self, "probe's own exe among the lockers");
    CloseHandle(h);

    // Scenario B: an unlocked tree -> no processes.
    const QString dirB = base + "/free";
    QDir().mkpath(dirB);
    {
        QFile f(dirB + "/plain.txt");
        f.open(QIODevice::WriteOnly);
        f.write("free");
    }
    procs.clear();
    const int n2 = WinApi::processesLockingDir(dirB, &procs);
    CHECK(n2 == 0 && procs.isEmpty(), "unlocked dir reports none");

    // Scenario C: a missing directory must not crash or report anything.
    procs.clear();
    const int n3 = WinApi::processesLockingDir(base + "/does-not-exist", &procs);
    CHECK(n3 == 0 && procs.isEmpty(), "missing dir handled gracefully");

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
