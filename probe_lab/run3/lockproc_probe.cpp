// processesLockingDir (Windows Restart Manager wrapper) + the "may we close
// this?" guard harness.
//
// The probe locks its OWN fixture file, so the locking process it must find is
// the probe itself — a fully self-contained end-to-end check of the RM path.
// That also gives the guard its most important case for free: the one process
// we must never terminate is the one running the move, and here it is the one
// holding the lock.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
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

static bool hasPid(const QVector<WinApi::LockingProcess>& procs, quint32 pid)
{
    for (const WinApi::LockingProcess& p : procs)
        if (p.pid == pid)
            return true;
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Logger::init();   // diagnostics from processesLockingDir land in ncduwin.log
    gLog = fopen("lockproc_probe.log", "w");

    const QString base = QCoreApplication::applicationDirPath() + "/fixture";
    const quint32 self = static_cast<quint32>(GetCurrentProcessId());

    // ---------------------------------------------------------------------
    // The guard list: what may never be terminated, and what may.
    // ---------------------------------------------------------------------
    CHECK(WinApi::isNeverCloseName(QStringLiteral("explorer.exe")),
          "guard: explorer.exe is protected");
    CHECK(WinApi::isNeverCloseName(QStringLiteral("C:/Windows/explorer.exe")),
          "guard: a full path to a protected program is recognised");
    CHECK(WinApi::isNeverCloseName(QStringLiteral("lsass.exe")),
          "guard: lsass.exe is protected");
    CHECK(WinApi::isNeverCloseName(QStringLiteral("System")),
          "guard: the System pseudo-process is protected");
    CHECK(!WinApi::isNeverCloseName(QStringLiteral("TIM.exe")),
          "guard: a user program is not protected");
    CHECK(!WinApi::isNeverCloseName(QString()),
          "guard: an empty name is not protected");
    CHECK(!WinApi::neverCloseNames().isEmpty(),
          "guard: the guard list is not empty");

    // ---------------------------------------------------------------------
    // Liveness.
    // ---------------------------------------------------------------------
    CHECK(WinApi::processAlive(self), "processAlive: this probe is alive");
    CHECK(!WinApi::processAlive(0), "processAlive: pid 0 is never alive");
    // A pid that is not in use. Windows recycles ids, so the only requirement is
    // that nothing is running under this one right now.
    CHECK(!WinApi::processAlive(0xFFFFFFF0u), "processAlive: an unused pid is not alive");
    // A console application with no windows of its own: the polite request has
    // nothing to post to, which is a truthful zero rather than an error.
    CHECK(WinApi::requestCloseProcess(self) == 0,
          "requestCloseProcess: a windowless process reports no windows");

    // ---------------------------------------------------------------------
    // Scenario A: a file held open with no sharing -> the dir counts as locked,
    // and the probe's own process is named as the one holding it.
    // ---------------------------------------------------------------------
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

    QVector<WinApi::LockingProcess> procs;
    const int n = WinApi::processesLockingDir(dirA, &procs);
    QStringList names;
    for (const WinApi::LockingProcess& p : procs)
        names << QStringLiteral("%1(%2)").arg(p.name, QString::number(p.pid));
    fprintf(gLog, "[INFO] found %d locking process(es): %s\n", n,
            names.join(QStringLiteral(", ")).toUtf8().constData());
    CHECK(n > 0, "locked dir reports at least one process");
    CHECK(!procs.isEmpty(), "the locking process is described, not just counted");
    CHECK(hasPid(procs, self), "the probe's own process is among the lockers");

    // The path is resolved for every entry, because it is what the guard list is
    // matched against ("Windows Explorer" is not "explorer.exe").
    bool pathOk = true;
    for (const WinApi::LockingProcess& p : procs)
        if (p.pid == self && (p.exePath.isEmpty() || !p.exePath.contains(QStringLiteral("lockproc"),
                                                                        Qt::CaseInsensitive)))
            pathOk = false;
    CHECK(pathOk, "the locker's image path was resolved");

    // The guard must refuse our own process — the whole point of the feature is
    // that it never closes the program doing the moving.
    bool selfRefused = true;
    for (const WinApi::LockingProcess& p : procs)
        if (p.pid == self && (p.safeToClose || p.blockKey != QStringLiteral("proc_close.block_self")))
            selfRefused = false;
    CHECK(selfRefused, "the probe's own process is marked not-closable");

    // And the refusal has to survive the close attempt itself.
    {
        const QVector<WinApi::CloseOutcome> outcomes = WinApi::closeProcesses(procs, 200);
        bool refused = true;
        for (const WinApi::CloseOutcome& o : outcomes)
            if (o.pid == self
                && (o.result != WinApi::CloseOutcome::Refused
                    || o.blockKey != QStringLiteral("proc_close.block_self")))
                refused = false;
        CHECK(refused, "closeProcesses refuses to touch this process");
        CHECK(WinApi::processAlive(self), "this probe survived its own close round");
    }
    CloseHandle(h);

    // ---------------------------------------------------------------------
    // Scenario B: an unlocked tree -> no processes.
    // ---------------------------------------------------------------------
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

    // ---------------------------------------------------------------------
    // Scenario C: a missing directory must not crash or report anything.
    // ---------------------------------------------------------------------
    procs.clear();
    const int n3 = WinApi::processesLockingDir(base + "/does-not-exist", &procs);
    CHECK(n3 == 0 && procs.isEmpty(), "missing dir handled gracefully");

    // ---------------------------------------------------------------------
    // Scenario D: a process that is already gone reads as Exited, not as a kill.
    // ---------------------------------------------------------------------
    {
        QProcess gone;
        gone.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit")});
        gone.waitForFinished(5000);
        const quint32 deadPid = static_cast<quint32>(gone.processId());
        gone.close();
        WinApi::LockingProcess entry;
        entry.pid = deadPid;
        entry.name = QStringLiteral("already-gone");
        entry.safeToClose = true;
        const QVector<WinApi::CloseOutcome> outcomes =
            WinApi::closeProcesses({entry}, 200);
        CHECK(outcomes.size() == 1
                  && outcomes.first().result == WinApi::CloseOutcome::Exited,
              "a process that already exited reports Exited");
    }

    // ---------------------------------------------------------------------
    // Scenario E: the force path really works. A process with no window to close
    // cannot be asked politely, so the grace period expires and it is ended.
    // ping is used because ending it costs nothing and it lingers by design.
    // ---------------------------------------------------------------------
    {
        QProcess linger;
        linger.start(qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/ping.exe"),
                     {QStringLiteral("-n"), QStringLiteral("60"), QStringLiteral("127.0.0.1")});
        if (!linger.waitForStarted(5000)) {
            fprintf(gLog, "[SKIP] could not start the lingering helper\n");
        } else {
            const quint32 pid = static_cast<quint32>(linger.processId());
            CHECK(WinApi::processAlive(pid), "the helper is running before the round");

            WinApi::LockingProcess entry;
            entry.pid = pid;
            entry.name = QStringLiteral("ping.exe");
            entry.safeToClose = true;
            const QVector<WinApi::CloseOutcome> outcomes =
                WinApi::closeProcesses({entry}, 300);
            CHECK(outcomes.size() == 1
                      && outcomes.first().result == WinApi::CloseOutcome::Killed,
                  "a windowless process that ignores the wait is force-closed");
            CHECK(!WinApi::processAlive(pid), "the force-closed helper is gone");
            linger.waitForFinished(5000);
        }
    }

    // ---------------------------------------------------------------------
    // Scenario F: recognising a program that came back after being closed
    // (DJI Studio and friends restart themselves from a helper, a tray agent or
    // a service). This is what separates "close it and carry on" from "closing
    // it is pointless" — the whole reason the user gets told about it instead
    // of being asked a second time.
    // ---------------------------------------------------------------------
    {
        auto mk = [](quint32 pid, const QString& name, const QString& path) {
            WinApi::LockingProcess p;
            p.pid = pid;
            p.name = name;
            p.exePath = path;
            p.safeToClose = true;
            return p;
        };

        CHECK(WinApi::respawnedAmong({mk(1, QStringLiteral("a.exe"),
                                         QStringLiteral("C:/App/a.exe"))},
                                     QStringList())
                  .isEmpty(),
              "nothing was closed, so nothing counts as respawned");

        const QStringList closed{
            QStringLiteral("C:\\Program Files\\DJI Studio\\DJIStudio.exe")};

        // Same program, new pid, different separators and case: still the same
        // program, and that is the only thing that matters here.
        const QStringList back = WinApi::respawnedAmong(
            {mk(4321, QStringLiteral("DJI Studio"),
                QStringLiteral("c:/program files/dji studio/djistudio.EXE"))},
            closed);
        CHECK(back.size() == 1 && back.first() == QStringLiteral("DJI Studio"),
              "a restarted program is recognised across pid, case and separators");

        CHECK(WinApi::respawnedAmong(
                  {mk(7, QStringLiteral("other.exe"), QStringLiteral("C:/Elsewhere/other.exe"))},
                  closed)
                  .isEmpty(),
              "a different program is not reported as respawned");

        // Two instances of the restarted program are one problem, not two.
        const QStringList twice = WinApi::respawnedAmong(
            {mk(11, QStringLiteral("DJI Studio"),
                QStringLiteral("C:\\Program Files\\DJI Studio\\DJIStudio.exe")),
             mk(12, QStringLiteral("DJI Studio"),
                QStringLiteral("c:\\program files\\dji studio\\djistudio.exe"))},
            closed);
        CHECK(twice.size() == 1,
              "two instances of the restarted program collapse into one entry");

        // The image path is not always readable for a process we cannot open.
        const QStringList byName = WinApi::respawnedAmong(
            {mk(99, QStringLiteral("DJI Studio"), QString())},
            {QStringLiteral("DJI Studio")});
        CHECK(byName.size() == 1,
              "an unreadable image path falls back to the display name");
    }

    // ---------------------------------------------------------------------
    // Scenario G: naming the folder's blocker when Restart Manager cannot.
    //
    // Two failures this covers, both of them reported from real machines:
    //
    //   * a data folder with a hundred thousand files. RmGetList only reports
    //     processes holding a file that was REGISTERED, and registering "the
    //     first N the walk happened to return" reliably misses the one file a
    //     program is actually holding. The fixture below puts the locked file
    //     deliberately LAST in directory order and first in modification order —
    //     which is what a log or a database being written looks like.
    //   * a program running FROM inside the folder. It may hold no data file
    //     open at all, so Restart Manager answers "nobody" — and for a folder
    //     that is an application's own working directory, that is exactly the
    //     case that matters most.
    // ---------------------------------------------------------------------
    {
        // Path containment, which both detections above depend on.
        CHECK(WinApi::pathInsideDirectory(QStringLiteral("C:\\Data\\sub\\x.dll"),
                                          QStringLiteral("C:\\Data")),
              "containment: a file below the folder is inside it");
        CHECK(!WinApi::pathInsideDirectory(QStringLiteral("C:\\Data2\\x.dll"),
                                           QStringLiteral("C:\\Data")),
              "containment: a sibling with a longer name is NOT inside it");
        CHECK(WinApi::pathInsideDirectory(QStringLiteral("C:\\Data"),
                                          QStringLiteral("C:\\Data")),
              "containment: the folder itself counts as inside");
        CHECK(WinApi::pathInsideDirectory(QStringLiteral("C:\\Data\\x.dll"),
                                          QStringLiteral("c:/data/")),
              "containment: case and a trailing separator do not matter");
        CHECK(!WinApi::pathInsideDirectory(QStringLiteral("C:\\Other\\x.dll"),
                                           QStringLiteral("C:\\Data")),
              "containment: an unrelated path is outside");
        CHECK(!WinApi::pathInsideDirectory(QString(), QStringLiteral("C:\\Data")),
              "containment: an empty path is never inside anything");
        CHECK(!WinApi::pathInsideDirectory(QStringLiteral("C:\\Data"), QString()),
              "containment: nothing is inside an empty folder path");

        // --- the lock beyond the sampling cap ---
        const QString many = base + QStringLiteral("/many");
        QDir().mkpath(many);
        for (int i = 0; i < 1000; ++i) {
            QFile f(many + QStringLiteral("/a%1.bin").arg(i, 4, 10, QLatin1Char('0')));
            if (f.open(QIODevice::WriteOnly))
                f.write("x");
        }
        // Written last, so it is the newest file in the tree, and named so that
        // it sorts after every one of the thousand above.
        const QString locked = many + QStringLiteral("/zz_locked.db-wal");
        {
            QFile seed(locked);
            if (seed.open(QIODevice::WriteOnly))
                seed.write("seed");
        }
        HANDLE hold = CreateFileW(reinterpret_cast<const wchar_t*>(
                                      QDir::toNativeSeparators(locked).utf16()),
                                  GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hold != INVALID_HANDLE_VALUE, "fixture: the newest file can be held open");
        if (hold != INVALID_HANDLE_VALUE) {
            QVector<WinApi::LockingProcess> procs;
            const int n = WinApi::processesLockingDir(many, &procs);
            CHECK(n >= 1 && hasPid(procs, self),
                  "the holder of the 1001st file is found (newest sampled first)");
            for (const WinApi::LockingProcess& p : procs) {
                if (p.pid != self)
                    continue;
                CHECK(!p.safeToClose
                          && p.blockKey == QLatin1String("proc_close.block_self"),
                      "the process running the move is never offered for closing");
            }

            // The trigger for the whole leftover path. A restore keys its
            // "record this as ours" branch off this answer, so a delete that
            // cannot finish has to say so rather than quietly returning success
            // with a folder full of files still in it — which is exactly how a
            // user ends up with a target folder Explorer refuses to remove and a
            // second move that says the name is taken.
            CHECK(!WinApi::deletePermanent({many}) && QDir(many).exists(),
                  "a delete blocked by an open file reports failure and leaves the folder");

            CloseHandle(hold);
            // ...and the same call works once the holder lets go, which is what
            // makes retrying after closing a program worth doing at all.
            CHECK(WinApi::deletePermanent({many}) && !QDir(many).exists(),
                  "the same delete succeeds once the holder has let go");
        }

        // --- the program running from inside the folder ---
        const QString own = base + QStringLiteral("/own");
        QDir().mkpath(own);
        const QString pingSrc = QStringLiteral("C:/Windows/System32/ping.exe");
        const QString pingDst = own + QStringLiteral("/ping.exe");
        QFile::remove(pingDst);
        const bool copied = QFile::copy(pingSrc, pingDst);
        CHECK(copied, "fixture: a program can be copied into the folder");
        if (copied) {
            QProcess runner;
            runner.setProgram(QDir::toNativeSeparators(pingDst));
            runner.setArguments({QStringLiteral("-n"), QStringLiteral("30"),
                                 QStringLiteral("127.0.0.1")});
            runner.start();
            const bool started = runner.waitForStarted(5000);
            CHECK(started, "fixture: the program inside the folder is running");
            if (started) {
                const quint32 pingPid = static_cast<quint32>(runner.processId());
                QVector<WinApi::LockingProcess> running;
                const int runningHits = WinApi::processesRunningFrom(own, &running);
                CHECK(runningHits >= 1 && hasPid(running, pingPid),
                      "a program running from inside the folder is named");

                // ...and it must reach the caller through the query the UI
                // actually uses, which is the whole point of merging the two.
                QVector<WinApi::LockingProcess> merged;
                WinApi::processesLockingDir(own, &merged);
                CHECK(hasPid(merged, pingPid),
                      "the combined query reports a program with no file open at all");

                int pidSeenOnce = 0;
                for (const WinApi::LockingProcess& p : merged)
                    if (p.pid == pingPid)
                        ++pidSeenOnce;
                CHECK(pidSeenOnce == 1, "one row per process, not one per detection");

                runner.kill();
                runner.waitForFinished(5000);
                CHECK(!WinApi::processAlive(pingPid),
                      "fixture: the program inside the folder is stopped again");
            } else {
                runner.kill();
                runner.waitForFinished(2000);
            }
        }
    }

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
