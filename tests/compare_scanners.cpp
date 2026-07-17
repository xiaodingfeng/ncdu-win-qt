// compare_scanners.cpp
//
// CLI diagnostic tool: runs both DiskScanner (Win32 FindFirstFile traversal)
// and MftScanner (NTFS MFT direct read) on the same path, then reports files
// whose sizes differ between the two. Especially useful for catching files
// that MFT scanning reports as 0 bytes.
//
// Usage:
//   compare_scanners.exe [path]
//   compare_scanners.exe C:/Users/Administrator
//
// Output:
//   - Console: summary statistics + sample mismatches
//   - scan_diff.csv (next to exe): full mismatch list as CSV

#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QSet>
#include <QMap>
#include <QStringList>
#include <memory>

#include "DiskScanner.h"
#include "MftScanner.h"
#include "FileNode.h"

namespace {

struct ScanResult {
    std::shared_ptr<FileNode> root;
    QString error;
    qint64 elapsedMs = 0;
};

// Flatten the FileNode tree into a path -> size map (files only).
// Directory nodes are skipped because their sizes are aggregated and may
// legitimately differ (skip rules, etc.). File-level sizes are the real test.
void flattenFiles(const std::shared_ptr<FileNode>& node,
                  QHash<QString, qint64>& out)
{
    if (!node) return;
    if (node->nodeType == NodeType::File && !node->path.isEmpty()) {
        out.insert(node->path, node->size);
    }
    for (const auto& child : node->children) {
        flattenFiles(child, out);
    }
}

// Normalize a path for cross-scanner comparison:
// - lowercase drive letter portion? No, keep case but compare case-insensitively
// - forward slashes
// - strip trailing slash
QString normalizeKey(const QString& p)
{
    QString s = p;
    s.replace('\\', '/');
    while (s.endsWith('/'))
        s.chop(1);
    return s;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QString path = (argc >= 2) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("C:/");
    QTextStream out(stdout);
    out << "=== compare_scanners ===" << "\n";
    out << "Target path: " << path << "\n\n";

    // ---- Run DiskScanner ----
    out << "[1/2] Running DiskScanner (Win32 traversal)..." << "\n";
    out.flush();
    DiskScanner diskScanner(path);
    diskScanner.setSkipHeavyDirs(false);

    ScanResult diskResult;
    {
        QEventLoop loop;
        QObject::connect(&diskScanner, &DiskScanner::finishedTree,
                         [&](std::shared_ptr<FileNode> root) {
                             diskResult.root = root;
                             loop.quit();
                         });
        QObject::connect(&diskScanner, &DiskScanner::error,
                         [&](const QString& key, const QMap<QString, QString>& args) {
                             // Render the i18n key + args into a readable string.
                             // For a CLI tool we just format "key{args}".
                             QString rendered = key;
                             if (!args.isEmpty()) {
                                 QStringList parts;
                                 for (auto it = args.begin(); it != args.end(); ++it)
                                     parts << it.key() + "=" + it.value();
                                 rendered += " {" + parts.join(", ") + "}";
                             }
                             diskResult.error = rendered;
                             loop.quit();
                         });
        QElapsedTimer t; t.start();
        diskScanner.start();
        loop.exec();
        diskScanner.wait();  // ensure QThread fully stops before QEventLoop is destroyed
        diskResult.elapsedMs = t.elapsed();
    }

    if (!diskResult.error.isEmpty()) {
        out << "DiskScanner ERROR: " << diskResult.error << "\n";
    }
    if (!diskResult.root) {
        out << "DiskScanner returned no tree, aborting.\n";
        return 1;
    }
    out << "DiskScanner done in " << diskResult.elapsedMs << " ms"
        << ", root size=" << diskResult.root->size << "\n";

    // ---- Run MftScanner ----
    out << "\n[2/2] Running MftScanner (NTFS MFT direct read)..." << "\n";
    out.flush();
    MftScanner mftScanner(path);
    mftScanner.setSkipHeavyDirs(false);

    ScanResult mftResult;
    {
        QEventLoop loop;
        QObject::connect(&mftScanner, &MftScanner::finishedTree,
                         [&](std::shared_ptr<FileNode> root) {
                             mftResult.root = root;
                             loop.quit();
                         });
        QObject::connect(&mftScanner, &MftScanner::error,
                         [&](const QString& key, const QMap<QString, QString>& args) {
                             QString rendered = key;
                             if (!args.isEmpty()) {
                                 QStringList parts;
                                 for (auto it = args.begin(); it != args.end(); ++it)
                                     parts << it.key() + "=" + it.value();
                                 rendered += " {" + parts.join(", ") + "}";
                             }
                             mftResult.error = rendered;
                             loop.quit();
                         });
        QElapsedTimer t; t.start();
        mftScanner.start();
        loop.exec();
        mftScanner.wait();  // ensure QThread fully stops before QEventLoop is destroyed
        mftResult.elapsedMs = t.elapsed();
    }

    if (!mftResult.error.isEmpty()) {
        out << "MftScanner ERROR: " << mftResult.error << "\n";
    }
    if (!mftResult.root) {
        out << "MftScanner returned no tree, aborting.\n";
        return 1;
    }
    out << "MftScanner done in " << mftResult.elapsedMs << " ms"
        << ", root size=" << mftResult.root->size << "\n";

    // ---- Build path -> size maps ----
    QHash<QString, qint64> diskMap, mftMap;
    flattenFiles(diskResult.root, diskMap);
    flattenFiles(mftResult.root, mftMap);

    out << "\nDiskScanner files: " << diskMap.size() << "\n";
    out << "MftScanner files:  " << mftMap.size() << "\n";

    // ---- Compare ----
    // Build a normalized key set: lowercased + slash-normalized path.
    // NTFS is case-insensitive, so compare paths case-insensitively.
    QHash<QString, qint64> diskNorm, mftNorm;
    QSet<QString> allKeys;
    for (auto it = diskMap.begin(); it != diskMap.end(); ++it) {
        QString k = normalizeKey(it.key()).toLower();
        diskNorm.insert(k, it.value());
        allKeys.insert(k);
    }
    for (auto it = mftMap.begin(); it != mftMap.end(); ++it) {
        QString k = normalizeKey(it.key()).toLower();
        mftNorm.insert(k, it.value());
        allKeys.insert(k);
    }

    qint64 onlyDisk = 0;
    qint64 onlyMft = 0;
    qint64 sizeMismatch = 0;
    qint64 zeroInMftNonZeroInDisk = 0;  // the most important category
    qint64 equalCount = 0;

    // For CSV output
    QString csvPath = QCoreApplication::applicationDirPath() + "/scan_diff.csv";
    QFile csvFile(csvPath);
    if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        out << "ERROR: cannot write CSV to " << csvPath << "\n";
        return 1;
    }
    QTextStream csvStream(&csvFile);
    csvStream << "path,disk_size,mft_size,diff,category\n";

    // Collect a small sample for console output
    struct Mismatch {
        QString path;
        qint64 diskSize;
        qint64 mftSize;
        QString category;
    };
    std::vector<Mismatch> samples;
    const int MAX_SAMPLES = 50;

    for (const QString& key : allKeys) {
        bool inDisk = diskNorm.contains(key);
        bool inMft = mftNorm.contains(key);
        qint64 dSize = inDisk ? diskNorm.value(key) : -1;
        qint64 mSize = inMft ? mftNorm.value(key) : -1;

        if (inDisk && !inMft) {
            onlyDisk++;
            csvStream << key << "," << dSize << ",-1," << "only_disk\n";
            continue;
        }
        if (inMft && !inDisk) {
            onlyMft++;
            csvStream << key << ",-1," << mSize << ",only_mft\n";
            continue;
        }
        // both present
        if (dSize == mSize) {
            equalCount++;
            continue;
        }
        sizeMismatch++;
        qint64 diff = mSize - dSize;
        QString category;
        if (mSize == 0 && dSize > 0) {
            category = "mft_zero_disk_nonzero";
            zeroInMftNonZeroInDisk++;
        } else if (mSize > 0 && dSize == 0) {
            category = "disk_zero_mft_nonzero";
        } else {
            category = "size_mismatch";
        }
        csvStream << key << "," << dSize << "," << mSize << "," << diff << "," << category << "\n";

        if ((int)samples.size() < MAX_SAMPLES && (category == "mft_zero_disk_nonzero" || category == "size_mismatch")) {
            samples.push_back({key, dSize, mSize, category});
        }
    }
    csvStream.flush();
    csvFile.close();

    // ---- Summary ----
    out << "\n=== Comparison Summary ===\n";
    out << "  Files only in DiskScanner:  " << onlyDisk << "\n";
    out << "  Files only in MftScanner:   " << onlyMft << "\n";
    out << "  Files in both, size equal:  " << equalCount << "\n";
    out << "  Files in both, size DIFF:   " << sizeMismatch << "\n";
    out << "    -> MFT=0 but Disk>0:      " << zeroInMftNonZeroInDisk << "  [main bug]\n";
    out << "\nFull CSV written to: " << csvPath << "\n";

    if (!samples.empty()) {
        out << "\n=== Sample mismatches (max " << MAX_SAMPLES << ") ===\n";
        out << "  DISK_SIZE      MFT_SIZE      PATH\n";
        for (const auto& m : samples) {
            out << "  " << QString::number(m.diskSize).rightJustified(12)
                << "  " << QString::number(m.mftSize).rightJustified(12)
                << "  " << m.path
                << "  [" << m.category << "]\n";
        }
    }

    out << "\nDone.\n";
    return 0;
}
