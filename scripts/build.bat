@echo off
setlocal enabledelayedexpansion

REM --- Switch to project root (script lives in scripts/) ---
cd /d "%~dp0.."

REM --- Ensure cmake is available (add Qt's bundled CMake to PATH) ---
where cmake >nul 2>nul
if errorlevel 1 (
    if exist "C:\Qt\Tools\CMake_64\bin\cmake.exe" (
        set "PATH=C:\Qt\Tools\CMake_64\bin;%PATH%"
    ) else if exist "C:\Program Files\CMake\bin\cmake.exe" (
        set "PATH=C:\Program Files\CMake\bin;%PATH%"
    ) else if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" (
        set "PATH=C:\Program Files (x86)\CMake\bin;%PATH%"
    )
)

echo ========================================
echo   NcduWin Build Script
echo ========================================
echo.

REM --- Find Qt6 installation ---
set QT_PATH=

if defined Qt6_DIR (
    set QT_PATH=%Qt6_DIR%
    echo Found Qt6_DIR environment variable.
) else (
    for /d %%Q in (C:\Qt\6.*) do (
        if exist "%%Q\msvc2019_64" (
            set QT_PATH=%%Q\msvc2019_64
        )
        if exist "%%Q\msvc2022_64" (
            set QT_PATH=%%Q\msvc2022_64
        )
    )
    if defined QT_PATH (
        echo Found Qt6 at: !QT_PATH!
    )
)

if not defined QT_PATH (
    echo Error: Qt6 not found.
    echo Set the Qt6_DIR environment variable or install Qt6 to C:\Qt\6.x\msvc2019_64
    exit /b 1
)

echo Using Qt6: %QT_PATH%
echo.

REM --- Clean previous build ---
if exist build rmdir /s /q build
if exist dist rmdir /s /q dist

REM --- Step 1: CMake Configure ---
echo [1/6] Configuring...
cmake -B build -DCMAKE_PREFIX_PATH="%QT_PATH%"
if errorlevel 1 (
    echo Error: CMake configuration failed.
    exit /b 1
)

REM --- Step 2: Build ---
echo.
echo [2/6] Building Release...
cmake --build build --config Release
if errorlevel 1 (
    echo Error: Build failed.
    exit /b 1
)

REM --- Step 3: Deploy Qt dependencies ---
echo.
echo [3/6] Deploying Qt dependencies...
"%QT_PATH%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw build\Release\NcduWin.exe
if errorlevel 1 (
    echo Error: windeployqt failed.
    exit /b 1
)
REM Remove large DX shader DLLs not needed by this application
if exist build\Release\dxcompiler.dll del build\Release\dxcompiler.dll
if exist build\Release\dxil.dll del build\Release\dxil.dll

REM --- Step 4: Copy locales ---
echo.
echo [4/6] Copying locales...
if not exist build\Release\locales mkdir build\Release\locales
xcopy /Y /Q locales\* build\Release\locales\ >nul
if errorlevel 1 (
    echo Error: Locales copy failed.
    exit /b 1
)

REM --- Step 5: Copy to dist ---
echo.
echo [5/6] Copying to dist...
if not exist dist mkdir dist
robocopy build\Release dist /E /NJH /NJS /NDL /NP >nul 2>nul
if errorlevel 8 (
    echo Error: Copy to dist failed.
    exit /b 1
)

REM --- Step 6: Build installer ---
echo.
echo [6/6] Building installer...
set ISCC=
if defined INNO_SETUP_DIR if exist "%INNO_SETUP_DIR%\ISCC.exe" set "ISCC=%INNO_SETUP_DIR%\ISCC.exe"
if defined ISCC goto :RUN_ISCC
REM Use where/dir to find ISCC; fallback to hardcoded known paths
where ISCC.exe >nul 2>&1
if not errorlevel 1 set "ISCC=ISCC.exe" & goto :RUN_ISCC
dir "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" >nul 2>&1
if not errorlevel 1 set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" & goto :RUN_ISCC
dir "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" >nul 2>&1
if not errorlevel 1 set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" & goto :RUN_ISCC
dir "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" >nul 2>&1
if not errorlevel 1 set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe" & goto :RUN_ISCC
dir "C:\Program Files\Inno Setup 6\ISCC.exe" >nul 2>&1
if not errorlevel 1 set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe" & goto :RUN_ISCC
echo   Warning: Inno Setup (ISCC.exe) not found - installer not built.
echo   Install Inno Setup 6 from: https://jrsoftware.org/isdl.php
echo   Or set INNO_SETUP_DIR to its install path.
goto :END_ISCC

:RUN_ISCC
echo   Using ISCC: %ISCC%
"%ISCC%" scripts\installer.iss
if errorlevel 1 (
    echo Error: Installer build failed.
    exit /b 1
)

:END_ISCC

echo.
echo ========================================
echo   Build Successful!
echo   Output: dist\NcduWin.exe
echo   Installer: dist\NcduWin_1.0.0_Setup.exe
echo ========================================
echo.
echo To run: double-click dist\NcduWin.exe
echo To distribute: copy the entire dist\ folder or use the installer
echo.
pause
endlocal
