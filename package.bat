@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo Building NcduWin release package, please wait...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-release.ps1"
if errorlevel 1 (
    echo.
    echo Build failed. Check the messages above.
) else (
    echo.
    echo Done.
)

pause
endlocal
