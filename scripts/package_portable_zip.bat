@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0.."

set "APP_NAME=NcduWin"
set "APP_VERSION=1.0.9"
set "ZIP_NAME=%APP_NAME%_%APP_VERSION%_Portable.zip"
set "ZIP_PATH=dist\%ZIP_NAME%"

if not exist "dist\NcduWin.exe" (
    echo Error: dist\NcduWin.exe was not found.
    echo Build the app first or run scripts\build.bat.
    exit /b 1
)

if exist "%ZIP_PATH%" del /f /q "%ZIP_PATH%"

powershell -NoProfile -ExecutionPolicy Bypass -Command "^ 
  $ErrorActionPreference = 'Stop'; ^
  $src = (Resolve-Path '.\dist').Path; ^
  $zip = Join-Path $src '%ZIP_NAME%'; ^
  $items = Get-ChildItem -Path $src -Force | Where-Object { $_.Name -ne '%ZIP_NAME%' }; ^
  if (-not $items) { throw 'No files found in dist folder to package.' }; ^
  Compress-Archive -Path $items.FullName -DestinationPath $zip -CompressionLevel Optimal -Force; ^
  Write-Host \"Portable ZIP created: $zip\";"

if errorlevel 1 (
    echo Error: ZIP packaging failed.
    exit /b 1
)

echo Portable ZIP: %ZIP_PATH%
echo.
exit /b 0
