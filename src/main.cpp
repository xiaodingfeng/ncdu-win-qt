#include <QApplication>
#include <QStyleHints>

#include "MainWindow.h"
#include "Style.h"
#include "I18n.h"

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#  include <shlobj.h>
#endif

int main(int argc, char* argv[])
{
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("NcduWin");
    app.setApplicationDisplayName("NcduWin");
    app.setApplicationVersion("1.0.1");

    // Load persisted language preference (or auto-detect from system locale).
    I18n::load();

    // Apply the application stylesheet.
    app.setStyleSheet(loadQSS());

    MainWindow window;
    window.show();

    return app.exec();
}
