$ErrorActionPreference = 'Stop'

# This script lives in scripts\; the Tauri project root is its parent.
$scriptsDir = $PSScriptRoot
$tauriDir = Split-Path -Parent $scriptsDir
$confPath = Join-Path $tauriDir 'src-tauri\tauri.conf.json'
$appConfigPath = Join-Path $tauriDir 'src-tauri\app-config.json'

if (-not (Test-Path $confPath)) {
    throw "tauri.conf.json not found: $confPath"
}
if (-not (Test-Path $appConfigPath)) {
    throw "app-config.json not found: $appConfigPath"
}

$utf8 = New-Object System.Text.UTF8Encoding($false)

# app-config.json is the single place to change the release version.
$version = (Get-Content $appConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json).version
if ([string]::IsNullOrWhiteSpace($version)) {
    $version = (Get-Content $confPath -Raw -Encoding UTF8 | ConvertFrom-Json).version
}
$version = $version.Trim()

# Keep the build metadata in sync so installer/portable names and file
# versions match app-config.json.
$versionTargets = @(
    @{ Path = $confPath; Pattern = '"version"\s*:\s*"[^"]*"'; Replacement = "`"version`": `"$version`"" },
    @{ Path = (Join-Path $tauriDir 'package.json'); Pattern = '"version"\s*:\s*"[^"]*"'; Replacement = "`"version`": `"$version`"" },
    @{ Path = (Join-Path $tauriDir 'src-tauri\Cargo.toml'); Pattern = '(?m)^version\s*=\s*"[^"]*"'; Replacement = "version = `"$version`"" }
)
foreach ($target in $versionTargets) {
    $raw = Get-Content $target.Path -Raw -Encoding UTF8
    $updated = $raw -replace $target.Pattern, $target.Replacement
    if ($updated -ne $raw) {
        [System.IO.File]::WriteAllText($target.Path, $updated, $utf8)
    }
}

Write-Host "=========================================="
Write-Host "  NcduWin $version release build"
Write-Host "=========================================="
Write-Host ""

# A running copy keeps the executable locked and makes the build fail.
Get-Process -Name 'NcduWin' -ErrorAction SilentlyContinue | Stop-Process -Force

$cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
if (Test-Path $cargoBin) {
    $env:Path = "$cargoBin;$env:Path"
}

Push-Location $tauriDir
try {
    if (-not (Test-Path (Join-Path $tauriDir 'node_modules'))) {
        Write-Host "[1/3] Installing npm dependencies..."
        npm install
        if ($LASTEXITCODE -ne 0) { throw 'npm install failed' }
    } else {
        Write-Host "[1/3] npm dependencies already installed."
    }

    Write-Host "[2/3] Building installer (npx tauri build)..."
    npx tauri build
    if ($LASTEXITCODE -ne 0) { throw 'tauri build failed' }

    $bundle = Join-Path $tauriDir 'src-tauri\target\release\bundle'
    $setup = Join-Path $bundle "nsis\NcduWin_${version}_x64-setup.exe"
    if (-not (Test-Path $setup)) { throw "installer not found: $setup" }

    Write-Host "[3/3] Creating portable package..."
    $portable = Join-Path $bundle 'portable'
    New-Item -ItemType Directory -Force -Path $portable | Out-Null

    # The release binary keeps the crate name; the portable package uses the
    # product name so users see a friendly file name.
    $releaseExe = Join-Path $tauriDir 'src-tauri\target\release\ncduwin.exe'
    if (-not (Test-Path $releaseExe)) { throw "release binary not found: $releaseExe" }
    $portableExe = Join-Path $portable 'NcduWin.exe'
    Copy-Item $releaseExe $portableExe -Force

    $readme = Get-ChildItem $scriptsDir -Filter '*.txt' | Select-Object -First 1
    if (-not $readme) { throw 'portable readme (*.txt) not found in scripts\' }

    # Keep the readme in sync with the release version.
    $text = Get-Content $readme.FullName -Raw -Encoding UTF8
    $text = $text -replace 'NcduWin \d+\.\d+\.\d+', "NcduWin $version"
    $text = $text -replace 'NcduWin_\d+\.\d+\.\d+_x64-setup', "NcduWin_${version}_x64-setup"
    [System.IO.File]::WriteAllText((Join-Path $portable $readme.Name), $text, $utf8)

    $stage = Join-Path $env:TEMP "ncduwin-portable-$version"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item $portableExe $stage -Force
    [System.IO.File]::WriteAllText((Join-Path $stage $readme.Name), $text, $utf8)

    $zip = Join-Path $portable "NcduWin_${version}_x64-portable.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    Remove-Item $stage -Recurse -Force

    $setupSize = [math]::Round((Get-Item $setup).Length / 1MB, 2)
    $zipSize = [math]::Round((Get-Item $zip).Length / 1MB, 2)

    Write-Host ""
    Write-Host "=========================================="
    Write-Host "  Build successful (version $version)"
    Write-Host "  Installer: $setup ($setupSize MB)"
    Write-Host "  Portable : $zip ($zipSize MB)"
    Write-Host "=========================================="
}
finally {
    Pop-Location
}
