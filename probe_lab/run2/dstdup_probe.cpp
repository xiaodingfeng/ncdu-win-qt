// Batch-move destination resolution tests (MoveDstResolver.h).
// Rules under test (ALWAYS-QUALIFIED semantics, 360-style mirror):
//   * every folder lands as root\<source-parent>\<Name> — unconditionally
//   * same-named folders from Roaming and Local never collide
//   * queued folders for the same root count as taken (any form)
//   * case-insensitive comparison (NTFS)
//   * unresolvable clash (same qualified path queued) -> ok=false, empty map
//   * result map is POSITION-keyed (0..n-1) — the contract callers must remap
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <cstdio>
#include "MoveDstResolver.h"

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
    gLog = fopen("dstdup_probe.log", "w");

    const QString root = QStringLiteral("D:\\MoveTarget");

    // 1. Plain batch, no name collisions at all -> STILL parent-qualified.
    {
        bool ok = false;
        const auto m = resolveBatchDst(root, {QStringLiteral("AppA"), QStringLiteral("AppB")},
                                       {QStringLiteral("C:\\Users\\u\\AppData\\Roaming\\AppA"),
                                        QStringLiteral("C:\\Users\\u\\AppData\\Local\\AppB")},
                                       {}, &ok);
        CHECK(ok, "plain batch resolves");
        CHECK(m.value(0) == root + QStringLiteral("\\Roaming\\AppA"), "AppA mirrored as Roaming\\AppA");
        CHECK(m.value(1) == root + QStringLiteral("\\Local\\AppB"), "AppB mirrored as Local\\AppB");
    }

    // 2. Roaming/Local same-name pair -> both qualified, one batch, no clash.
    {
        bool ok = false;
        const auto m = resolveBatchDst(root,
                                       {QStringLiteral("Foo"), QStringLiteral("Foo")},
                                       {QStringLiteral("C:\\Users\\u\\AppData\\Roaming\\Foo"),
                                        QStringLiteral("C:\\Users\\u\\AppData\\Local\\Foo")},
                                       {}, &ok);
        CHECK(ok, "roaming/local pair resolves");
        CHECK(m.value(0) == root + QStringLiteral("\\Roaming\\Foo"), "roaming Foo qualified");
        CHECK(m.value(1) == root + QStringLiteral("\\Local\\Foo"), "local Foo qualified");
    }

    // 3. Queued flat name (entry queued under the old flat semantics) cannot
    //    clash with a parent-qualified landing.
    {
        bool ok = false;
        const auto m = resolveBatchDst(root, {QStringLiteral("Foo")},
                                       {QStringLiteral("C:\\Users\\u\\AppData\\Local\\Foo")},
                                       {QStringLiteral("Foo")}, &ok);
        CHECK(ok, "queued flat name does not block qualified landing");
        CHECK(m.value(0) == root + QStringLiteral("\\Local\\Foo"), "lands at Local\\Foo");
    }

    // 4. Queued structured rel from a DIFFERENT parent leaves the slot free.
    {
        bool ok = true;
        const auto m = resolveBatchDst(root, {QStringLiteral("Foo")},
                                       {QStringLiteral("C:\\Users\\u\\AppData\\Roaming\\Foo")},
                                       {QStringLiteral("Local\\Foo")}, &ok);
        CHECK(ok, "structured queued rel of other parent resolves");
        CHECK(m.value(0) == root + QStringLiteral("\\Roaming\\Foo"), "lands at Roaming\\Foo");
    }

    // 4b. Retrying a batch whose earlier run already queued the SAME source in
    //     qualified form: the computed destination collides -> unresolvable.
    {
        bool ok = true;
        const auto m = resolveBatchDst(root,
                                       {QStringLiteral("Foo"), QStringLiteral("Foo")},
                                       {QStringLiteral("C:\\Users\\u\\AppData\\Roaming\\Foo"),
                                        QStringLiteral("C:\\Users\\u\\AppData\\Local\\Foo")},
                                       {QStringLiteral("Local\\Foo")}, &ok);
        CHECK(!ok, "same qualified path queued reported as unresolvable");
        CHECK(m.isEmpty(), "unresolvable returns empty map");
    }

    // 5. Case-insensitive comparison: queued "Local\Foo" blocks "local\Foo".
    {
        bool ok = true;
        const auto m = resolveBatchDst(root, {QStringLiteral("Foo")},
                                       {QStringLiteral("C:\\Users\\u\\AppData\\local\\Foo")},
                                       {QStringLiteral("Local\\Foo")}, &ok);
        CHECK(!ok, "case-only qualified clash is caught (NTFS)");
        CHECK(m.isEmpty(), "case clash returns empty map");
    }

    // 6. Mixed batch -> ALL members qualified, no exceptions.
    {
        bool ok = false;
        const auto m = resolveBatchDst(root,
                                       {QStringLiteral("Cache"), QStringLiteral("Cache"), QStringLiteral("AppZ")},
                                       {QStringLiteral("C:\\u\\AppData\\Roaming\\Cache"),
                                        QStringLiteral("C:\\u\\AppData\\Local\\Cache"),
                                        QStringLiteral("C:\\u\\AppData\\Roaming\\AppZ")},
                                       {}, &ok);
        CHECK(ok, "mixed batch resolves");
        CHECK(m.value(0).endsWith(QStringLiteral("\\Roaming\\Cache")), "first qualified");
        CHECK(m.value(1).endsWith(QStringLiteral("\\Local\\Cache")), "second qualified");
        CHECK(m.value(2) == root + QStringLiteral("\\Roaming\\AppZ"), "non-colliding also qualified");
    }

    // 7. Two different names from the SAME parent both qualify without
    //    falsely clashing with each other.
    {
        bool ok = false;
        const auto m = resolveBatchDst(root, {QStringLiteral("A"), QStringLiteral("B")},
                                       {QStringLiteral("C:\\u\\AppData\\Roaming\\A"),
                                        QStringLiteral("C:\\u\\AppData\\Roaming\\B")},
                                       {}, &ok);
        CHECK(ok, "same-parent pair resolves");
        CHECK(m.value(0) == root + QStringLiteral("\\Roaming\\A"), "A qualified");
        CHECK(m.value(1) == root + QStringLiteral("\\Roaming\\B"), "B qualified");
    }

    // 8. Key contract: positions are 0..n-1 and dense — no caller index may
    //    ever be assumed. A remap (picked[k] -> byPos.value(k)) must yield a
    //    destination for EVERY picked entry, including a selection not
    //    starting at 0 (the panel-side bug this guards against).
    {
        const QList<int> picked = {3, 7};   // panel indices, not positions
        bool ok = false;
        const auto byPos = resolveBatchDst(root, {QStringLiteral("Foo"), QStringLiteral("Bar")},
                                           {QStringLiteral("C:\\u\\AppData\\Local\\Foo"),
                                            QStringLiteral("C:\\u\\AppData\\LocalLow\\Bar")},
                                           {}, &ok);
        CHECK(ok, "remap scenario resolves");
        QMap<int, QString> byDir;
        for (int k = 0; k < picked.size(); ++k)
            byDir.insert(picked.at(k), byPos.value(k));
        bool allNonEmpty = true;
        for (int dirIndex : picked)
            if (byDir.value(dirIndex).isEmpty()) allNonEmpty = false;
        CHECK(allNonEmpty, "position-keyed map remaps densely to panel indices");
    }

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
