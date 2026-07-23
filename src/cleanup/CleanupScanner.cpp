#include "CleanupScanner.h"

#include "MemoryMonitor.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <map>
#include <stack>

// ---------------------------------------------------------------------------
// Default constants
// ---------------------------------------------------------------------------
namespace {
constexpr qint64 MIN_LARGE_FILE_SIZE = 50LL * 1024 * 1024;   // 50 MB
constexpr qint64 LARGE_ARCHIVE_MIN_SIZE = 100LL * 1024 * 1024;  // 100 MB
constexpr int MAX_LARGE_FILES = 200;

// File extensions for tmp/log/bak classification.
const QStringList& tmpFileSuffixes()
{
    static const QStringList s = {".tmp", ".log", ".bak", ".old", ".cache"};
    return s;
}

const QStringList& archiveSuffixes()
{
    static const QStringList s = {".zip", ".rar", ".7z", ".tar", ".iso",
                                  ".msi", ".dmg", ".tgz", ".xz"};
    return s;
}

const QStringList& pycSuffixes()
{
    static const QStringList s = {".pyc", ".pyo"};
    return s;
}

// Build cache subdirectory names (obj, build, dist, .cache, ...).
const QStringList& buildCacheNames()
{
    static const QStringList s = {"obj", "build", "dist", ".cache",
                                  ".mypy_cache", ".ruff_cache",
                                  ".pytest_cache", ".tox", ".eggs"};
    return s;
}

// Names to skip when recursing for __pycache__ / node_modules.
const QSet<QString>& pycacheSkipNames()
{
    static const QSet<QString> s = {"node_modules", ".git", "venv", ".venv"};
    return s;
}

const QSet<QString>& nodeModulesSkipNames()
{
    static const QSet<QString> s = {".git", "venv", ".venv"};
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / cancel
// ---------------------------------------------------------------------------

CleanupScanner::CleanupScanner(const QString& scanPath,
                               std::shared_ptr<FileNode> rootNode,
                               QObject* parent)
    : QThread(parent)
    , m_scanPath(QFileInfo(scanPath).absoluteFilePath())
    , m_rootNode(std::move(rootNode))
{
}

void CleanupScanner::cancel()
{
    m_cancel = true;
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

QString CleanupScanner::normalizePath(const QString& path)
{
    // Normalize: absolute path, lowercased, forward slashes.
    QString p = QFileInfo(path).absoluteFilePath();
    p = p.toLower();
    p.replace('\\', '/');
    return p;
}

bool CleanupScanner::pathUnder(const QString& path, const QString& rootNorm)
{
    const QString pn = normalizePath(path);
    QString root = rootNorm;
    while (root.endsWith('/'))
        root.chop(1);
    return pn == root || pn.startsWith(root + '/');
}

bool CleanupScanner::pathExistsAndAccessible(const QString& path) const
{
    const QFileInfo info(path);
    return info.exists();
}

// ---------------------------------------------------------------------------
// Directory size computation (fallback when no FileNode tree is available)
// ---------------------------------------------------------------------------

qint64 CleanupScanner::computePathSize(const QString& path) const
{
    QFileInfo info(path);
    if (!info.exists())
        return 0;
    if (info.isFile())
        return info.size();

    // Iterative traversal — avoids C++ stack overflow on deeply nested trees.
    qint64 total = 0;
    std::stack<QString> pending;
    pending.push(path);
    while (!pending.empty()) {
        const QString cur = pending.top();
        pending.pop();
        QDir dir(cur);
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& e : entries) {
            if (e.isDir())
                pending.push(e.absoluteFilePath());
            else
                total += e.size();
        }
    }
    return total;
}

int CleanupScanner::countFiles(const QString& path) const
{
    QFileInfo info(path);
    if (!info.exists())
        return 0;
    if (info.isFile())
        return 1;

    // Iterative traversal — avoids C++ stack overflow on deeply nested trees.
    int count = 0;
    std::stack<QString> pending;
    pending.push(path);
    while (!pending.empty()) {
        const QString cur = pending.top();
        pending.pop();
        QDir dir(cur);
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& e : entries) {
            if (e.isDir())
                pending.push(e.absoluteFilePath());
            else
                ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// makeTarget — conditionally append a target if path exists & is under root
// ---------------------------------------------------------------------------

CleanupTarget CleanupScanner::makeTarget(const QString& key, const QString& path,
                                          DangerLevel danger, bool isDir,
                                          bool checked, bool enabled,
                                          const QString& remark)
{
    CleanupTarget t;
    t.key = key;
    t.path = path;
    t.danger = danger;
    t.isDir = isDir;
    t.checked = checked;
    t.enabled = enabled;
    t.remark = remark;
    return t;
}

// ---------------------------------------------------------------------------
// discoverTargets — finds all cleanable targets
// ---------------------------------------------------------------------------

void CleanupScanner::discoverTargets(std::vector<CleanupTarget>& out)
{
    const QString rootNorm = normalizePath(m_scanPath);

    const QString user = qEnvironmentVariable("USERPROFILE");
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    const QString roaming = qEnvironmentVariable("APPDATA");
    const QString win = qEnvironmentVariable("WINDIR");

    const QString userDir = !user.isEmpty() ? user
        : QDir::homePath();
    const QString localDir = !local.isEmpty() ? local
        : QDir::cleanPath(userDir + "/AppData/Local");
    const QString roamingDir = !roaming.isEmpty() ? roaming
        : QDir::cleanPath(userDir + "/AppData/Roaming");
    const QString winDir = !win.isEmpty() ? win
        : QStringLiteral("C:/Windows");

    // Only add if path exists and is under root.
    auto add = [&](const QString& key, const QString& path,
                   DangerLevel danger, bool enabled, bool checked,
                   const QString& remark = QString()) {
        if (!pathExistsAndAccessible(path))
            return;
        if (!pathUnder(path, rootNorm))
            return;
        const QFileInfo info(path);
        out.push_back(makeTarget(key, path, danger, info.isDir(),
                                 checked, enabled, remark));
    };

    // ── S-level: system-generated temp/cache ─────────────────────────────
    add("cleanup.s_temp", QDir(m_scanPath).absoluteFilePath("temp"),
        DangerLevel::S, true, true, "cleanup.remark_s_temp");
    add("cleanup.s_cache", QDir(m_scanPath).absoluteFilePath("cache"),
        DangerLevel::S, true, true, "cleanup.remark_s_cache");

    // System TEMP / TMP environment variable
    {
        const QString sysTemp = !qEnvironmentVariable("TEMP").isEmpty()
            ? qEnvironmentVariable("TEMP")
            : qEnvironmentVariable("TMP");
        if (!sysTemp.isEmpty() && pathUnder(sysTemp, rootNorm)) {
            add("cleanup.junk.windows_temp", sysTemp,
                DangerLevel::S, true, true, "cleanup.remark_s_temp");
        }
    }

    // User temp directory
    add("cleanup.junk.user_temp",
        QDir::cleanPath(userDir + "/AppData/Local/Temp"),
        DangerLevel::S, true, true, "cleanup.remark_s_temp");

    // Recursive __pycache__ discovery
    discoverPycache(m_scanPath, out);

    // .tmp/.log/.bak/.old/.cache files in root
    discoverTmpFiles(m_scanPath, out);

    // ── A-level: user cache, regeneratable ──────────────────────────────
    add("cleanup.a_temp", QDir(m_scanPath).absoluteFilePath("tmp"),
        DangerLevel::A, true, true, "cleanup.remark_a_temp");

    // Build cache directories under the scan root
    for (const auto& cacheName : buildCacheNames()) {
        add("cleanup.a_build_cache",
            QDir(m_scanPath).absoluteFilePath(cacheName),
            DangerLevel::A, true, true, "cleanup.remark_a_build");
    }

    // Recursive node_modules discovery
    discoverNodeModules(m_scanPath, out);

    // .pyc/.pyo files in root
    discoverCompiledPy(m_scanPath, out);

    // ── Browser caches (Chrome, Edge, Firefox) ─────────────────────────
    {
        // Chrome Cache
        add("cleanup.a_browser_cache",
            QDir::cleanPath(localDir + "/Google/Chrome/User Data/Default/Cache"),
            DangerLevel::A, true, true, "cleanup.remark_a_browser_cache");
        // Chrome Code Cache
        add("cleanup.a_browser_cache",
            QDir::cleanPath(localDir + "/Google/Chrome/User Data/Default/Code Cache"),
            DangerLevel::A, true, true, "cleanup.remark_a_browser_cache");
        // Edge Cache
        add("cleanup.a_browser_cache",
            QDir::cleanPath(localDir + "/Microsoft/Edge/User Data/Default/Cache"),
            DangerLevel::A, true, true, "cleanup.remark_a_browser_cache");
        // Edge Code Cache
        add("cleanup.a_browser_cache",
            QDir::cleanPath(localDir + "/Microsoft/Edge/User Data/Default/Code Cache"),
            DangerLevel::A, true, true, "cleanup.remark_a_browser_cache");
        // Firefox profiles — iterate each profile's cache2 dir
        {
            const QString ffProfiles = QDir::cleanPath(
                userDir + "/AppData/Roaming/Mozilla/Firefox/Profiles");
            if (pathExistsAndAccessible(ffProfiles) && pathUnder(ffProfiles, rootNorm)) {
                QDir ffDir(ffProfiles);
                const auto profiles = ffDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const auto& profile : profiles) {
                    const QString cacheDir = QDir::cleanPath(
                        profile.absoluteFilePath() + "/cache2");
                    if (pathExistsAndAccessible(cacheDir)) {
                        out.push_back(makeTarget(
                            "cleanup.a_browser_cache", cacheDir,
                            DangerLevel::A, true, true,
                            "cleanup.remark_a_browser_cache"));
                    }
                }
            }
        }
    }

    // ── Package manager caches ───────────────────────────────────────────
    // pip cache
    add("cleanup.a_pip_cache",
        QDir::cleanPath(localDir + "/pip/cache"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // npm cache (Local)
    add("cleanup.a_npm_cache",
        QDir::cleanPath(userDir + "/AppData/Local/npm-cache"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // npm cache (Roaming)
    add("cleanup.a_npm_cache",
        QDir::cleanPath(userDir + "/AppData/Roaming/npm-cache"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // NuGet cache
    add("cleanup.a_nuget_cache",
        QDir::cleanPath(userDir + "/.nuget/packages"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // Gradle cache
    add("cleanup.a_gradle_cache",
        QDir::cleanPath(userDir + "/.gradle/caches"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // Maven cache
    add("cleanup.a_maven_cache",
        QDir::cleanPath(userDir + "/.m2/repository"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // cargo cache
    add("cleanup.a_cargo_cache",
        QDir::cleanPath(userDir + "/.cargo/registry/cache"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // yarn cache
    add("cleanup.a_yarn_cache",
        QDir::cleanPath(localDir + "/Yarn/cache"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // Go module cache
    add("cleanup.a_go_cache",
        QDir::cleanPath(userDir + "/go/pkg/mod/cache"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // Composer cache (PHP)
    add("cleanup.a_composer_cache",
        QDir::cleanPath(userDir + "/AppData/Local/Composer/files"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // Docker (Windows)
    add("cleanup.a_docker_cache",
        QDir::cleanPath(localDir + "/Docker/wsl/data"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    // Conda package caches
    add("cleanup.a_conda_cache",
        QDir::cleanPath(userDir + "/miniconda3/pkgs"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    add("cleanup.a_conda_cache",
        QDir::cleanPath(userDir + "/anaconda3/pkgs"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");
    add("cleanup.a_conda_cache",
        QDir::cleanPath(userDir + "/AppData/Local/conda/conda/pkgs"),
        DangerLevel::A, true, true, "cleanup.remark_a_pkg_cache");

    // ── Windows known cache locations ──────────────────────────────────
    // Windows Temp (S-level)
    add("cleanup.junk.windows_temp",
        QDir::cleanPath(winDir + "/Temp"),
        DangerLevel::S, true, true, "cleanup.remark_s_temp");
    // Windows Prefetch (A-level)
    add("cleanup.a_prefetch",
        QDir::cleanPath(winDir + "/Prefetch"),
        DangerLevel::A, true, true, "cleanup.remark_a_prefetch");
    // Windows Error Reporting — LiveKernelReports (B-level)
    add("cleanup.b_error_reports",
        QDir::cleanPath(winDir + "/LiveKernelReports"),
        DangerLevel::B, true, false, "cleanup.remark_b_error_reports");
    // Windows Error Reporting — WER (B-level)
    add("cleanup.b_error_reports",
        QDir::cleanPath(userDir + "/AppData/Local/Microsoft/Windows/WER"),
        DangerLevel::B, true, false, "cleanup.remark_b_error_reports");
    // Windows Font Cache (A-level)
    add("cleanup.a_font_cache",
        QDir::cleanPath(localDir + "/Microsoft/Windows/Fonts"),
        DangerLevel::A, true, true, "cleanup.remark_a_font_cache");
    // Thumbnail cache (A-level)
    add("cleanup.a_thumb_cache",
        QDir::cleanPath(localDir + "/Microsoft/Windows/Explorer"),
        DangerLevel::A, true, true, "cleanup.remark_a_thumb_cache");
    // Windows Logs (B-level)
    add("cleanup.b_win_logs",
        QDir::cleanPath(winDir + "/Logs"),
        DangerLevel::B, true, false, "cleanup.remark_b_win_logs");
    // Recent files (B-level)
    add("cleanup.b_recent_files",
        QDir::cleanPath(userDir + "/AppData/Roaming/Microsoft/Windows/Recent"),
        DangerLevel::B, true, false, "cleanup.remark_b_recent");
    // Internet Explorer cache (A-level)
    add("cleanup.a_ie_cache",
        QDir::cleanPath(localDir + "/Microsoft/Windows/INetCache"),
        DangerLevel::A, true, true, "cleanup.remark_a_ie_cache");
    add("cleanup.a_ie_cache",
        QDir::cleanPath(localDir + "/Microsoft/Windows/Temporary Internet Files"),
        DangerLevel::A, true, true, "cleanup.remark_a_ie_cache");
    // Windows Event Logs (B-level)
    add("cleanup.b_event_logs",
        QDir::cleanPath(winDir + "/System32/winevt/Logs"),
        DangerLevel::B, true, false, "cleanup.remark_b_event_logs");
    // Delivery Optimization (A-level)
    add("cleanup.a_delivery_opt",
        QDir::cleanPath(winDir + "/DeliveryOptimization"),
        DangerLevel::A, true, true, "cleanup.remark_a_delivery_opt");
    // DirectX Shader Cache (A-level)
    add("cleanup.a_shader_cache",
        QDir::cleanPath(localDir + "/D3DSCache"),
        DangerLevel::A, true, true, "cleanup.remark_a_shader_cache");
    add("cleanup.a_shader_cache",
        QDir::cleanPath(userDir + "/AppData/LocalLow/NVIDIA/PerDriverVersion/DXCache"),
        DangerLevel::A, true, true, "cleanup.remark_a_shader_cache");
    add("cleanup.a_shader_cache",
        QDir::cleanPath(userDir + "/AppData/Local/AMD/DxCache"),
        DangerLevel::A, true, true, "cleanup.remark_a_shader_cache");

    // ── B-level: large but potentially useful ───────────────────────────
    add("cleanup.b_logs",
        QDir(m_scanPath).absoluteFilePath("logs"),
        DangerLevel::B, true, false, "cleanup.remark_b_logs");
    add("cleanup.b_old_backups",
        QDir(m_scanPath).absoluteFilePath("backup"),
        DangerLevel::B, true, false, "cleanup.remark_b_backup");
    add("cleanup.b_old_backups",
        QDir(m_scanPath).absoluteFilePath("old"),
        DangerLevel::B, true, false, "cleanup.remark_b_backup");
    add("cleanup.b_downloads",
        QDir(m_scanPath).absoluteFilePath("downloads"),
        DangerLevel::B, true, false, "cleanup.remark_b_downloads");
    add("cleanup.b_downloads",
        QDir(m_scanPath).absoluteFilePath("Downloads"),
        DangerLevel::B, true, false, "cleanup.remark_b_downloads");

    // Large archive files in root (virtual group)
    discoverLargeArchives(m_scanPath, out);

    // Windows Update caches (B-level in _discover_windows_caches)
    add("cleanup.c_win_update",
        QDir::cleanPath(winDir + "/SoftwareDistribution/Download"),
        DangerLevel::B, false, false, "cleanup.remark_c_win_update");
    add("cleanup.c_win_update",
        QDir::cleanPath(winDir + "/SoftwareDistribution/Delivery Optimization"),
        DangerLevel::B, false, false, "cleanup.remark_c_win_update");

    // ── C-level: system state, recovery, updates (disabled) ─────────────
    // Only add these if the scan root itself is under the Windows directory.
    if (pathUnder(rootNorm, normalizePath(winDir))) {
        add("cleanup.c_win_update",
            QDir::cleanPath(winDir + "/SoftwareDistribution/Download"),
            DangerLevel::C, false, false, "cleanup.remark_c_win_update");
        add("cleanup.c_win_update",
            QDir::cleanPath(winDir + "/SoftwareDistribution/Delivery Optimization"),
            DangerLevel::C, false, false, "cleanup.remark_c_win_update");
        add("cleanup.c_win_temp",
            QDir::cleanPath(winDir + "/Temp"),
            DangerLevel::C, false, false, "cleanup.remark_c_win_temp");
        add("cleanup.c_system32",
            QDir::cleanPath(winDir + "/System32"),
            DangerLevel::C, false, false, "cleanup.remark_c_system");
        add("cleanup.c_syswow64",
            QDir::cleanPath(winDir + "/SysWOW64"),
            DangerLevel::C, false, false, "cleanup.remark_c_system");
    }

    // Recycle Bin (B-level) — always at the drive root, not under scan path.
    // The previous code joined scanPath + "$Recycle.Bin" (only valid when
    // scanning a drive root) and required the scan root to be under the user
    // directory (inverted check). Now we compute the drive root from the scan
    // path and add the recycle bin directly if it exists.
    {
        QString driveRoot;
        if (m_scanPath.length() >= 2 && m_scanPath[1] == ':')
            driveRoot = m_scanPath.left(2) + QStringLiteral("/");
        else
            driveRoot = m_scanPath;
        const QString recycleBinPath =
            QDir::cleanPath(driveRoot + QStringLiteral("/$Recycle.Bin"));
        if (pathExistsAndAccessible(recycleBinPath)) {
            const QFileInfo info(recycleBinPath);
            out.push_back(makeTarget(
                "cleanup.b_recycle", recycleBinPath,
                DangerLevel::B, info.isDir(), false,
                "cleanup.remark_b_recycle"));
        }
    }
}

// ---------------------------------------------------------------------------
// Recursive __pycache__ discovery (S-level)
// ---------------------------------------------------------------------------

void CleanupScanner::discoverPycache(const QString& root,
                                     std::vector<CleanupTarget>& out,
                                     int maxDepth)
{
    if (maxDepth <= 0 || m_cancel.load())
        return;

    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (m_cancel.load())
            return;
        const QString name = entry.fileName();
        if (name == "__pycache__") {
            out.push_back(makeTarget(
                "cleanup.s_pycache", entry.absoluteFilePath(),
                DangerLevel::S, true, true,
                "cleanup.remark_s_pycache"));
        } else if (!name.startsWith('.')) {
            if (pycacheSkipNames().contains(name.toLower()))
                continue;
            discoverPycache(entry.absoluteFilePath(), out, maxDepth - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// .tmp/.log/.bak/.old/.cache files in root (S-level virtual group)
// ---------------------------------------------------------------------------

void CleanupScanner::discoverTmpFiles(const QString& root,
                                       std::vector<CleanupTarget>& out)
{
    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Files);
    qint64 total = 0;
    int count = 0;
    for (const auto& entry : entries) {
        const QString nameL = entry.fileName().toLower();
        bool match = false;
        for (const auto& suf : tmpFileSuffixes()) {
            if (nameL.endsWith(suf)) {
                match = true;
                break;
            }
        }
        if (match) {
            total += entry.size();
            ++count;
        }
    }
    if (count > 0) {
        CleanupTarget t = makeTarget(
            "cleanup.s_tmp_files", root,
            DangerLevel::S, false, true, true,
            "cleanup.remark_s_tmp_files");
        t.size = total;
        t.fileCount = count;
        out.push_back(t);
    }
}

// ---------------------------------------------------------------------------
// Recursive node_modules discovery (A-level)
// ---------------------------------------------------------------------------

void CleanupScanner::discoverNodeModules(const QString& root,
                                         std::vector<CleanupTarget>& out,
                                         int maxDepth)
{
    if (maxDepth <= 0 || m_cancel.load())
        return;

    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (m_cancel.load())
            return;
        const QString name = entry.fileName();
        if (name == "node_modules") {
            out.push_back(makeTarget(
                "cleanup.a_node_modules", entry.absoluteFilePath(),
                DangerLevel::A, true, true,
                "cleanup.remark_a_node_modules"));
        } else if (!name.startsWith('.')) {
            if (nodeModulesSkipNames().contains(name.toLower()))
                continue;
            discoverNodeModules(entry.absoluteFilePath(), out, maxDepth - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// .pyc/.pyo files in root (A-level virtual group)
// ---------------------------------------------------------------------------

void CleanupScanner::discoverCompiledPy(const QString& root,
                                        std::vector<CleanupTarget>& out)
{
    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Files);
    qint64 total = 0;
    int count = 0;
    for (const auto& entry : entries) {
        const QString nameL = entry.fileName().toLower();
        for (const auto& suf : pycSuffixes()) {
            if (nameL.endsWith(suf)) {
                total += entry.size();
                ++count;
                break;
            }
        }
    }
    if (count > 0) {
        CleanupTarget t = makeTarget(
            "cleanup.a_pyc", root,
            DangerLevel::A, false, true, true,
            "cleanup.remark_a_pyc");
        t.size = total;
        t.fileCount = count;
        out.push_back(t);
    }
}

// ---------------------------------------------------------------------------
// Large archive files >100MB in root (B-level virtual group)
// ---------------------------------------------------------------------------

void CleanupScanner::discoverLargeArchives(const QString& root,
                                           std::vector<CleanupTarget>& out)
{
    QDir dir(root);
    const auto entries = dir.entryInfoList(QDir::Files);
    qint64 total = 0;
    int count = 0;
    for (const auto& entry : entries) {
        const QString nameL = entry.fileName().toLower();
        bool match = false;
        // .tar.gz is a two-extension suffix; check it specially.
        if (nameL.endsWith(".tar.gz"))
            match = true;
        if (!match) {
            for (const auto& suf : archiveSuffixes()) {
                if (nameL.endsWith(suf)) {
                    match = true;
                    break;
                }
            }
        }
        if (match && entry.size() > LARGE_ARCHIVE_MIN_SIZE) {
            total += entry.size();
            ++count;
        }
    }
    if (count > 0) {
        CleanupTarget t = makeTarget(
            "cleanup.b_large_archives", root,
            DangerLevel::B, false, false, true,
            "cleanup.remark_b_large_archives");
        t.size = total;
        t.fileCount = count;
        out.push_back(t);
    }
}

// ---------------------------------------------------------------------------
// File danger classification
// ---------------------------------------------------------------------------

DangerLevel CleanupScanner::classifyFileDanger(const QString& path, const QString& name)
{
    const QString nameL = name.toLower();
    const QString pn = normalizePath(path);
    const QString win = normalizePath(
        !qEnvironmentVariable("WINDIR").isEmpty()
            ? qEnvironmentVariable("WINDIR")
            : QStringLiteral("C:/Windows"));

    // D-level: user personal folders (hidden from large-file cleanup list)
    static const QStringList personalSegments = {
        "documents", "desktop", "pictures", "videos", "music", "onedrive"
    };
    for (const auto& seg : personalSegments) {
        if (pn.contains('/' + seg + '/'))
            return DangerLevel::D;
    }

    // C-level: anything under the real Windows directory
    if (pn == win || pn.startsWith(win + '/'))
        return DangerLevel::C;

    // C-level: installed program directories — deleting files from here can
    // break installed applications. Covers Program Files, Program Files (x86),
    // and ProgramData (shared application state).
    const QString progFiles = normalizePath(
        !qEnvironmentVariable("ProgramFiles").isEmpty()
            ? qEnvironmentVariable("ProgramFiles")
            : QStringLiteral("C:/Program Files"));
    const QString progFilesX86 = normalizePath(
        !qEnvironmentVariable("ProgramFiles(x86)").isEmpty()
            ? qEnvironmentVariable("ProgramFiles(x86)")
            : QStringLiteral("C:/Program Files (x86)"));
    const QString programData = normalizePath(
        !qEnvironmentVariable("ProgramData").isEmpty()
            ? qEnvironmentVariable("ProgramData")
            : QStringLiteral("C:/ProgramData"));
    if (pn == progFiles || pn.startsWith(progFiles + '/') ||
        pn == progFilesX86 || pn.startsWith(progFilesX86 + '/') ||
        pn == programData || pn.startsWith(programData + '/'))
        return DangerLevel::C;

    // C-level: critical system files at the drive root (pagefile.sys,
    // hiberfil.sys, swapfile.sys) — shown but not cleanable.
    static const QStringList systemRootFiles = {
        "pagefile.sys", "hiberfil.sys", "swapfile.sys"
    };
    for (const auto& sf : systemRootFiles) {
        if (nameL == sf)
            return DangerLevel::C;
    }

    // A-level: cache-like files
    if (nameL.endsWith(".tmp") || nameL.endsWith(".cache") ||
        nameL.endsWith(".log") || nameL.endsWith(".pyc"))
        return DangerLevel::A;

    // B-level: archives, disk images, large data files
    static const QStringList archiveExts = {
        ".zip", ".rar", ".7z", ".tar", ".tgz", ".iso", ".dmg",
        ".vhd", ".vhdx", ".vmdk", ".ova"
    };
    for (const auto& ext : archiveExts) {
        if (nameL.endsWith(ext))
            return DangerLevel::B;
    }

    // Default: B (user can choose)
    return DangerLevel::B;
}

// ---------------------------------------------------------------------------
// Populate target sizes from the FileNode tree
// ---------------------------------------------------------------------------

void CleanupScanner::scanTargetsFromTree(std::vector<CleanupTarget>& targets)
{
    if (!m_rootNode)
        return;

    // Build a path -> FileNode map by walking the tree iteratively.
    // (Already iterative; added memory-pressure guard so we stop walking
    // if RAM gets low — partial target sizing is preferable to a crash.)
    std::map<QString, std::shared_ptr<FileNode>> nodeMap;
    std::vector<std::shared_ptr<FileNode>> stack;
    stack.push_back(m_rootNode);
    while (!stack.empty()) {
        if (MemoryMonitor::isLowMemory())
            break;
        auto node = stack.back();
        stack.pop_back();
        nodeMap[normalizePath(node->path)] = node;
        for (const auto& child : node->children) {
            if (child)
                stack.push_back(child);
        }
    }

    for (auto& target : targets) {
        if (target.size > 0 && target.fileCount > 0)
            continue;
        auto it = nodeMap.find(normalizePath(target.path));
        if (it == nodeMap.end())
            continue;
        auto node = it->second;
        target.size = node->size;
        target.fileCount = node->isDir() ? node->fileCount : 1;
    }
}

// ---------------------------------------------------------------------------
// Large file collection from the tree (mirror find_large_files_from_tree)
// ---------------------------------------------------------------------------

void CleanupScanner::scanLargeFiles(std::vector<LargeFile>& out)
{
    if (!m_rootNode)
        return;

    std::vector<LargeFile> results;
    std::vector<std::shared_ptr<FileNode>> stack;
    stack.push_back(m_rootNode);

    while (!stack.empty()) {
        if (m_cancel.load())
            return;
        auto node = stack.back();
        stack.pop_back();
        if (node->nodeType == NodeType::File && node->size >= MIN_LARGE_FILE_SIZE) {
            LargeFile lf;
            lf.path = node->path;
            lf.name = node->name;
            lf.size = node->size;
            lf.danger = classifyFileDanger(node->path, node->name);
            // Skip system (C-level) files — must not be shown or deleted.
            if (lf.danger == DangerLevel::C)
                continue;
            results.push_back(lf);
        }
        for (const auto& child : node->children) {
            if (child)
                stack.push_back(child);
        }
    }

    // Sort largest-first, cap at MAX_LARGE_FILES
    std::sort(results.begin(), results.end(),
              [](const LargeFile& a, const LargeFile& b) {
                  return a.size > b.size;
              });
    if (results.size() > MAX_LARGE_FILES)
        results.resize(MAX_LARGE_FILES);
    out = std::move(results);
}

// ---------------------------------------------------------------------------
// Main thread entry point
// ---------------------------------------------------------------------------

void CleanupScanner::run()
{
    // Phase 1: discover targets
    std::vector<CleanupTarget> targets;
    discoverTargets(targets);
    if (m_cancel.load())
        return;

    // Phase 2: populate sizes from the tree
    if (m_rootNode) {
        scanTargetsFromTree(targets);
    }

    qint64 totalSize = 0;
    int totalCount = 0;
    for (auto& t : targets) {
        if (m_cancel.load())
            return;
        // Compute size from the filesystem for targets not populated from the
        // FileNode tree (e.g. recycle bin at drive root, browser caches outside
        // the scan root, or when no tree is available at all).
        if (t.size == 0 && t.fileCount == 0 && pathExistsAndAccessible(t.path)) {
            if (t.isDir) {
                t.size = computePathSize(t.path);
                t.fileCount = countFiles(t.path);
            } else {
                t.size = QFileInfo(t.path).size();
                t.fileCount = 1;
            }
        }
        totalSize += t.size;
        totalCount += t.fileCount;
        emit targetScanned(t);
    }

    // Phase 3: find large files
    std::vector<LargeFile> largeFiles;
    scanLargeFiles(largeFiles);
    if (m_cancel.load())
        return;

    qint64 largeTotal = 0;
    for (const auto& lf : largeFiles) {
        if (m_cancel.load())
            return;
        largeTotal += lf.size;
        emit largeFileFound(lf);
    }

    emit finished(targets, largeFiles, totalSize, totalCount, largeTotal);
}
