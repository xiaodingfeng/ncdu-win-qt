#include "Identify.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSet>
#include <optional>

namespace Identify {

namespace {

// A rule result: an i18n key plus optional interpolation parameters.
struct RuleResult {
    QString key;
    QMap<QString, QString> params;
};

// Well-known install/software folder names (lowercase) -> product name.
// Software name identification table.
const QHash<QString, QString>& softwareNames() {
    static const QHash<QString, QString> m = {
        { QStringLiteral("adobe"),                         QStringLiteral("Adobe") },
        { QStringLiteral("adobe acrobat"),                QStringLiteral("Adobe Acrobat") },
        { QStringLiteral("adobe photoshop"),               QStringLiteral("Adobe Photoshop") },
        { QStringLiteral("adobe illustrator"),             QStringLiteral("Adobe Illustrator") },
        { QStringLiteral("adobe premiere"),               QStringLiteral("Adobe Premiere Pro") },
        { QStringLiteral("adobe creative cloud files"),    QStringLiteral("Adobe Creative Cloud") },
        { QStringLiteral("microsoft office"),             QStringLiteral("Microsoft Office") },
        { QStringLiteral("microsoft edge"),               QStringLiteral("Microsoft Edge") },
        { QStringLiteral("microsoft visual studio"),     QStringLiteral("Microsoft Visual Studio") },
        { QStringLiteral("microsoft vscode"),             QStringLiteral("Microsoft VS Code") },
        { QStringLiteral("microsoft 365"),                QStringLiteral("Microsoft 365") },
        { QStringLiteral("windowsapps"),                  QStringLiteral("Microsoft Store apps") },
        { QStringLiteral("google"),                       QStringLiteral("Google") },
        { QStringLiteral("google chrome"),                QStringLiteral("Google Chrome") },
        { QStringLiteral("google earth"),                 QStringLiteral("Google Earth") },
        { QStringLiteral("mozilla firefox"),              QStringLiteral("Mozilla Firefox") },
        { QStringLiteral("thunderbird"),                  QStringLiteral("Mozilla Thunderbird") },
        { QStringLiteral("python"),                       QStringLiteral("Python") },
        { QStringLiteral("python3"),                      QStringLiteral("Python") },
        { QStringLiteral("python39"),                     QStringLiteral("Python 3.9") },
        { QStringLiteral("python310"),                    QStringLiteral("Python 3.10") },
        { QStringLiteral("python311"),                    QStringLiteral("Python 3.11") },
        { QStringLiteral("python312"),                    QStringLiteral("Python 3.12") },
        { QStringLiteral("python313"),                    QStringLiteral("Python 3.13") },
        { QStringLiteral("python314"),                    QStringLiteral("Python 3.14") },
        { QStringLiteral("jetbrains"),                    QStringLiteral("JetBrains") },
        { QStringLiteral("intellij idea"),                QStringLiteral("IntelliJ IDEA") },
        { QStringLiteral("pycharm"),                      QStringLiteral("JetBrains PyCharm") },
        { QStringLiteral("webstorm"),                     QStringLiteral("JetBrains WebStorm") },
        { QStringLiteral("clion"),                       QStringLiteral("JetBrains CLion") },
        { QStringLiteral("goland"),                      QStringLiteral("JetBrains GoLand") },
        { QStringLiteral("rider"),                       QStringLiteral("JetBrains Rider") },
        { QStringLiteral("androidstudio"),                QStringLiteral("Android Studio") },
        { QStringLiteral("steam"),                        QStringLiteral("Steam") },
        { QStringLiteral("steamapps"),                   QStringLiteral("Steam games") },
        { QStringLiteral("epic games"),                   QStringLiteral("Epic Games") },
        { QStringLiteral("blizzard"),                    QStringLiteral("Blizzard") },
        { QStringLiteral("discord"),                      QStringLiteral("Discord") },
        { QStringLiteral("slack"),                        QStringLiteral("Slack") },
        { QStringLiteral("spotify"),                     QStringLiteral("Spotify") },
        { QStringLiteral("telegram"),                    QStringLiteral("Telegram") },
        { QStringLiteral("wechat"),                       QStringLiteral("WeChat") },
        { QStringLiteral("qq"),                           QStringLiteral("QQ") },
        { QStringLiteral("dingtalk"),                    QStringLiteral("DingTalk") },
        { QStringLiteral("feishu"),                      QStringLiteral("Feishu / Lark") },
        { QStringLiteral("wps office"),                  QStringLiteral("WPS Office") },
        { QStringLiteral("neteasemusic"),                 QStringLiteral("NetEase Music") },
        { QStringLiteral("qqmusic"),                      QStringLiteral("QQ Music") },
        { QStringLiteral("nodejs"),                        QStringLiteral("Node.js") },
        { QStringLiteral("nvidia corporation"),           QStringLiteral("NVIDIA") },
        { QStringLiteral("amd"),                          QStringLiteral("AMD") },
        { QStringLiteral("intel"),                        QStringLiteral("Intel") },
        { QStringLiteral("oracle"),                       QStringLiteral("Oracle") },
        { QStringLiteral("java"),                         QStringLiteral("Java") },
        { QStringLiteral("jdk"),                          QStringLiteral("Java Development Kit") },
        { QStringLiteral("docker"),                       QStringLiteral("Docker") },
        { QStringLiteral("vmware"),                       QStringLiteral("VMware") },
        { QStringLiteral("virtualbox"),                   QStringLiteral("VirtualBox") },
        { QStringLiteral("github desktop"),              QStringLiteral("GitHub Desktop") },
        { QStringLiteral("git"),                          QStringLiteral("Git") },
        { QStringLiteral("notepad++"),                    QStringLiteral("Notepad++") },
        { QStringLiteral("sublime text"),                 QStringLiteral("Sublime Text") },
        { QStringLiteral("obs studio"),                   QStringLiteral("OBS Studio") },
        { QStringLiteral("audacity"),                    QStringLiteral("Audacity") },
        { QStringLiteral("blender"),                      QStringLiteral("Blender") },
        { QStringLiteral("inkscape"),                     QStringLiteral("Inkscape") },
        { QStringLiteral("gimp"),                          QStringLiteral("GIMP") },
        { QStringLiteral("7-zip"),                        QStringLiteral("7-Zip") },
        { QStringLiteral("winrar"),                       QStringLiteral("WinRAR") },
        { QStringLiteral("bandizip"),                     QStringLiteral("Bandizip") },
        { QStringLiteral("everything"),                   QStringLiteral("Everything Search") },
        { QStringLiteral("utools"),                      QStringLiteral("uTools") },
        { QStringLiteral("vscode"),                       QStringLiteral("Microsoft VS Code") },
        { QStringLiteral("code"),                          QStringLiteral("Microsoft VS Code") },
        { QStringLiteral("wechat files"),                QStringLiteral("WeChat Files") },
    };
    return m;
}

QString norm(const QString& name) {
    return name.trimmed().toLower();
}

// Walk up ``depth`` parents and return that ancestor's name (or "").
QString parentSegment(const std::shared_ptr<FileNode>& node, int depth) {
    std::shared_ptr<FileNode> cur = node;
    for (int i = 0; i < depth; ++i) {
        if (!cur)
            return {};
        cur = cur->parent.lock();
        if (!cur)
            return {};
    }
    return cur ? cur->name : QString();
}

bool isHome(const QString& path) {
    const QString home = QDir::homePath();
    // Canonical comparison first (resolves symlinks / case).
    const QString cp = QDir(path).canonicalPath();
    const QString ch = QDir(home).canonicalPath();
    if (!cp.isEmpty() && !ch.isEmpty())
        return cp.compare(ch, Qt::CaseInsensitive) == 0;
    // Fallback: compare normalized, trailing-slash-stripped paths.
    auto strip = [](QString s) {
        s = s.trimmed().toLower();
        while (s.endsWith('/') || s.endsWith('\\'))
            s.chop(1);
        return s;
    };
    return strip(path) == strip(home);
}

std::optional<RuleResult> ruleSystemPath(const std::shared_ptr<FileNode>& node) {
    QString p = node->path.toLower();
    p.replace('/', '\\');
    const QString nameL = norm(node->name);

    if (nameL == QStringLiteral("windows") || p.endsWith(QStringLiteral("\\windows")))
        return RuleResult{ QStringLiteral("identify.system_windows"), {} };
    if (p.contains(QStringLiteral("$recycle.bin")) || nameL == QStringLiteral("$recycle.bin"))
        return RuleResult{ QStringLiteral("identify.recycle"), {} };
    if (p.contains(QStringLiteral("\\program files")) ||
        p.contains(QStringLiteral("\\program files (x86)"))) {
        // Try to name the actual product one level deeper.
        const QString seg = parentSegment(node, 1);
        if (!seg.isEmpty()) {
            auto it = softwareNames().constFind(norm(seg));
            if (it != softwareNames().constEnd()) {
                return RuleResult{
                    QStringLiteral("identify.software_prefix"),
                    { { QStringLiteral("name"), it.value() + QStringLiteral(" (Program Files)") } }
                };
            }
        }
        return RuleResult{ QStringLiteral("identify.program_files"), {} };
    }
    if (p.contains(QStringLiteral("\\appdata")))
        return RuleResult{ QStringLiteral("identify.appdata"), {} };

    if (nameL == QStringLiteral("downloads")) {
        const auto par = node->parent.lock();
        if (par) {
            const QString homeBase = QFileInfo(QDir::homePath()).fileName().toLower();
            const QString parName = norm(par->name);
            if (parName == QStringLiteral("home") || parName == homeBase)
                return RuleResult{ QStringLiteral("identify.downloads"), {} };
        }
    }
    if (nameL == QStringLiteral("temp") || nameL == QStringLiteral("tmp") ||
        p.contains(QStringLiteral("\\temp")) ||
        p.contains(QStringLiteral("\\appdata\\local\\temp")))
        return RuleResult{ QStringLiteral("identify.temp"), {} };
    if (nameL == QStringLiteral("logs") || nameL == QStringLiteral("log") ||
        p.endsWith(QStringLiteral("\\logs")))
        return RuleResult{ QStringLiteral("identify.logs"), {} };
    if (nameL == QStringLiteral("cache") || nameL == QStringLiteral("caches") ||
        nameL == QStringLiteral("cache2"))
        return RuleResult{ QStringLiteral("identify.cache"), {} };

    // Root of a drive / no parent.
    if (!node->parent.lock())
        return isHome(node->path) ? std::optional<RuleResult>(
                   RuleResult{ QStringLiteral("identify.user_home"), {} })
                                 : std::nullopt;
    return std::nullopt;
}

std::optional<RuleResult> ruleSoftwareName(const std::shared_ptr<FileNode>& node) {
    const QString nameL = norm(node->name);
    const auto& names = softwareNames();

    // Exact match.
    auto exact = names.constFind(nameL);
    if (exact != names.constEnd())
        return RuleResult{ QStringLiteral("identify.software_prefix"),
                           { { QStringLiteral("name"), exact.value() } } };

    // Loose prefix match: pick the longest matching key so that
    // "adobe photoshop 2024" matches "adobe photoshop" rather than "adobe".
    QString bestKey;
    QString bestProd;
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        const QString& key = it.key();
        if (nameL == key || nameL.startsWith(key + QStringLiteral(" "))) {
            if (bestKey.isEmpty() || key.size() > bestKey.size()) {
                bestKey = key;
                bestProd = it.value();
            }
        }
    }
    if (!bestProd.isEmpty())
        return RuleResult{ QStringLiteral("identify.software_prefix"),
                           { { QStringLiteral("name"), bestProd } } };
    return std::nullopt;
}

std::optional<RuleResult> ruleProjectMarkers(const std::shared_ptr<FileNode>& node) {
    if (!node->isDir())
        return std::nullopt;
    const QString nameL = norm(node->name);

    if (nameL == QStringLiteral("node_modules"))
        return RuleResult{ QStringLiteral("identify.node_modules"), {} };
    if (nameL == QStringLiteral(".git"))
        return RuleResult{ QStringLiteral("identify.git_repo"), {} };
    if (nameL == QStringLiteral("__pycache__") ||
        nameL == QStringLiteral(".pytest_cache") ||
        nameL == QStringLiteral(".mypy_cache") ||
        nameL == QStringLiteral(".ruff_cache"))
        return RuleResult{ QStringLiteral("identify.cache"), {} };
    if (nameL == QStringLiteral("build") || nameL == QStringLiteral("dist") ||
        nameL == QStringLiteral("out") || nameL == QStringLiteral("target") ||
        nameL == QStringLiteral("bin") || nameL == QStringLiteral("release"))
        return RuleResult{ QStringLiteral("identify.build_output"), {} };

    // Marker files: cheap existence scan (no contents read).
    const QDir dir(node->path);
    const QStringList entries =
        dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    QSet<QString> lower;
    lower.reserve(entries.size());
    for (const QString& e : entries)
        lower.insert(e.toLower());

    if (lower.contains(QStringLiteral(".git")))
        return RuleResult{ QStringLiteral("identify.git_repo"), {} };
    if (lower.contains(QStringLiteral("package.json")))
        return RuleResult{ QStringLiteral("identify.software_prefix"),
                           { { QStringLiteral("name"),
                               QStringLiteral("Node.js / JavaScript project") } } };
    if (lower.contains(QStringLiteral("pyvenv.cfg")) ||
        lower.contains(QStringLiteral("venv")) ||
        lower.contains(QStringLiteral(".venv")))
        return RuleResult{ QStringLiteral("identify.python_venv"), {} };
    if (lower.contains(QStringLiteral("setup.py")) ||
        lower.contains(QStringLiteral("setup.cfg")) ||
        lower.contains(QStringLiteral("pyproject.toml")))
        return RuleResult{ QStringLiteral("identify.python_pkg"), {} };
    for (const QString& e : lower) {
        if (e.endsWith(QStringLiteral(".log")))
            return RuleResult{ QStringLiteral("identify.logs"), {} };
    }
    return std::nullopt;
}

std::optional<RuleResult> identify(const std::shared_ptr<FileNode>& node) {
    if (!node)
        return std::nullopt;
    if (node->nodeType == NodeType::Symlink)
        return std::nullopt;

    // Ordered rule chain. First match wins.
    if (auto r = ruleSystemPath(node))      return r;
    if (auto r = ruleProjectMarkers(node))  return r;
    if (auto r = ruleSoftwareName(node))    return r;
    return std::nullopt;
}

}  // namespace

QString describe(const std::shared_ptr<FileNode>& node,
                 const std::function<QString(const QString&)>& tr) {
    const auto result = identify(node);
    if (!result)
        return {};

    // Translate the key (language fallback handled by the callback), then
    // interpolate any {placeholder} parameters the rule produced.
    QString s = tr ? tr(result->key) : result->key;
    for (auto it = result->params.constBegin(); it != result->params.constEnd(); ++it) {
        s.replace(QStringLiteral("{") + it.key() + QStringLiteral("}"), it.value());
    }
    return s;
}

}  // namespace Identify
