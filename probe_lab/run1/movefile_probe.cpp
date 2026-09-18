// moveContentsRecursive harness: includes the VERBATIM function extracted
// from src/ui/AppPathSyncDialog.cpp into run1/_func.inc (regenerate with the
// python snippet in project memory after editing the source function).
// Asserts files, shortcuts, hidden files and nested trees all move, and that
// locked files stay behind safely.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include <windows.h>
#include "WinApi.h"

#include "_func.inc"

static int gPass = 0, gFail = 0;
static FILE* gLog = nullptr;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

static const char* kTop = "hello top file";
static const char* kLnk = "dummy-lnk-bytes-1234";
static const char* kHidden = "hidden data";
static const char* kF1 = "file one content";
static const char* kF2 = "file two content";

static void mk(const QString& path, const QByteArray& data, bool hidden = false)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    f.close();
    if (hidden)
        SetFileAttributesW((const wchar_t*)QDir::toNativeSeparators(path).utf16(),
                           FILE_ATTRIBUTE_HIDDEN);
}

static bool same(const QString& path, const char* expect)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return f.readAll() == QByteArray(expect);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    gLog = fopen("movefile_probe.log", "w");
    const QString base = QCoreApplication::applicationDirPath() + "/fixture";
    const QString src = base + "/src";
    const QString dst = base + "/dst";

    QDir(base).removeRecursively();
    QDir().mkpath(src + "/SubA/Deep");
    QDir().mkpath(src + "/emptySub");
    QDir().mkpath(dst);
    mk(src + "/top.txt", kTop);
    mk(src + "/link.lnk", kLnk);
    mk(src + "/hidden.txt", kHidden, true);
    mk(src + "/SubA/file1.txt", kF1);
    mk(src + "/SubA/Deep/file2.txt", kF2);

    std::atomic<qint64> copied{0};
    std::atomic<bool> cancelled{false};
    moveContentsRecursive(src, dst, &copied, &cancelled);

    CHECK(same(dst + "/top.txt", kTop), "top-level file moved");
    CHECK(same(dst + "/link.lnk", kLnk), "shortcut (.lnk) moved byte-for-byte");
    CHECK(same(dst + "/hidden.txt", kHidden), "hidden file moved");
    CHECK(same(dst + "/SubA/file1.txt", kF1), "nested file moved");
    CHECK(same(dst + "/SubA/Deep/file2.txt", kF2), "deeply nested file moved");
    CHECK(QDir(dst + "/emptySub").exists(), "empty folder rebuilt");
    CHECK(!QFile::exists(src + "/top.txt"), "top-level file removed from src");
    CHECK(!QFile::exists(src + "/link.lnk"), "shortcut removed from src");
    CHECK(!QFile::exists(src + "/SubA"), "nested tree removed from src");
    CHECK(!QFile::exists(src), "src dir itself removed when fully drained");

    // Overwrite at destination.
    const QString src2 = base + "/src2", dst2 = base + "/dst2";
    QDir().mkpath(src2); QDir().mkpath(dst2);
    mk(dst2 + "/top.txt", "OLD");
    mk(src2 + "/top.txt", kTop);
    std::atomic<qint64> copied2{0};
    moveContentsRecursive(src2, dst2, &copied2, &cancelled);
    CHECK(same(dst2 + "/top.txt", kTop), "existing dst file overwritten by move");

    // Locked file stays behind, siblings still move.
    const QString src3 = base + "/src3", dst3 = base + "/dst3";
    QDir().mkpath(src3); QDir().mkpath(dst3);
    mk(src3 + "/free.txt", "free");
    mk(src3 + "/locked.txt", "locked");
    HANDLE hLock = CreateFileW((const wchar_t*)QDir::toNativeSeparators(src3 + "/locked.txt").utf16(),
                               GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    std::atomic<qint64> copied3{0};
    moveContentsRecursive(src3, dst3, &copied3, &cancelled);
    if (hLock != INVALID_HANDLE_VALUE) CloseHandle(hLock);
    CHECK(same(dst3 + "/free.txt", "free"), "unlocked sibling moved despite locked file");
    if (hLock != INVALID_HANDLE_VALUE) {
        CHECK(QFile::exists(src3 + "/locked.txt"), "locked file stays in src");
        CHECK(!QFile::exists(dst3 + "/locked.txt"), "locked file not half-copied to dst");
        CHECK(QDir(src3).exists(), "src dir kept while a file could not move");
    } else {
        fprintf(gLog, "[SKIP] could not lock file on this machine\n");
    }

    // Cancel before start touches nothing.
    const QString src4 = base + "/src4", dst4 = base + "/dst4";
    QDir().mkpath(src4); QDir().mkpath(dst4);
    mk(src4 + "/a.txt", "aaa");
    mk(src4 + "/b.txt", "bbb");
    std::atomic<qint64> copied4{0};
    std::atomic<bool> cancelled4{true};
    moveContentsRecursive(src4, dst4, &copied4, &cancelled4);
    CHECK(QFile::exists(src4 + "/a.txt") && QFile::exists(src4 + "/b.txt"),
          "cancelled run touches nothing");

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
