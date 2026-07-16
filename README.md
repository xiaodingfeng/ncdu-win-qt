# NcduWin

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Qt6](https://img.shields.io/badge/GUI-Qt6-green.svg)](https://www.qt.io)
[![Platform: Windows](https://img.shields.io/badge/platform-windows-lightgrey.svg)](#supported-platforms)
[![Version](https://img.shields.io/badge/version-1.0.1-blue.svg)](#)

**[English](README.md)** | **[中文](README_ZH.md)**

> A modern, light-themed disk usage analyzer for Windows, inspired by Linux [`ncdu`](https://dev.yorhel.nl/ncdu).

NcduWin is a native desktop application that scans your disk and visualizes
folder sizes with both an ncdu-style file list **and** a squarified treemap.
It is built with **C++17 and Qt 6**, ships as an installer with all
dependencies bundled, and supports English / 简体中文 out of the box.

---

## ✨ Features

- **Blazing fast scanning** — multi-threaded disk scanning for maximum speed,
  even on large drives.
- **Skip heavy directories** — optionally skip deep scanning of large
  folders like `node_modules` and `.git` while still showing their sizes.
- **Full system access** — auto-requests admin privileges to scan protected
  system folders like `C:\Windows` and other users' directories.
- **Dual visualization** — ncdu-style file list on the left, interactive
  treemap on the right for intuitive space analysis.
- **Smart sorting** — click any column to sort by Name, Size, Percentage,
  File Count, or Type.
- **Software identification** — recognizes common software install folders
  (Adobe, JetBrains, Microsoft Office, Steam, etc.) and developer projects.
- **Breadcrumb navigation** — quickly jump to any parent directory.
- **Hover tooltips** — see full file paths and software identification
  on mouse hover.
- **Safe delete options** — right-click to send to Recycle Bin (undoable)
  or delete permanently.
- **One-click disk cleanup** — dedicated Cleanup tab detects and removes:
  - Common junk: temp files, browser caches, pip/npm caches
  - Large files (>50MB) with safety-level classification
- **Safety-first deletion** — five-level safety system ensures you won't
  accidentally delete important files.
- **Auto-scan on startup** — instantly shows your home directory usage
  on first launch.
- **English and Chinese** — built-in localization with persistent language
  preference.
- **Clean, modern UI** — soft colors, rounded corners, and clear visual
  hierarchy.
- **Installer package** — one-click setup with desktop and Start Menu
  shortcuts; uninstaller included.

---

## 📸 Screenshots

| File list + treemap | Cleanup panel |
|---|---|
| ![File list + treemap](docs/screenshots/treeMap.png) | ![Cleanup panel](docs/screenshots/cleanup.png) |

---

## 🚀 Quick start

### Option A — Download the release

1. Go to the [Releases](../../releases) page.
2. Download `NcduWin_Setup.exe`.
3. Run the installer and follow the prompts. No runtime required.

### Option B — Build from source

**Requirements:**
- Windows 10/11
- [Qt 6.x](https://www.qt.io/download) (msvc2019_64 or msvc2022_64)
- [CMake](https://cmake.org/download/) 3.16+
- Visual Studio 2022 (or Build Tools) with MSVC

```bash
git clone https://github.com/xiaodingfeng/ncdu-win-qt.git
cd ncdu-win-qt
scripts\build.bat
```

The script produces `dist\NcduWin_1.0.1_Setup.exe` — an installer with
all Qt dependencies bundled.

### Option C — Open in Visual Studio

1. Launch **Visual Studio 2022**.
2. **File → Open → Folder** and select the project root.
3. In the toolbar **Solution Configurations** dropdown, select **VS 2022 (Debug)**.
4. The **Startup Item** dropdown will show `NcduWin.exe` — select it.
5. Press **F5** to build and run.

> The project auto-detects your Qt 6 installation under `C:\Qt\6.x\`.
> Debug and Release builds both deploy the correct Qt DLLs automatically.

---

## 📁 Project layout

```
ncdu-win-qt/
├── src/                    # C++ source files
│   ├── main.cpp            # Entry point
│   ├── version.h.in        # Version template (processed by CMake)
│   ├── core/               # Core data & utilities
│   │   ├── DiskScanner.h/cpp
│   │   ├── FileNode.h
│   │   ├── FormatHelpers.h/cpp
│   │   ├── I18n.h/cpp
│   │   ├── Identify.h/cpp
│   │   └── WinApi.h/cpp
│   ├── ui/                 # UI components
│   │   ├── BreadcrumbBar.h/cpp
│   │   ├── LegendBar.h/cpp
│   │   ├── MainWindow.h/cpp
│   │   ├── SizeBarDelegate.h/cpp
│   │   ├── Style.h
│   │   └── TreemapWidget.h/cpp
│   └── cleanup/            # Cleanup feature
│       ├── CleanupPanel.h/cpp
│       ├── CleanupScanner.h/cpp
│       ├── CleanupTarget.h
│       └── CleanupWorker.h/cpp
├── locales/                # i18n JSON files
│       ├── en.json
│       └── zh.json
├── scripts/
│   ├── build.bat           # Build script (CMake + MSVC + windeployqt + ISCC)
│   ├── installer.iss       # Inno Setup installer script
│   └── version.iss.in      # Version template for installer
├── tests/
│   ├── test_scanner.cpp    # C++ unit tests (Qt Test)
├── docs/
│   └── screenshots/
│       ├── treeMap.png
│       └── cleanup.png
├── app.ico
├── app.manifest
├── CMakeLists.txt
├── LICENSE
├── README.md
└── README_ZH.md
```

---

## ⌨️ Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open folder… |
| `F5` | Re-scan current |
| `Ctrl+Q` | Quit |
| `Backspace` | Go to parent folder |
| `Enter` / `Return` | Open selected folder |
| `Delete` | Move selection to Recycle Bin |
| `Shift+Delete` | Delete selection permanently |
| `Ctrl+F` | Focus the search box |
| `Esc` | Clear search / go up |

---

## 🌍 Adding a translation

1. Copy `locales/en.json` to `locales/<code>.json`
   (e.g. `ja.json`, `fr.json`).
2. Translate the values.
3. Register the language in `src/core/I18n.cpp`:
   ```cpp
   {"en", "English"},
   {"zh", "简体中文"},
   {"ja", "日本語"},
   ```
4. The new language will automatically appear in the **Language** menu.

---

## 🧪 Development

### Command line
```bash
scripts\build.bat
build\Release\test_scanner.exe   # run unit tests
```

### Visual Studio
Open the project folder and select the **VS 2022 (Debug)** preset.
Debug and Release builds are both supported — Qt DLLs deploy automatically.

---

## 📝 License

MIT © NcduWin Contributors. See [LICENSE](LICENSE).

This project is inspired by [`ncdu`](https://dev.yorhel.nl/ncdu) by
Yoran Heling and uses the squarified treemap algorithm by Bruls, Houtman
& van Wijk (2000). All trademarks belong to their respective owners.
