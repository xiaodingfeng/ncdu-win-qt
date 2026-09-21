# NcduWin

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Qt6](https://img.shields.io/badge/GUI-Qt6-green.svg)](https://www.qt.io)
[![Platform: Windows](https://img.shields.io/badge/platform-windows-lightgrey.svg)](#支持的平台)
[![Version](https://img.shields.io/badge/version-1.0.8-blue.svg)](#)

**[English](README.md)** | **[中文](README_ZH.md)**

> 一款现代、浅色主题的 Windows 磁盘使用分析器，灵感来自 Linux 的 [`ncdu`](https://dev.yorhel.nl/ncdu)。

NcduWin 是一个原生桌面应用，它扫描您的磁盘并使用 ncdu 风格的文件列表 **和** 矩形树图来可视化文件夹大小。它使用 **C++17 和 Qt 6** 构建，以安装包形式分发（所有依赖已打包），开箱即支持英语和简体中文。

---

## ✨ 功能特性

- **极速扫描** — 直接读取 NTFS 磁盘索引表（MFT）枚举文件，扫描速度比传统目录遍历方式快数倍；多线程处理让大容量硬盘依然保持流畅响应。
- **重复文件查找** — 采用三级漏斗算法（大小分组 → 前 4KB 哈希 → 全文件哈希），以极低的 I/O 与内存开销精准识别重复文件。自动跳过系统二进制文件与用户数据，每组默认保留首个文件。
- **AI 智能分析** — 选中文件、目录或待清理项，交给 AI 说明它的用途、是否可以安全清理以及清理风险，并可连续追问；兼容 OpenAI 风格接口，接口地址、API Key 与模型均可在「AI → AI 设置」中配置。
- **主题切换** — 支持深色 / 浅色主题切换，并可自定义主题色，打造个性化视觉体验。
- **跳过重型目录** — 可选择性跳过 `node_modules`、`.git` 等大型文件夹的深度扫描，同时仍显示其大小。
- **完整系统访问** — 自动请求管理员权限，支持扫描 `C:\Windows` 和其他用户目录等受保护的系统文件夹。
- **双重视图** — 左侧 ncdu 风格文件列表，右侧交互式树图，直观分析空间占用。
- **智能排序** — 点击任意列按名称、大小、百分比、文件数量或类型排序。
- **软件识别** — 自动识别常见软件安装目录（Adobe、JetBrains、Microsoft Office、Steam 等）和开发项目。
- **面包屑导航** — 快速跳转到任意父目录。
- **悬停提示** — 鼠标悬停时显示完整文件路径和软件识别信息。
- **安全删除选项** — 右键发送到回收站（可撤销）或永久删除。
- **一键磁盘清理** — 专门的清理标签页检测并移除：
  - 常见垃圾：临时文件、浏览器缓存、pip/npm 缓存
  - 大文件（>50MB），带安全等级分类
  - 重复文件，支持分组选择清理
- **安全优先删除** — 五级安全系统（S/A/B/C/D）确保不会误删重要文件，系统状态与用户数据永不自动选中。
- **系统配置优化** — 独立的「系统优化」标签页，一站式修复常见系统与网络问题：
  - Windows 更新守护：一键开关自动更新，关闭后不再自动下载安装，需要时随时恢复
  - 网络急救修复：重置 IP / DNS / IPv6 / Winsock / 防火墙并续约 DHCP，全程显示进度
  - 刷新 DNS 缓存、重置应用商店缓存
- **保存位置迁移** — 把占用 C 盘的目录整体搬到其他磁盘，原位置保留目录联接（junction），软件无需重新配置即可照常使用：
  - 系统文件夹：下载 / 文档 / 桌面 / 图片 / 视频 / 音乐 / 3D 对象 / 联系人 / 收藏夹 / 链接 / 保存的游戏 / 搜索，共 12 个 Windows 库文件夹
  - 软件数据目录：自动扫描 AppData / LocalAppData / LocalLow 及文档下的用户数据，支持勾选批量迁移
- **迁移安全机制** — 复制 → 校验 → 建立目录联接 → 删除旧目录的分步流程，任一步失败可一键还原；目标目录会写入"请勿删除、移动或重命名"提示标记，避免误删导致数据不同步。
- **占用程序处理** — 搬移前先检查目录被哪些程序占用，逐个列出「程序 + PID」并询问是否关闭：可关闭的先礼后兵（请求退出 → 超时强制结束），系统关键程序（资源管理器、登录进程、安全软件等）绝不自动关闭，只提示手动处理。搬移未完成时同样给出这份清单并支持重试续做；选择跳过则保持目录原样、取消勾选，稍后可重新搬移。搬移失败时会明确指出「是哪个程序、为什么关不掉、该手动退出还是重启后立即重试」，不留一句笼统的"被占用"。
- **搬回前先确认没有程序在用原位置** — 搬回要把原位置清空后重写，若此时程序仍在运行，运行中的软件会写进一个正在被清空的目录，回来的副本也无法校验。所以点「恢复」后先查一遍占用：有关得掉的程序就问一次是否关闭，关掉后重新确认再搬；关不掉（或关掉又自己起来）就不动手，原位置和另一块盘的数据都保持原样，并直接说明是谁在占用、该手动退出还是重启后立即重试。宁可不动，也不留下「半搬回」的残局。
- **残留自动清理** — 搬回时若目标位置的副本删不掉（里面的文件正被程序占用），不会当作"已完成"收场：再次列出占用者并询问是否关闭后重试；仍删不掉就记在案，该行出现「清理残留」按钮，点一下即可随时重来。此后重新搬移也不会再撞上"存在相同目录名目录"的死胡同，而是直接引导先清理。
- **状态筛选** — 软件数据目录列表的「状态」列自带筛选入口，可一次勾选多个状态（例如只看「未完成」和「待搬移」），面板不会勾一次就关；被筛掉的目录只是隐藏，勾选状态原样保留。「已勾选 / 合计」只统计看得见的行，搬移也只针对看得见的行，不会把隐藏的目录一起搬走。
- **扫描耗时统计** — 状态栏显示每次扫描的实际耗时，格式化显示为毫秒 / 秒 / 分秒 / 时分。
- **启动自动扫描** — 首次启动立即显示用户主目录的空间使用情况。
- **中英双语** — 内置本地化支持，语言选择自动保存。
- **清新现代界面** — 柔和配色、圆角设计、清晰的视觉层次。
- **安装包分发** — 一键安装，自动创建桌面和开始菜单快捷方式，自带卸载程序。

---

## 📸 截图

| 文件列表 + 树图 | 清理面板 |
|---|---|
| ![File list + treemap](docs/screenshots/treeMap.png) | ![Cleanup panel](docs/screenshots/cleanup.png) |

| AI 分析 | 系统优化 |
|---|---|
| ![AI analysis](docs/screenshots/ai.png) | ![System optimization](docs/screenshots/system.png) |

| 系统文件夹位置迁移 | 用户数据目录位置迁移 |
|---|---|
| ![System folder relocation](docs/screenshots/systemMenu.png) | ![User data folder relocation](docs/screenshots/userMenu.png) |

---

## 🚀 快速开始

### 选项 A — 下载发布版本

1. 前往 [Releases](../../releases) 页面。
2. 下载 `NcduWin_Setup.exe`。
3. 运行安装程序，按提示完成安装。无需任何运行时环境。

### 选项 B — 从源码构建

**环境要求：**
- Windows 10/11
- [Qt 6.x](https://www.qt.io/download)（msvc2019_64 或 msvc2022_64）
- [CMake](https://cmake.org/download/) 3.16+
- Visual Studio 2022（或 Build Tools）含 MSVC

```bash
git clone https://github.com/xiaodingfeng/ncdu-win-qt.git
cd ncdu-win-qt
scripts\build.bat
```

构建脚本产出 `dist\NcduWin_1.0.8_Setup.exe` 安装包（含所有 Qt 依赖）。

### 选项 C — 在 Visual Studio 中打开

1. 启动 **Visual Studio 2022**。
2. **文件 → 打开 → 文件夹**，选择项目根目录。
3. 工具栏 **解决方案配置** 下拉选择 **VS 2022 (Debug)**。
4. **启动项** 下拉将显示 `NcduWin.exe` — 选中它。
5. 按 **F5** 编译并运行。

> 项目会自动检测 `C:\Qt\6.x\` 下的 Qt 6 安装。
> Debug 和 Release 构建均会自动部署对应版本的 Qt DLL。

---

## 📁 项目布局

```
ncdu-win-qt/
├── src/                    # C++ 源码
│   ├── main.cpp            # 入口
│   ├── version.h.in        # 版本模板（由 CMake 处理）
│   ├── core/               # 核心数据与工具
│   │   ├── DiskScanner.h/cpp
│   │   ├── FileNode.h
│   │   ├── FormatHelpers.h/cpp
│   │   ├── I18n.h/cpp
│   │   ├── Identify.h/cpp
│   │   ├── KnownFolderPath.h   # 系统文件夹路径解析（含 OneDrive 重定向）
│   │   ├── KnownFolderTable.h  # 12 个系统文件夹唯一定义表
│   │   ├── Logger.h/cpp
│   │   ├── MemoryMonitor.h
│   │   ├── MftScanner.h/cpp    # NTFS MFT 直读（快速通道）
│   │   ├── MoveDstResolver.h   # 批量迁移的目标路径推导
│   │   ├── SafeMoveWorker.h/cpp  # 可取消、带校验的安全搬移线程
│   │   ├── ScanRoots.h         # 用户数据扫描根目录
│   │   └── WinApi.h/cpp
│   ├── ui/                 # UI 组件
│   │   ├── AppDataMovePanel.h/cpp   # 软件数据目录迁移面板
│   │   ├── AppPathSyncDialog.h/cpp  # 系统文件夹位置迁移对话框
│   │   ├── BreadcrumbBar.h/cpp
│   │   ├── DialogI18n.h        # 统一确认 / 提示弹框
│   │   ├── InstalledApps.h     # 已安装程序发现
│   │   ├── LegendBar.h/cpp
│   │   ├── MainWindow.h/cpp
│   │   ├── SizeBarDelegate.h/cpp
│   │   ├── Style.h
│   │   ├── SystemOptPanel.h/cpp    # 系统配置优化面板
│   │   ├── ToggleSwitch.h/cpp
│   │   └── TreemapWidget.h/cpp
│   ├── ai/                 # AI 分析
│   │   ├── AiAnalysisDialog.h/cpp  # 分析结果窗口
│   │   ├── AiService.h/cpp         # 请求与流式响应
│   │   └── AiSettingsDialog.h/cpp  # 模型与密钥设置
│   └── cleanup/            # 清理功能
│       ├── CleanupPanel.h/cpp
│       ├── CleanupScanner.h/cpp
│       ├── CleanupTarget.h
│       ├── CleanupWorker.h/cpp
│       └── DuplicateScanner.h/cpp  # 重复文件检测
├── locales/                # i18n JSON 文件
│   ├── en.json
│   └── zh.json
├── scripts/
│   ├── build.bat           # 构建脚本 (CMake + MSVC + windeployqt + ISCC)
│   ├── check_i18n.py       # 语言包审计（对称性 / 覆盖率 / 死键）
│   ├── installer.iss       # Inno Setup 安装包脚本
│   └── version.iss.in      # 安装包版本模板
├── tests/
│   ├── test_scanner.cpp    # C++ 单元测试 (Qt Test)
│   └── compare_scanners.cpp  # 扫描器对比基准测试
├── probe_lab/              # 回归靶场（9 个探针 / 292 项断言）
│   ├── CMakeLists.txt
│   └── run1/ … run9/       # 搬移安全 / 标记文件名 / 系统文件夹 / 语言刷新 / 占用识别 / 残留清理 / 状态筛选
├── resources/              # 随程序嵌入的资源
│   └── dont_delete_folder.ico  # 迁移目标"请勿删除"标记图标
├── docs/                   # 官网与截图
│   ├── index.html
│   └── screenshots/
│       ├── treeMap.png
│       ├── cleanup.png
│       ├── ai.png
│       ├── system.png
│       ├── systemMenu.png
│       └── userMenu.png
├── app.ico
├── app.manifest
├── CMakeLists.txt
├── CMakePresets.json
├── CMakeSettings.json
├── LICENSE
├── README.md
└── README_ZH.md
```

---

## ⌨️ 键盘快捷键

| 快捷键 | 操作 |
|---|---|
| `Ctrl+O` | 打开文件夹… |
| `F5` | 重新扫描当前目录 |
| `Ctrl+Q` | 退出 |
| `Backspace` | 跳转到父文件夹 |
| `Enter` / `Return` | 打开选中的文件夹 |
| `Delete` | 将选中项移至回收站 |
| `Shift+Delete` | 永久删除选中项 |
| `Ctrl+F` | 聚焦搜索框 |
| `Esc` | 清除搜索 / 返回上层 |

---

## 🌍 添加翻译

1. 复制 `locales/en.json` 到 `locales/<code>.json`（例如 `ja.json`、`fr.json`）。
2. 翻译所有值。
3. 在 `src/core/I18n.cpp` 中注册语言：
   ```cpp
   {"en", "English"},
   {"zh", "简体中文"},
   {"ja", "日本語"},
   ```
4. 新语言将自动出现在 **语言** 菜单中。

---

## 🧪 开发

### 命令行构建
```bash
scripts\build.bat
build\Release\test_scanner.exe   # 运行单元测试
```

### Visual Studio 构建
打开项目文件夹，工具栏选择 **VS 2022 (Debug)** 预设即可。
Debug 和 Release 构建均支持，Qt DLL 自动部署。

---

## 📝 许可证

MIT © NcduWin Contributors。详见 [LICENSE](LICENSE)。

本项目灵感来自 Yoran Heling 的 [`ncdu`](https://dev.yorhel.nl/ncdu)，并使用 Bruls、Houtman 和 van Wijk（2000）的矩形树图算法。所有商标归各自所有者所有。
