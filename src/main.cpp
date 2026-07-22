#include <QApplication>
#include <QStyleHints>

#include "MainWindow.h"
#include "Style.h"
#include "I18n.h"
#include "Logger.h"

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
#endif

int main(int argc, char* argv[])
{
    // QApplication must exist before Logger::init() (it uses
    // QCoreApplication::applicationDirPath()).
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("NcduWin");
    app.setApplicationDisplayName("NcduWin");
    app.setApplicationVersion("1.0.3");

    // Install global logger BEFORE any other component — captures Qt
    // warnings, C++ uncaught exceptions, and Windows SEH crashes.
    Logger::init();
    Logger::info(QString("Application started (v%1)").arg(app.applicationVersion()));

    // Load persisted language preference (or auto-detect from system locale).
    I18n::load();

    // Load persisted theme preference and resolve "system" against the OS
    // color scheme before applying the stylesheet, so the first paint uses
    // the correct palette (fixes invisible text when Windows is in dark mode).
    Theme::load();
    Theme::applyEffective();
    app.setStyleSheet(loadQSS());

    MainWindow window;
    window.show();

    return app.exec();
}
