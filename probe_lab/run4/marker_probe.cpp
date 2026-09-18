// writeDontDeleteMarker harness.
//
// The icon itself is language-neutral (a plain warning triangle), but the FILE
// NAME is the sentence the user reads in Explorer, so it follows the active UI
// language. What this checks, against the real WinApi.cpp + the app's own qrc:
//   1. the name comes from locales/ for both shipped languages,
//   2. a move in English writes the English name, in Chinese the Chinese one,
//   3. a folder already marked in one language is NOT marked again in the other
//      (switching language must not leave two icons in one folder),
//   4. the payload really is the embedded .ico,
//   5. an empty path is refused.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <cstdio>
#include "WinApi.h"
#include "I18n.h"

static int gPass = 0, gFail = 0;
static FILE* gLog = nullptr;
#define CHECK(cond, name) do { \
    if (cond) { ++gPass; fprintf(gLog, "[PASS] %s\n", name); } \
    else      { ++gFail; fprintf(gLog, "[FAIL] %s\n", name); } \
    fflush(gLog); \
} while (0)

// Every .ico sitting in *dir* — the count is the point: one marker, never two.
static QStringList icosIn(const QString& dir)
{
    return QDir(dir).entryList({QStringLiteral("*.ico")}, QDir::Files);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    gLog = fopen("marker_probe.log", "w");

    // Dump the embedded resource tree — the qt_add_resources alias may differ
    // from what the app expects.
    QDir resRoot(QStringLiteral(":/"));
    for (const QString& top : resRoot.entryList(QDir::Dirs)) {
        QDir d(QStringLiteral(":/") + top);
        fprintf(gLog, "[RES] :/%s -> %s\n", top.toUtf8().constData(),
                d.entryList().join(QStringLiteral(", ")).toUtf8().constData());
    }

    const QString root = QCoreApplication::applicationDirPath() + "/fixture/marker";

    // ---- 1. both spellings come from the locale files ------------------------
    // persist=false: a probe must not leave the app itself in the language it
    // happened to test last.
    I18n::setLanguage(QStringLiteral("en"), false);
    const QString enName = I18n::tr(QStringLiteral("app_move.marker_name"));
    I18n::setLanguage(QStringLiteral("zh"), false);
    const QString zhName = I18n::tr(QStringLiteral("app_move.marker_name"));

    fprintf(gLog, "[INFO] en name  = %s\n", enName.toUtf8().constData());
    fprintf(gLog, "[INFO] zh name  = %s\n", zhName.toUtf8().constData());
    // If this fails the probe ran without locales/ next to the exe (I18n hands
    // back the key itself), which would make every name check below vacuous.
    CHECK(enName != QStringLiteral("app_move.marker_name")
              && zhName != QStringLiteral("app_move.marker_name"),
          "marker name is translated (locales/ next to the exe)");
    CHECK(enName == QStringLiteral("!Do not delete, move or rename this folder.ico"),
          "English name is English");
    CHECK(zhName == QStringLiteral("!请勿删除、移动或重命名文件夹.ico"),
          "Chinese name is Chinese");
    CHECK(!enName.contains(QStringLiteral("请勿")),
          "the English marker carries no Chinese text");

    const QStringList names = WinApi::dontDeleteMarkerNames();
    fprintf(gLog, "[INFO] known marker names: %s\n",
            names.join(QStringLiteral(" | ")).toUtf8().constData());
    CHECK(names.size() == 2 && names.contains(enName) && names.contains(zhName),
          "both shipped languages are recognised");
    CHECK(names.first() == zhName, "the active language's name is offered first");

    // ---- 2. an English session writes the English name ----------------------
    // The active language at the moment of the move is what decides the name,
    // so it is set immediately before each write.
    I18n::setLanguage(QStringLiteral("en"), false);
    const QString enDir = root + "/en";
    QDir(enDir).removeRecursively();
    QDir().mkpath(enDir);
    CHECK(WinApi::writeDontDeleteMarker(enDir), "marker written (English UI)");
    CHECK(QFile::exists(enDir + QLatin1Char('/') + enName),
          "the file on disk is named in English");
    CHECK(!QFile::exists(enDir + QLatin1Char('/') + zhName),
          "no Chinese-named file in an English session");

    QFile f(enDir + QLatin1Char('/') + enName);
    const bool opened = f.open(QIODevice::ReadOnly);
    const QByteArray payload = opened ? f.readAll() : QByteArray();
    fprintf(gLog, "[INFO] marker size: %lld bytes\n", static_cast<long long>(payload.size()));
    CHECK(payload.size() > 1000, "marker has real ico payload");
    // ICONDIR: reserved=0, type=1 (icon). The payload really is the icon, not
    // an empty or truncated file.
    CHECK(payload.size() > 6 && payload.at(0) == '\0' && payload.at(1) == '\0'
              && payload.at(2) == '\1' && payload.at(3) == '\0',
          "payload is a valid .ico");
    CHECK(icosIn(enDir).size() == 1, "exactly one marker file");

    // ---- 3. re-running in the other language does not double-mark -----------
    CHECK(WinApi::writeDontDeleteMarker(enDir), "second call is a no-op success");
    CHECK(icosIn(enDir).size() == 1, "repeat call adds no second marker");

    I18n::setLanguage(QStringLiteral("zh"), false);
    CHECK(WinApi::writeDontDeleteMarker(enDir), "already-marked folder accepted (now Chinese UI)");
    CHECK(icosIn(enDir).size() == 1 && !QFile::exists(enDir + QLatin1Char('/') + zhName),
          "switching language does not add a second marker");

    // ---- 4. a Chinese session writes the Chinese name ----------------------
    // Still Chinese-active from the check above.
    const QString zhDir = root + "/zh";
    QDir(zhDir).removeRecursively();
    QDir().mkpath(zhDir);
    CHECK(WinApi::writeDontDeleteMarker(zhDir), "marker written (Chinese UI)");
    CHECK(QFile::exists(zhDir + QLatin1Char('/') + zhName),
          "the file on disk is named in Chinese");
    CHECK(icosIn(zhDir).size() == 1, "exactly one marker file (Chinese)");

    CHECK(!WinApi::writeDontDeleteMarker(QString()), "empty dir rejected");

    fprintf(gLog, "RESULT: %d passed, %d failed\n", gPass, gFail);
    fclose(gLog);
    return gFail == 0 ? 0 : 1;
}
