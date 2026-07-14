// Unit tests for FileNode, sortBySizeDesc, and FormatHelpers.
//
// Uses the Qt Test framework. Run with:  ctest --output-on-failure
// Or directly:  ./test_scanner

#include <QtTest/QtTest>

#include "FileNode.h"
#include "FormatHelpers.h"

class TestScanner : public QObject {
    Q_OBJECT

private slots:
    // ---- FileNode field initialization ----
    void testFileNodeDefaultFields()
    {
        auto node = std::make_shared<FileNode>();
        QCOMPARE(node->name, QString());
        QCOMPARE(node->path, QString());
        QCOMPARE(node->nodeType, NodeType::File);
        QCOMPARE(node->size, qint64(0));
        QCOMPARE(node->fileCount, 0);
        QCOMPARE(node->dirCount, 0);
        QCOMPARE(node->isHidden, false);
        QCOMPARE(node->skipped, false);
        QVERIFY(node->error.isEmpty());
        QVERIFY(node->children.empty());
        QVERIFY(node->parent.expired());
    }

    void testIsDirForDirectory()
    {
        auto node = std::make_shared<FileNode>();
        node->nodeType = NodeType::Directory;
        QVERIFY(node->isDir());
    }

    void testIsDirForDrive()
    {
        auto node = std::make_shared<FileNode>();
        node->nodeType = NodeType::Drive;
        QVERIFY(node->isDir());
    }

    void testIsDirForFile()
    {
        auto node = std::make_shared<FileNode>();
        node->nodeType = NodeType::File;
        QVERIFY(!node->isDir());
    }

    void testIsDirForSymlink()
    {
        auto node = std::make_shared<FileNode>();
        node->nodeType = NodeType::Symlink;
        QVERIFY(!node->isDir());
    }

    void testTotalItems()
    {
        auto node = std::make_shared<FileNode>();
        node->fileCount = 3;
        node->dirCount = 2;
        QCOMPARE(node->totalItems(), 5);
    }

    // ---- sortBySizeDesc ----
    void testSortsBySizeDescending()
    {
        auto root = std::make_shared<FileNode>();
        root->nodeType = NodeType::Directory;

        auto a = std::make_shared<FileNode>();
        a->name = "a"; a->size = 10;

        auto b = std::make_shared<FileNode>();
        b->name = "b"; b->size = 100;

        auto c = std::make_shared<FileNode>();
        c->name = "c"; c->size = 50;

        root->children = {a, b, c};
        root->sortBySizeDesc();

        QCOMPARE(root->children.size(), size_t(3));
        QCOMPARE(root->children[0]->name, QString("b"));  // 100
        QCOMPARE(root->children[1]->name, QString("c"));  // 50
        QCOMPARE(root->children[2]->name, QString("a"));  // 10
    }

    void testTieBreakByNameLowercase()
    {
        auto root = std::make_shared<FileNode>();
        root->nodeType = NodeType::Directory;

        // Equal sizes -> sorted by name.toLower()
        auto B = std::make_shared<FileNode>();
        B->name = "B"; B->size = 50;

        auto a = std::make_shared<FileNode>();
        a->name = "a"; a->size = 50;

        auto C = std::make_shared<FileNode>();
        C->name = "C"; C->size = 50;

        root->children = {B, a, C};
        root->sortBySizeDesc();

        // All same size -> alphabetical by lowercase: a, B, C
        QCOMPARE(root->children[0]->name, QString("a"));
        QCOMPARE(root->children[1]->name, QString("B"));
        QCOMPARE(root->children[2]->name, QString("C"));
    }

    void testRecursesIntoSubdirs()
    {
        auto root = std::make_shared<FileNode>();
        root->nodeType = NodeType::Directory;

        auto sub = std::make_shared<FileNode>();
        sub->name = "sub"; sub->nodeType = NodeType::Directory;

        auto x = std::make_shared<FileNode>();
        x->name = "x"; x->size = 10;

        auto y = std::make_shared<FileNode>();
        y->name = "y"; y->size = 200;

        sub->children = {x, y};
        root->children = {sub};
        root->sortBySizeDesc();

        // Subdir children should also be sorted.
        QCOMPARE(sub->children[0]->name, QString("y"));  // 200
        QCOMPARE(sub->children[1]->name, QString("x"));  // 10
    }

    void testDoesNotRecurseIntoFiles()
    {
        auto root = std::make_shared<FileNode>();
        root->nodeType = NodeType::Directory;

        auto file = std::make_shared<FileNode>();
        file->name = "f.txt"; file->nodeType = NodeType::File; file->size = 5;

        // Children of a file node should not be touched by sort.
        auto inner = std::make_shared<FileNode>();
        inner->name = "inner"; inner->size = 999;
        file->children = {inner};

        root->children = {file};
        root->sortBySizeDesc();

        QCOMPARE(file->children.size(), size_t(1));
        QCOMPARE(file->children[0]->name, QString("inner"));
    }

    // ---- humanSize ----
    void testHumanSizeBytes()
    {
        QCOMPARE(humanSize(0), QStringLiteral("0 B"));
        QCOMPARE(humanSize(512), QStringLiteral("512 B"));
        QCOMPARE(humanSize(1023), QStringLiteral("1023 B"));
    }

    void testHumanSizeKiB()
    {
        QCOMPARE(humanSize(1024), QStringLiteral("1.0 KiB"));
        QCOMPARE(humanSize(1536), QStringLiteral("1.5 KiB"));
    }

    void testHumanSizeMiB()
    {
        QCOMPARE(humanSize(1024 * 1024), QStringLiteral("1.0 MiB"));
    }

    void testHumanSizeGiB()
    {
        const qint64 one_gib = static_cast<qint64>(1024) * 1024 * 1024;
        QCOMPARE(humanSize(one_gib), QStringLiteral("1.0 GiB"));
    }

    void testHumanSizeNegative()
    {
        QCOMPARE(humanSize(-1024), QStringLiteral("-1.0 KiB"));
    }

    // ---- humanCount ----
    void testHumanCountSmall()
    {
        QCOMPARE(humanCount(0), QStringLiteral("0"));
        QCOMPARE(humanCount(999), QStringLiteral("999"));
    }

    void testHumanCountThousands()
    {
        QCOMPARE(humanCount(1500), QStringLiteral("1.5k"));
    }

    void testHumanCountMillions()
    {
        QCOMPARE(humanCount(2500000), QStringLiteral("2.5M"));
    }
};

QTEST_MAIN(TestScanner)
#include "test_scanner.moc"
