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
    // Enable Qt high-DPI attributes BEFORE creating the QApplication
    // (this is the recommended way in Qt 5.6+ to get correct scaling).
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

#ifdef _WIN32
    // Try to set Windows DPI awareness to Per-Monitor v2 (if available).
    // This helps avoid blurry scaling and makes per-monitor DPI changes work.
    // Use runtime checks so code still builds on older SDKs.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        // SetProcessDpiAwarenessContext is available on Windows 10 Creators Update and later.
        typedef BOOL(WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
        SetProcessDpiAwarenessContext_t setDpiAwarenessCtx = (SetProcessDpiAwarenessContext_t)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setDpiAwarenessCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            setDpiAwarenessCtx((HANDLE)-4);
        } else {
            // Fallback: try Shcore.dll SetProcessDpiAwareness (Windows 8.1+)
            HMODULE shcore = LoadLibraryA("Shcore.dll");
            if (shcore) {
                typedef HRESULT(WINAPI *SetProcessDpiAwareness_t)(int);
                SetProcessDpiAwareness_t setProcessDpiAwareness =
                    (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
                if (setProcessDpiAwareness) {
                    // PROCESS_PER_MONITOR_DPI_AWARE = 2
                    setProcessDpiAwareness(2);
                }
                FreeLibrary(shcore);
            } else {
                // Last fallback for very old Windows: SetProcessDPIAware()
#pragma warning(push)
#pragma warning(disable: 4996)
                SetProcessDPIAware();
#pragma warning(pop)
            }
        }
    }
#endif

    // QApplication must exist before Logger::init() (it uses
    // QCoreApplication::applicationDirPath()).
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("NcduWin");
    app.setApplicationDisplayName("NcduWin");
    app.setApplicationVersion("1.0.9");

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
