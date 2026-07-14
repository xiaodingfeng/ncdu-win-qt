#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QCursor>
#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QAction>
#include <QActionGroup>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>
#include <QFileInfo>
#include <QDir>
#include <QVariant>
#include <QRegularExpression>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPen>
#include <QRectF>
#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>

#include <algorithm>
#include <set>

#include "Style.h"
#include "I18n.h"
#include "Identify.h"
#include "WinApi.h"
#include "FormatHelpers.h"
#include "SizeBarDelegate.h"
#include "TreemapWidget.h"
#include "BreadcrumbBar.h"
#include "LegendBar.h"
#include "CleanupPanel.h"
#include "version.h"

Q_DECLARE_METATYPE(std::shared_ptr<FileNode>)
Q_DECLARE_METATYPE(CleanupTarget)
Q_DECLARE_METATYPE(LargeFile)
Q_DECLARE_METATYPE(std::vector<CleanupTarget>)
Q_DECLARE_METATYPE(std::vector<LargeFile>)
Q_DECLARE_METATYPE(std::vector<CleanupWorker::ItemRef>)

// ---- Column sort configuration --------------------------------------------
struct SortColInfo { const char* key; Qt::SortOrder defaultOrder; };
static const SortColInfo SORT_COLUMNS[] = {
    {"name",  Qt::AscendingOrder},   // 0
    {"size",  Qt::DescendingOrder},  // 1
    {"size",  Qt::DescendingOrder},  // 2
    {"size",  Qt::DescendingOrder},  // 3
    {"items", Qt::DescendingOrder},  // 4
    {"type",  Qt::AscendingOrder},   // 5
};
static constexpr int SORT_COLUMNS_COUNT = 6;

static QString sortKeyForColumn(int col) {
    if (col >= 0 && col < SORT_COLUMNS_COUNT)
        return QString::fromLatin1(SORT_COLUMNS[col].key);
    return QStringLiteral("size");
}

static Qt::SortOrder defaultSortOrderForColumn(int col) {
    if (col >= 0 && col < SORT_COLUMNS_COUNT)
        return SORT_COLUMNS[col].defaultOrder;
    return Qt::DescendingOrder;
}

// ---- Number formatting helper ---------------------------------------------
static QString withCommas(qint64 n) {
    QString s = QString::number(n);
    for (int pos = s.length() - 3; pos > 0; pos -= 3)
        s.insert(pos, ',');
    return s;
}

// ---- Icon helpers (draw simple colored glyphs, no image assets) -----------
static QIcon makeTypeIcon(const QString& colorHex, bool isDir) {
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(QColor(colorHex));
    p.setPen(Qt::NoPen);
    if (isDir) {
        p.drawRoundedRect(1, 3, 16, 13, 3, 3);
        p.drawRoundedRect(1, 1, 7, 4, 2, 2);
    } else {
        p.drawRoundedRect(3, 3, 12, 12, 3, 3);
    }
    p.end();
    return QIcon(pm);
}

static QIcon typeIcon(const std::shared_ptr<FileNode>& node) {
    QString ext;
    int dot = node->name.lastIndexOf('.');
    if (dot >= 0)
        ext = node->name.mid(dot).toLower();

    QString cacheKey = QStringLiteral("%1|%2|%3|%4")
        .arg(static_cast<int>(node->nodeType))
        .arg(ext)
        .arg(node->isHidden)
        .arg(node->isDir());
    static QMap<QString, QIcon> cache;
    auto it = cache.find(cacheKey);
    if (it != cache.end())
        return it.value();

    QString color = typeColor(node->name, node->isDir(), node->isHidden);
    QIcon icon = makeTypeIcon(color, node->isDir());
    cache[cacheKey] = icon;
    return icon;
}
// --------------------------------------------------------------------------- //
// Constructor / destructor
// --------------------------------------------------------------------------- //
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName("root");
    setWindowTitle(I18n::tr("app.title"));
    resize(1200, 780);
    setMinimumSize(860, 560);
    // Register metatypes for cross-thread queued connections.
    qRegisterMetaType<std::shared_ptr<FileNode>>("std::shared_ptr<FileNode>");
    qRegisterMetaType<CleanupTarget>("CleanupTarget");
    qRegisterMetaType<LargeFile>("LargeFile");
    qRegisterMetaType<std::vector<CleanupTarget>>("std::vector<CleanupTarget>");
    qRegisterMetaType<std::vector<LargeFile>>("std::vector<LargeFile>");
    qRegisterMetaType<std::vector<CleanupWorker::ItemRef>>("std::vector<CleanupWorker::ItemRef>");

    buildUI();
    buildMenu();
    wireSignals();
    populatePathCombo();
    reflectSortIndicator();

    // Auto-scan home dir on first show for instant value.
    QTimer::singleShot(120, this, [this]() {
        QString path = m_pathCombo->currentText().trimmed();
        if (path.isEmpty())
            path = getHomeDir();
        startScan(path);
    });
}

MainWindow::~MainWindow()
{
    // Threads are cancelled and waited in closeEvent; nothing extra here.
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_scanner) {
        m_scanner->cancel();
        m_scanner->wait(3000);
    }
    if (m_cleanupScanner) {
        m_cleanupScanner->cancel();
        m_cleanupScanner->wait(3000);
    }
    if (m_cleanupWorker) {
        m_cleanupWorker->cancel();
        m_cleanupWorker->wait(3000);
    }
    QMainWindow::closeEvent(e);
}

// --------------------------------------------------------------------------- //
// UI construction
// --------------------------------------------------------------------------- //
void MainWindow::buildUI()
{
    auto* central = new QWidget(this);
    central->setObjectName("root");
    setCentralWidget(central);

    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Top bar ---
    {
        auto* bar = new QFrame;
        bar->setObjectName("topbar");
        bar->setFixedHeight(56);
        auto* lay = new QHBoxLayout(bar);
        lay->setContentsMargins(18, 8, 18, 8);
        lay->setSpacing(12);

        auto* logo = new QLabel(QStringLiteral("\u25CD"));  // ◍
        logo->setStyleSheet(QStringLiteral("color: %1; font-size: 22px; font-weight: 700;")
                                .arg(QString::fromLatin1(C::PRIMARY)));
        auto* title = new QLabel(APP_NAME);
        title->setObjectName("title");
        auto* sub = new QLabel(I18n::tr("app.subtitle"));
        sub->setObjectName("subtitle");
        m_subtitleLabel = sub;

        auto* titleCol = new QVBoxLayout;
        titleCol->setSpacing(0);
        titleCol->addWidget(title);
        titleCol->addWidget(sub);

        lay->addWidget(logo);
        lay->addLayout(titleCol);
        lay->addSpacing(16);

        m_pathCombo = new QComboBox;
        m_pathCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_pathCombo->setEditable(true);
        lay->addWidget(m_pathCombo, 1);

        m_browseBtn = new QPushButton(I18n::tr("button.browse"));
        m_browseBtn->setCursor(Qt::PointingHandCursor);
        lay->addWidget(m_browseBtn);

        m_skipCheckbox = new QCheckBox(I18n::tr("button.skip_heavy"));
        m_skipCheckbox->setChecked(m_skipHeavyDirs);
        m_skipCheckbox->setCursor(Qt::PointingHandCursor);
        m_skipCheckbox->setToolTip(I18n::tr("tooltip.skip_heavy"));
        lay->addWidget(m_skipCheckbox);

        m_scanBtn = new QPushButton(I18n::tr("button.scan"));
        m_scanBtn->setObjectName("primary");
        m_scanBtn->setCursor(Qt::PointingHandCursor);
        m_scanBtn->setMinimumWidth(96);
        lay->addWidget(m_scanBtn);

        m_cancelBtn = new QPushButton(I18n::tr("button.cancel"));
        m_cancelBtn->setObjectName("danger");
        m_cancelBtn->setCursor(Qt::PointingHandCursor);
        m_cancelBtn->setVisible(false);
        lay->addWidget(m_cancelBtn);

        root->addWidget(bar);
    }

    // --- Toolbar (breadcrumb + search) ---
    {
        auto* bar = new QFrame;
        bar->setObjectName("breadcrumb");
        bar->setFixedHeight(44);
        auto* lay = new QHBoxLayout(bar);
        lay->setContentsMargins(18, 4, 18, 4);
        lay->setSpacing(12);

        m_breadcrumb = new BreadcrumbBar;
        lay->addWidget(m_breadcrumb, 1);

        m_searchBox = new QLineEdit;
        m_searchBox->setPlaceholderText(I18n::tr("search.placeholder"));
        m_searchBox->setMinimumWidth(200);
        m_searchBox->setMaximumWidth(300);
        lay->addWidget(m_searchBox);

        m_upBtn = new QPushButton(I18n::tr("button.up"));
        m_upBtn->setObjectName("ghost");
        m_upBtn->setCursor(Qt::PointingHandCursor);
        m_upBtn->setToolTip(I18n::tr("menu.view.up"));
        lay->addWidget(m_upBtn);

        m_refreshBtn = new QPushButton(I18n::tr("button.refresh"));
        m_refreshBtn->setObjectName("ghost");
        m_refreshBtn->setCursor(Qt::PointingHandCursor);
        m_refreshBtn->setToolTip(I18n::tr("menu.view.refresh"));
        m_refreshBtn->setFixedWidth(36);
        lay->addWidget(m_refreshBtn);

        root->addWidget(bar);
    }

    // --- Splitter: file list (left) + tabbed right panel ---
    {
        auto* splitter = new QSplitter(Qt::Horizontal);
        splitter->setHandleWidth(8);
        splitter->setChildrenCollapsible(false);

        // File list
        m_tree = new QTreeWidget;
        m_tree->setObjectName("filelist");
        m_tree->setRootIsDecorated(false);
        m_tree->setUniformRowHeights(true);
        m_tree->setAlternatingRowColors(false);
        m_tree->setSelectionMode(QTreeWidget::ExtendedSelection);
        m_tree->setSortingEnabled(false);
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        m_tree->setFocus();
        setHeaderLabels();

        auto* header = m_tree->header();
        header->setSectionResizeMode(0, QHeaderView::Interactive);
        header->resizeSection(0, 180);
        header->setSectionResizeMode(1, QHeaderView::Stretch);
        header->resizeSection(1, 140);
        header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        header->resizeSection(2, 90);
        header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        header->resizeSection(3, 70);
        header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        header->resizeSection(4, 80);
        header->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        header->resizeSection(5, 100);
        header->setSectionsClickable(true);
        header->setSortIndicatorShown(true);
        header->setStretchLastSection(false);

        // Custom delegate for the distribution bar column (index 1).
        m_tree->setItemDelegateForColumn(1, new SizeBarDelegate(m_tree));
        splitter->addWidget(m_tree);

        // Right panel: tabbed (Treemap + Cleanup)
        m_rightTabs = new QTabWidget;
        m_rightTabs->setObjectName("rightpanel");

        // Tab 1: Treemap + legend
        auto* treemapTab = new QWidget;
        auto* tlay = new QVBoxLayout(treemapTab);
        tlay->setContentsMargins(0, 0, 0, 0);
        tlay->setSpacing(8);
        m_treemap = new TreemapWidget;
        tlay->addWidget(m_treemap, 1);
        m_legend = new LegendBar;
        auto* legendLay = new QHBoxLayout;
        legendLay->setContentsMargins(12, 0, 12, 8);
        legendLay->addWidget(m_legend);
        auto* legendWrap = new QWidget;
        legendWrap->setLayout(legendLay);
        tlay->addWidget(legendWrap);
        m_rightTabs->addTab(treemapTab, I18n::tr("tab.treemap"));

        // Tab 2: Cleanup panel
        m_cleanupPanel = new CleanupPanel;
        m_rightTabs->addTab(m_cleanupPanel, I18n::tr("tab.cleanup"));

        splitter->addWidget(m_rightTabs);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        splitter->setSizes({680, 480});

        root->addWidget(splitter, 1);
    }

    // --- Status bar ---
    {
        auto* bar = new QFrame;
        bar->setObjectName("statusbar");
        bar->setFixedHeight(30);
        auto* lay = new QHBoxLayout(bar);
        lay->setContentsMargins(18, 4, 18, 4);
        lay->setSpacing(16);

        m_statusLabel = new QLabel(I18n::tr("status.ready"));
        m_statusLabel->setObjectName("status");
        lay->addWidget(m_statusLabel);
        lay->addStretch(1);

        m_progress = new QProgressBar;
        m_progress->setFixedWidth(180);
        m_progress->setRange(0, 0);
        m_progress->setVisible(false);
        lay->addWidget(m_progress);

        m_hoverLabel = new QLabel(QString());
        m_hoverLabel->setObjectName("status");
        m_hoverLabel->setAlignment(Qt::AlignRight);
        m_hoverLabel->setMinimumWidth(220);
        lay->addWidget(m_hoverLabel);

        root->addWidget(bar);
    }
}

void MainWindow::setHeaderLabels()
{
    QStringList labels = {
        I18n::tr("column.name"),
        I18n::tr("column.distribution"),
        I18n::tr("column.size"),
        I18n::tr("column.percent"),
        I18n::tr("column.items"),
        I18n::tr("column.type"),
    };
    m_tree->setColumnCount(labels.size());
    m_tree->setHeaderLabels(labels);
}

// --------------------------------------------------------------------------- //
// Menu bar
// --------------------------------------------------------------------------- //
void MainWindow::buildMenu()
{
    auto* mb = menuBar();

    // --- File ---
    auto* fileMenu = mb->addMenu(I18n::tr("menu.file"));
    m_actions["file.open"] = fileMenu->addAction(I18n::tr("menu.file.open_folder"));
    m_actions["file.open"]->setShortcut(QKeySequence("Ctrl+O"));
    connect(m_actions["file.open"], &QAction::triggered, this, &MainWindow::onBrowse);

    m_actions["file.rescan"] = fileMenu->addAction(I18n::tr("menu.file.rescan"));
    m_actions["file.rescan"]->setShortcut(QKeySequence("F5"));
    connect(m_actions["file.rescan"], &QAction::triggered, this, QOverload<>::of(&MainWindow::refresh));

    fileMenu->addSeparator();

    m_actions["file.quit"] = fileMenu->addAction(I18n::tr("menu.file.quit"));
    m_actions["file.quit"]->setShortcut(QKeySequence("Ctrl+Q"));
    connect(m_actions["file.quit"], &QAction::triggered, this, &MainWindow::close);

    // --- View ---
    auto* viewMenu = mb->addMenu(I18n::tr("menu.view"));
    m_actions["view.up"] = viewMenu->addAction(I18n::tr("menu.view.up"));
    m_actions["view.up"]->setShortcut(QKeySequence("Backspace"));
    connect(m_actions["view.up"], &QAction::triggered, this, QOverload<>::of(&MainWindow::goUp));

    m_actions["view.refresh"] = viewMenu->addAction(I18n::tr("menu.view.refresh"));
    m_actions["view.refresh"]->setShortcut(QKeySequence("F5"));
    connect(m_actions["view.refresh"], &QAction::triggered, this, QOverload<>::of(&MainWindow::refresh));

    viewMenu->addSeparator();

    m_actions["view.show_files"] = viewMenu->addAction(I18n::tr("menu.view.show_files"));
    m_actions["view.show_files"]->setCheckable(true);
    m_actions["view.show_files"]->setChecked(m_showFiles);
    connect(m_actions["view.show_files"], &QAction::toggled, this, &MainWindow::onShowFilesToggled);

    // --- Language ---
    auto* langMenu = mb->addMenu(I18n::tr("menu.language"));
    auto* langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);
    for (const auto& code : I18n::availableLanguages()) {
        auto* act = langMenu->addAction(I18n::languageDisplayName(code));
        act->setCheckable(true);
        act->setChecked(code == I18n::currentLanguage());
        langGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, code]() { switchLanguage(code); });
    }

    // --- Help ---
    auto* helpMenu = mb->addMenu(I18n::tr("menu.help"));
    m_actions["help.about"] = helpMenu->addAction(I18n::tr("menu.help.about"));
    connect(m_actions["help.about"], &QAction::triggered, this, QOverload<>::of(&MainWindow::showAbout));

    m_actions["help.homepage"] = helpMenu->addAction(I18n::tr("menu.help.documentation"));
    connect(m_actions["help.homepage"], &QAction::triggered, this, QOverload<>::of(&MainWindow::openHomepage));

    m_actions["help.source"] = helpMenu->addAction(I18n::tr("menu.help.source"));
    connect(m_actions["help.source"], &QAction::triggered, this, QOverload<>::of(&MainWindow::openSourceUrl));
}

// --------------------------------------------------------------------------- //
// Signal wiring
// --------------------------------------------------------------------------- //
void MainWindow::wireSignals()
{
    connect(m_scanBtn, &QPushButton::clicked, this, &MainWindow::onScanClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(m_upBtn, &QPushButton::clicked, this, QOverload<>::of(&MainWindow::goUp));
    connect(m_refreshBtn, &QPushButton::clicked, this, QOverload<>::of(&MainWindow::refresh));
    connect(m_skipCheckbox, &QCheckBox::toggled, this, &MainWindow::onSkipToggled);
    connect(m_searchBox, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);

    connect(m_breadcrumb, &BreadcrumbBar::navigated, this, [this](std::shared_ptr<FileNode> node) {
        navigateTo(node);
    });

    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onItemDoubleClicked);
    connect(m_tree->header(), &QHeaderView::sectionClicked, this, &MainWindow::onHeaderClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onContextMenu);

    connect(m_treemap, &TreemapWidget::cellDoubleClicked, this, [this](std::shared_ptr<FileNode> node) {
        navigateTo(node);
    });
    connect(m_treemap, &TreemapWidget::hovered, this, &MainWindow::onTreemapHover);

    // Cleanup panel signals
    connect(m_cleanupPanel, &CleanupPanel::cleanRequested, this, &MainWindow::onCleanTargets);
    connect(m_cleanupPanel, &CleanupPanel::rescanRequested, this, &MainWindow::onCleanupRescan);
    connect(m_cleanupPanel, &CleanupPanel::pathRevealRequested, this, [this](const QString& path) {
        revealInExplorer(path);
    });

    // Keyboard shortcuts
    auto* esc = new QShortcut(QKeySequence("Escape"), this);
    connect(esc, &QShortcut::activated, this, &MainWindow::onEscape);

    auto* ret = new QShortcut(QKeySequence("Return"), m_tree);
    connect(ret, &QShortcut::activated, this, &MainWindow::onEnter);

    auto* enter = new QShortcut(QKeySequence("Enter"), m_tree);
    connect(enter, &QShortcut::activated, this, &MainWindow::onEnter);

    auto* del = new QShortcut(QKeySequence("Delete"), m_tree);
    connect(del, &QShortcut::activated, this, QOverload<>::of(&MainWindow::recycleSelected));

    auto* shiftDel = new QShortcut(QKeySequence("Shift+Delete"), m_tree);
    connect(shiftDel, &QShortcut::activated, this, QOverload<>::of(&MainWindow::deletePermanentSelected));

    auto* ctrlF = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(ctrlF, &QShortcut::activated, this, [this]() { m_searchBox->setFocus(); });
}

void MainWindow::populatePathCombo()
{
    m_pathCombo->clear();
    m_pathCombo->addItem(getHomeDir());
    for (const auto& d : listDrives())
        m_pathCombo->addItem(d);
    m_pathCombo->setEditText(getHomeDir());
}

// --------------------------------------------------------------------------- //
// Scanning
// --------------------------------------------------------------------------- //
void MainWindow::onSkipToggled(bool checked)
{
    m_skipHeavyDirs = checked;
}

void MainWindow::onScanClicked()
{
    QString path = m_pathCombo->currentText().trimmed();
    if (path.isEmpty())
        path = getHomeDir();
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, APP_NAME,
            I18n::tr("msg.path_missing", QMap<QString, QString>{{"path", path}}));
        return;
    }
    startScan(path);
}

void MainWindow::onBrowse()
{
    QString start = m_pathCombo->currentText().trimmed();
    if (start.isEmpty())
        start = getHomeDir();
    QString path = QFileDialog::getExistingDirectory(this, I18n::tr("menu.file.open_folder"), start);
    if (!path.isEmpty()) {
        m_pathCombo->setEditText(path);
        startScan(path);
    }
}

void MainWindow::startScan(const QString& path)
{
    if (m_scanner) {
        m_scanner->cancel();
        if (!m_scanner->wait(100)) {
            m_statusLabel->setText(I18n::tr("status.cancelling"));
            return;
        }
        delete m_scanner;
        m_scanner = nullptr;
    }

    if (m_root)
        m_lastScanPath = m_root->path;

    m_scanBtn->setVisible(false);
    m_cancelBtn->setVisible(true);
    m_progress->setVisible(true);
    m_statusLabel->setText(I18n::tr("status.scanning", QMap<QString, QString>{{"path", path}}));
    m_tree->clear();
    m_treemap->clear();
    m_root.reset();
    m_current.reset();
    m_breadcrumb->setNode(nullptr);

    m_scanner = new DiskScanner(path, this);
    m_scanner->setSkipHeavyDirs(m_skipHeavyDirs);
    connect(m_scanner, &DiskScanner::progress, this, &MainWindow::onScanProgress);
    connect(m_scanner, &DiskScanner::finishedTree, this, &MainWindow::onScanDone);
    connect(m_scanner, &DiskScanner::error, this, &MainWindow::onScanError);
    m_scanner->start();
}

void MainWindow::onScanProgress(const QString& path)
{
    m_statusLabel->setText(I18n::tr("status.scanning", QMap<QString, QString>{{"path", path}}));
}

void MainWindow::onScanDone(std::shared_ptr<FileNode> root)
{
    m_scanner = nullptr;
    m_scanBtn->setVisible(true);
    m_cancelBtn->setVisible(false);
    m_progress->setVisible(false);
    m_root = root;
    m_lastScanPath = root->path;
    navigateTo(root);

    m_statusLabel->setText(I18n::tr("status.scanned", QMap<QString, QString>{
        {"path", root->path},
        {"size", humanSize(root->size)},
        {"files", humanCount(root->fileCount)},
        {"folders", humanCount(root->dirCount)},
    }));
    updateDiskFreeLabel(root->path);

    // Auto-start cleanup scan on the scanned path.
    startCleanupScan(root->path);
}

void MainWindow::onScanError(const QString& msg)
{
    m_scanner = nullptr;
    m_scanBtn->setVisible(true);
    m_cancelBtn->setVisible(false);
    m_progress->setVisible(false);
    if (msg.toLower().contains("cancelled")) {
        m_statusLabel->setText(I18n::tr("status.cancelled"));
        if (!m_lastScanPath.isEmpty())
            updateDiskFreeLabel(m_lastScanPath);
    } else {
        m_statusLabel->setText(I18n::tr("status.scan_failed", QMap<QString, QString>{{"msg", msg}}));
        QMessageBox::warning(this, APP_NAME,
            I18n::tr("msg.scan_failed", QMap<QString, QString>{{"msg", msg}}));
    }
}

void MainWindow::onCancel()
{
    if (m_scanner) {
        m_scanner->cancel();
        m_statusLabel->setText(I18n::tr("status.cancelling"));
    }
}

// --------------------------------------------------------------------------- //
// Navigation
// --------------------------------------------------------------------------- //
void MainWindow::navigateTo(std::shared_ptr<FileNode> node)
{
    if (!node)
        return;
    m_current = node;
    m_breadcrumb->setNode(node);
    populateList(node);
    m_treemap->setNode(node, m_showFiles, 1);
    updateStatusForCurrent();
}

void MainWindow::goUp()
{
    if (m_current) {
        auto parent = m_current->parent.lock();
        if (parent)
            navigateTo(parent);
    }
}

void MainWindow::refresh()
{
    if (m_current)
        startScan(m_current->path);
}

void MainWindow::toggleShowFiles()
{
    QAction* act = m_actions.value("view.show_files");
    if (act)
        onShowFilesToggled(act->isChecked());
}

void MainWindow::onShowFilesToggled(bool checked)
{
    m_showFiles = checked;
    if (m_current)
        m_treemap->setNode(m_current, m_showFiles, 1);
}

void MainWindow::onEscape()
{
    if (!m_searchBox->text().isEmpty()) {
        m_searchBox->clear();
    } else {
        goUp();
    }
}

void MainWindow::onEnter()
{
    auto* item = m_tree->currentItem();
    if (!item)
        return;
    auto node = item->data(0, Qt::UserRole).value<std::shared_ptr<FileNode>>();
    if (node && node->isDir()) {
        if (node->skipped)
            showSkippedMsg();
        else
            navigateTo(node);
    }
}

void MainWindow::onItemDoubleClicked(QTreeWidgetItem* item, int /*col*/)
{
    auto node = item->data(0, Qt::UserRole).value<std::shared_ptr<FileNode>>();
    if (!node)
        return;
    if (node->isDir()) {
        if (node->skipped)
            showSkippedMsg();
        else
            navigateTo(node);
    } else {
        openPath(node->path);
    }
}

void MainWindow::showSkippedMsg()
{
    QMessageBox::information(this, APP_NAME, I18n::tr("msg.skipped_contents"));
}

void MainWindow::updateDiskFreeLabel(const QString& path)
{
    auto [free, used, total] = WinApi::getDiskFreeSpace(path);
    QString freeStr = free > 0 ? humanSize(free) : QStringLiteral("\u2014");
    double pctUsed = total > 0 ? (used * 100.0 / total) : 0.0;
    m_diskFreeText = I18n::tr("status.disk_free", QMap<QString, QString>{
        {"free", freeStr},
        {"total", humanSize(total)},
        {"pct", QString::number(100 - pctUsed, 'f', 0)},
    });
    m_hoverLabel->setText(m_diskFreeText);
}

void MainWindow::onTreemapHover(std::shared_ptr<FileNode> node)
{
    if (!node) {
        m_hoverLabel->setText(m_diskFreeText);
        return;
    }
    QString pct;
    if (m_current && m_current->size > 0)
        pct = QStringLiteral("  \u2022  %1%").arg(100.0 * node->size / m_current->size, 0, 'f', 1);
    m_hoverLabel->setText(QStringLiteral("%1  \u2022  %2%3")
                              .arg(node->name)
                              .arg(humanSize(node->size))
                              .arg(pct));
}

// --------------------------------------------------------------------------- //
// List population (with per-column sort, tooltips, identify)
// --------------------------------------------------------------------------- //
void MainWindow::populateList(std::shared_ptr<FileNode> node)
{
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    if (!node)
        return;

    qint64 total = std::max<qint64>(1, node->size);
    auto children = node->children;

    // Filter by search text.
    if (!m_searchText.isEmpty()) {
        QString q = m_searchText.toLower();
        children.erase(
            std::remove_if(children.begin(), children.end(),
                           [&q](const std::shared_ptr<FileNode>& c) {
                               return !c->name.toLower().contains(q);
                           }),
            children.end());
    }

    // Sort using the current column + order.
    children = sortChildren(children);

    // ".." parent entry for ncdu feel.
    auto parent = node->parent.lock();
    if (parent && m_searchText.isEmpty()) {
        auto* upItem = new QTreeWidgetItem(QStringList{
            I18n::tr("breadcrumb.parent"), QString(), QString(), QString(), QString(), QString()});
        upItem->setData(0, Qt::UserRole, QVariant::fromValue(parent));
        upItem->setForeground(0, QColor(QString::fromLatin1(C::TEXT_SEC)));
        QFont f = upItem->font(0);
        f.setItalic(true);
        upItem->setFont(0, f);
        upItem->setToolTip(0, parent->path);
        m_tree->addTopLevelItem(upItem);
    }

    for (const auto& c : children) {
        auto* item = new QTreeWidgetItem;
        item->setText(0, c->name);
        item->setData(0, Qt::UserRole, QVariant::fromValue(c));
        item->setIcon(0, typeIcon(c));
        item->setToolTip(0, buildRowTooltip(c));

        double fraction = total > 0 ? static_cast<double>(c->size) / total : 0.0;
        QString color = typeColor(c->name, c->isDir(), c->isHidden);
        item->setData(1, SizeBarDelegate::BAR_ROLE, fraction);
        item->setData(1, SizeBarDelegate::COLOR_ROLE, color);
        item->setTextAlignment(1, Qt::AlignCenter);

        item->setText(2, humanSize(c->size));
        item->setTextAlignment(2, Qt::AlignRight);
        item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC)));
        item->setToolTip(2, I18n::tr("size.bytes", QMap<QString, QString>{{"n", withCommas(c->size)}}));

        double pct = total > 0 ? 100.0 * c->size / total : 0.0;
        item->setText(3, pct >= 0.05 ? QString::number(pct, 'f', 1) + "%" : QString());
        item->setTextAlignment(3, Qt::AlignRight);
        item->setForeground(3, QColor(QString::fromLatin1(C::TEXT_SEC)));

        if (c->isDir())
            item->setText(4, humanCount(c->totalItems()));
        else
            item->setText(4, QStringLiteral("\u2014"));
        item->setTextAlignment(4, Qt::AlignRight);
        item->setForeground(4, QColor(QString::fromLatin1(C::TEXT_MUTED)));

        item->setText(5, I18n::tr(typeKey(c->name, c->isDir())));
        item->setForeground(5, QColor(QString::fromLatin1(C::TEXT_MUTED)));

        if (!c->error.isEmpty())
            item->setForeground(0, QColor(QString::fromLatin1(C::TEXT_MUTED)));
        if (c->skipped) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, QColor(QString::fromLatin1(C::TEXT_SEC)));
        }

        m_tree->addTopLevelItem(item);
    }

    m_tree->header()->setSectionsClickable(true);
    reflectSortIndicator();
}

QString MainWindow::buildRowTooltip(const std::shared_ptr<FileNode>& node) const
{
    QStringList lines;
    lines << node->path;
    QString desc = Identify::describe(node, [](const QString& key) { return I18n::tr(key); });
    if (!desc.isEmpty()) {
        lines << QString();
        lines << I18n::tr("properties.identify") + ": " + desc;
    }
    if (node->skipped) {
        lines << QString();
        lines << I18n::tr("properties.note") + ": " + I18n::tr("status.skipped");
    }
    if (!node->error.isEmpty()) {
        lines << QString();
        lines << I18n::tr("properties.note") + ": " + node->error;
    }
    return lines.join("\n");
}

std::vector<std::shared_ptr<FileNode>> MainWindow::sortChildren(
    const std::vector<std::shared_ptr<FileNode>>& children) const
{
    QString key = sortKeyForColumn(m_sortCol);
    bool desc = m_sortOrder == Qt::DescendingOrder;

    auto result = children;

    auto nameCmp = [](const std::shared_ptr<FileNode>& a,
                      const std::shared_ptr<FileNode>& b) {
        return a->name.toLower().compare(b->name.toLower());
    };

    if (key == "name") {
        auto cmp = [&](const std::shared_ptr<FileNode>& a,
                       const std::shared_ptr<FileNode>& b) {
            int r = nameCmp(a, b);
            return desc ? r > 0 : r < 0;
        };
        std::sort(result.begin(), result.end(), cmp);
    } else if (key == "items") {
        auto cmp = [&](const std::shared_ptr<FileNode>& a,
                       const std::shared_ptr<FileNode>& b) {
            if (a->totalItems() != b->totalItems())
                return desc ? a->totalItems() > b->totalItems()
                            : a->totalItems() < b->totalItems();
            return nameCmp(a, b) < 0;
        };
        std::sort(result.begin(), result.end(), cmp);
    } else if (key == "type") {
        auto cmp = [&](const std::shared_ptr<FileNode>& a,
                       const std::shared_ptr<FileNode>& b) {
            QString ta = typeKey(a->name, a->isDir());
            QString tb = typeKey(b->name, b->isDir());
            int r = ta.compare(tb);
            if (r == 0)
                r = nameCmp(a, b);
            return desc ? r > 0 : r < 0;
        };
        std::sort(result.begin(), result.end(), cmp);
    } else {  // size
        auto cmp = [&](const std::shared_ptr<FileNode>& a,
                       const std::shared_ptr<FileNode>& b) {
            if (a->size != b->size)
                return desc ? a->size > b->size : a->size < b->size;
            return nameCmp(a, b) < 0;
        };
        std::sort(result.begin(), result.end(), cmp);
    }

    return result;
}

void MainWindow::updateStatusForCurrent()
{
    auto n = m_current;
    if (!n)
        return;
    m_statusLabel->setText(I18n::tr("status.current", QMap<QString, QString>{
        {"path", n->path},
        {"size", humanSize(n->size)},
        {"files", humanCount(n->fileCount)},
        {"folders", humanCount(n->dirCount)},
    }));
}

// --------------------------------------------------------------------------- //
// Search / sort
// --------------------------------------------------------------------------- //
void MainWindow::onSearchChanged(const QString& text)
{
    m_searchText = text.trimmed();
    if (m_current)
        populateList(m_current);
}

void MainWindow::onHeaderClicked(int logicalIndex)
{
    if (logicalIndex < 0 || logicalIndex >= SORT_COLUMNS_COUNT)
        return;
    if (logicalIndex == m_sortCol) {
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder)
                          ? Qt::DescendingOrder
                          : Qt::AscendingOrder;
    } else {
        m_sortCol = logicalIndex;
        m_sortOrder = defaultSortOrderForColumn(logicalIndex);
    }
    reflectSortIndicator();
    if (m_current)
        populateList(m_current);
}

void MainWindow::reflectSortIndicator()
{
    m_tree->header()->setSortIndicator(m_sortCol, m_sortOrder);
}

// --------------------------------------------------------------------------- //
// Context menu + actions
// --------------------------------------------------------------------------- //
void MainWindow::onContextMenu(const QPoint& pos)
{
    auto* item = m_tree->itemAt(pos);
    if (!item)
        return;
    auto node = item->data(0, Qt::UserRole).value<std::shared_ptr<FileNode>>();
    if (!node)
        return;

    auto* menu = new QMenu(this);
    QAction* actOpen = menu->addAction(
        node->isDir() ? I18n::tr("ctx.open_folder") : I18n::tr("ctx.open_file"));
    connect(actOpen, &QAction::triggered, this, [this, node]() { openPath(node->path); });

    QAction* actExplorer = menu->addAction(I18n::tr("ctx.reveal"));
    connect(actExplorer, &QAction::triggered, this, [this, node]() { revealInExplorer(node->path); });

    QAction* actCopy = menu->addAction(I18n::tr("ctx.copy_path"));
    connect(actCopy, &QAction::triggered, this, [this, node]() { copyPath(node->path); });

    menu->addSeparator();

    QAction* actRecycle = menu->addAction(I18n::tr("ctx.recycle"));
    connect(actRecycle, &QAction::triggered, this, QOverload<>::of(&MainWindow::recycleSelected));

    QAction* actPerm = menu->addAction(I18n::tr("ctx.delete_permanent"));
    connect(actPerm, &QAction::triggered, this, QOverload<>::of(&MainWindow::deletePermanentSelected));

    menu->addSeparator();

    QAction* actProp = menu->addAction(I18n::tr("ctx.properties"));
    connect(actProp, &QAction::triggered, this, [this, node]() { showProperties(node); });

    menu->exec(QCursor::pos());
    delete menu;
}

void MainWindow::openPath(const QString& path)
{
    WinApi::openPath(path);
}

void MainWindow::revealInExplorer(const QString& path)
{
    WinApi::revealInExplorer(path);
}

void MainWindow::copyPath(const QString& path)
{
    QApplication::clipboard()->setText(path);
    m_statusLabel->setText(I18n::tr("status.copied", QMap<QString, QString>{{"path", path}}));
}

// --------------------------------------------------------------------------- //
// Deletion
// --------------------------------------------------------------------------- //
void MainWindow::collectDeletable(
    std::vector<std::shared_ptr<FileNode>>& deletable,
    std::vector<std::shared_ptr<FileNode>>& rejected) const
{
    auto items = m_tree->selectedItems();
    for (auto* it : items) {
        auto n = it->data(0, Qt::UserRole).value<std::shared_ptr<FileNode>>();
        if (!n)
            continue;
        if (n->parent.expired()) {
            rejected.push_back(n);  // refuse to delete top-level drive/folder
        } else {
            deletable.push_back(n);
        }
    }
}

void MainWindow::formatNames(const std::vector<std::shared_ptr<FileNode>>& nodes,
                             QString& names, QString& more) const
{
    QStringList parts;
    int limit = std::min<int>(8, static_cast<int>(nodes.size()));
    for (int i = 0; i < limit; ++i)
        parts << QStringLiteral("\u2022 %1").arg(nodes[i]->name);
    names = parts.join("\n");
    if (static_cast<int>(nodes.size()) > 8)
        more = I18n::tr("dialog.recycle.more", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()) - 8)}});
    else
        more.clear();
}

void MainWindow::recycleSelected()
{
    std::vector<std::shared_ptr<FileNode>> nodes, rejected;
    collectDeletable(nodes, rejected);
    if (nodes.empty() && rejected.empty())
        return;
    if (!rejected.empty()) {
        QMessageBox::warning(this, APP_NAME, I18n::tr("dialog.delete.warn_root"));
        if (nodes.empty())
            return;
    }

    QString names, more;
    formatNames(nodes, names, more);
    auto reply = QMessageBox::question(
        this, I18n::tr("dialog.recycle.title"),
        I18n::tr("dialog.recycle.body", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()))},
            {"names", names},
            {"more", more},
        }),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reply != QMessageBox::Yes)
        return;

    QStringList paths;
    for (const auto& n : nodes)
        paths << n->path;

    bool ok = WinApi::sendToRecycleBin(paths);
    if (ok) {
        afterDelete(nodes);
        m_statusLabel->setText(I18n::tr("status.moved_recycle", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()))}}));
    } else {
        QMessageBox::warning(this, APP_NAME, I18n::tr("dialog.delete.failed"));
    }
}

void MainWindow::deletePermanentSelected()
{
    std::vector<std::shared_ptr<FileNode>> nodes, rejected;
    collectDeletable(nodes, rejected);
    if (nodes.empty() && rejected.empty())
        return;
    if (!rejected.empty()) {
        QMessageBox::warning(this, APP_NAME, I18n::tr("dialog.delete.warn_root"));
        if (nodes.empty())
            return;
    }

    QString names, more;
    formatNames(nodes, names, more);
    auto reply = QMessageBox::question(
        this, I18n::tr("dialog.delete.title"),
        I18n::tr("dialog.delete.body", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()))},
            {"names", names},
            {"more", more},
        }),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reply != QMessageBox::Yes)
        return;

    QStringList paths;
    for (const auto& n : nodes)
        paths << n->path;

    bool ok = WinApi::deletePermanent(paths);
    if (ok) {
        afterDelete(nodes);
        m_statusLabel->setText(I18n::tr("status.deleted", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()))}}));
    } else {
        QMessageBox::warning(this, APP_NAME, I18n::tr("dialog.delete.failed"));
    }
}

std::shared_ptr<FileNode> MainWindow::findNodeByPath(const QString& path) const
{
    if (!m_root)
        return nullptr;
    QString norm = QFileInfo(path).absoluteFilePath().toLower();
    QString rootNorm = QFileInfo(m_root->path).absoluteFilePath().toLower();
    if (norm == rootNorm)
        return m_root;
    // Verify that norm is under rootNorm.
    if (!norm.startsWith(rootNorm + '/'))
        return nullptr;
    QString rel = norm.mid(rootNorm.length() + 1);
    auto node = m_root;
    auto parts = rel.split('/', Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        QString partNorm = part.toLower();
        std::shared_ptr<FileNode> found;
        for (const auto& child : node->children) {
            if (child->name.toLower() == partNorm) {
                found = child;
                break;
            }
        }
        if (!found)
            return nullptr;
        node = found;
    }
    return node;
}

bool MainWindow::pruneMissingFromNode(const std::shared_ptr<FileNode>& node)
{
    if (!node->isDir())
        return false;
    std::vector<std::shared_ptr<FileNode>> removed;
    for (const auto& child : node->children) {
        if (!QFileInfo::exists(child->path)) {
            removed.push_back(child);
        } else if (child->isDir()) {
            pruneMissingFromNode(child);
            if (!QFileInfo::exists(child->path))
                removed.push_back(child);
        }
    }
    for (const auto& child : removed) {
        auto it = std::find(node->children.begin(), node->children.end(), child);
        if (it != node->children.end())
            node->children.erase(it);
    }
    if (!removed.empty()) {
        recomputeSizes(node);
        return true;
    }
    return false;
}

void MainWindow::syncTreeAfterCleanup(
    const std::vector<CleanupWorker::ItemRef>& successItems)
{
    std::vector<std::shared_ptr<FileNode>> nodesToRemove;
    bool pruned = false;

    for (const auto& item : successItems) {
        if (item.type == "file") {
            // Remove from large files list.
            m_largeFiles.erase(
                std::remove_if(m_largeFiles.begin(), m_largeFiles.end(),
                               [&](const LargeFile& lf) { return lf.path == item.path; }),
                m_largeFiles.end());
            auto node = findNodeByPath(item.path);
            if (node && !node->parent.expired())
                nodesToRemove.push_back(node);
        } else if (item.type == "target") {
            auto node = findNodeByPath(item.path);
            if (!node)
                continue;
            if (!QFileInfo::exists(item.path)) {
                if (!node->parent.expired())
                    nodesToRemove.push_back(node);
            } else if (node->isDir()) {
                if (pruneMissingFromNode(node))
                    pruned = true;
            }
        }
    }

    if (!nodesToRemove.empty()) {
        afterDelete(nodesToRemove);
    } else if (pruned && m_current) {
        navigateTo(m_current);
    }
}

void MainWindow::afterDelete(const std::vector<std::shared_ptr<FileNode>>& nodes)
{
    std::vector<std::shared_ptr<FileNode>> parents;
    std::set<FileNode*> seen;
    for (const auto& n : nodes) {
        auto p = n->parent.lock();
        if (!p)
            continue;
        auto it = std::find(p->children.begin(), p->children.end(), n);
        if (it != p->children.end())
            p->children.erase(it);
        if (seen.insert(p.get()).second)
            parents.push_back(p);
    }
    for (const auto& parent : parents)
        recomputeSizes(parent);
    if (m_current)
        navigateTo(m_current);
}

void MainWindow::recomputeSizes(std::shared_ptr<FileNode> node)
{
    while (node) {
        qint64 size = 0;
        int fileCount = 0;
        int dirCount = 0;
        for (const auto& c : node->children) {
            size += c->size;
            if (c->isDir()) {
                fileCount += c->fileCount;
                dirCount += 1 + c->dirCount;
            } else if (c->nodeType == NodeType::File) {
                fileCount += 1;
            }
        }
        node->size = size;
        node->fileCount = fileCount;
        node->dirCount = dirCount;
        node = node->parent.lock();
    }
}

// --------------------------------------------------------------------------- //
// Properties dialog (with identify integration)
// --------------------------------------------------------------------------- //
void MainWindow::showProperties(const std::shared_ptr<FileNode>& node)
{
    QDialog dlg(this);
    dlg.setWindowTitle(I18n::tr("properties.title"));
    dlg.setMinimumWidth(460);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(8);

    auto* title = new QLabel(node->name);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700; color: %1;")
                             .arg(QString::fromLatin1(C::TEXT)));
    lay->addWidget(title);
    lay->addSpacing(4);

    auto addRow = [&](const QString& labelKey, const QString& value) {
        auto* rowW = new QWidget;
        auto* rlay = new QHBoxLayout(rowW);
        rlay->setContentsMargins(0, 0, 0, 0);
        rlay->setSpacing(12);
        auto* lab = new QLabel(I18n::tr(labelKey));
        lab->setStyleSheet(QStringLiteral("color: %1; min-width: 90px;")
                               .arg(QString::fromLatin1(C::TEXT_MUTED)));
        auto* val = new QLabel(value);
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rlay->addWidget(lab);
        rlay->addWidget(val, 1);
        lay->addWidget(rowW);
    };

    addRow("properties.name", node->name);
    addRow("properties.path", node->path);
    addRow("properties.type", I18n::tr(typeKey(node->name, node->isDir())));
    addRow("properties.size", humanSize(node->size));
    if (m_current && m_current->size > 0) {
        double pct = 100.0 * node->size / m_current->size;
        addRow("properties.percent",
               I18n::tr("properties.percent_of", QMap<QString, QString>{
                   {"pct", QString::number(pct, 'f', 2)}}));
    }
    if (node->isDir()) {
        addRow("properties.files", humanCount(node->fileCount));
        addRow("properties.folders", humanCount(node->dirCount));
    }

    QString desc = Identify::describe(node, [](const QString& key) { return I18n::tr(key); });
    if (!desc.isEmpty())
        addRow("properties.identify", desc);
    if (!node->error.isEmpty())
        addRow("properties.note", node->error);

    lay->addStretch(1);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* closeBtn = new QPushButton(I18n::tr("button.close"));
    closeBtn->setObjectName("primary");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(closeBtn);
    lay->addLayout(btnRow);

    dlg.exec();
}

// --------------------------------------------------------------------------- //
// About dialog
// --------------------------------------------------------------------------- //
void MainWindow::showAbout()
{
    QDialog dlg(this);
    dlg.setWindowTitle(I18n::tr("about.title"));
    dlg.setMinimumWidth(420);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(24, 24, 24, 18);
    lay->setSpacing(10);

    auto* title = new QLabel(APP_NAME);
    title->setObjectName("about-title");
    lay->addWidget(title);

    auto* body = new QLabel(I18n::tr("about.body", QMap<QString, QString>{{"version", APP_VERSION}}));
    body->setObjectName("about-body");
    body->setWordWrap(true);
    lay->addWidget(body);

    auto* homepageLbl = new QLabel(
        QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none;\">%3</a>")
            .arg(HOMEPAGE)
            .arg(QString::fromLatin1(C::PRIMARY))
            .arg(I18n::tr("about.homepage", QMap<QString, QString>{{"url", HOMEPAGE}})));
    homepageLbl->setOpenExternalLinks(true);
    homepageLbl->setWordWrap(true);
    lay->addWidget(homepageLbl);

    auto* srcLbl = new QLabel(
        QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none;\">%3</a>")
            .arg(SOURCE_URL)
            .arg(QString::fromLatin1(C::PRIMARY))
            .arg(I18n::tr("about.source", QMap<QString, QString>{{"url", SOURCE_URL}})));
    srcLbl->setOpenExternalLinks(true);
    srcLbl->setWordWrap(true);
    lay->addWidget(srcLbl);

    auto* thanks = new QLabel(I18n::tr("about.thanks"));
    thanks->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                              .arg(QString::fromLatin1(C::TEXT_MUTED)));
    thanks->setWordWrap(true);
    lay->addWidget(thanks);

    lay->addStretch(1);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* closeBtn = new QPushButton(I18n::tr("button.ok"));
    closeBtn->setObjectName("primary");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(closeBtn);
    lay->addLayout(btnRow);

    dlg.exec();
}

void MainWindow::openHomepage()
{
    QDesktopServices::openUrl(QUrl(HOMEPAGE));
}

void MainWindow::openSourceUrl()
{
    QDesktopServices::openUrl(QUrl(SOURCE_URL));
}

// --------------------------------------------------------------------------- //
// Cleanup (S/A/B/C/D system + large files)
// --------------------------------------------------------------------------- //
void MainWindow::startCleanupScan(const QString& scanPath)
{
    if (m_cleanupScanner) {
        m_cleanupScanner->cancel();
        if (!m_cleanupScanner->wait(100)) {
            m_statusLabel->setText(I18n::tr("status.cancelling"));
            return;
        }
        delete m_cleanupScanner;
        m_cleanupScanner = nullptr;
    }

    m_cleanupPanel->startScanProgress();
    m_cleanupTargets.clear();
    m_largeFiles.clear();

    m_cleanupScanner = new CleanupScanner(scanPath, m_root, this);
    connect(m_cleanupScanner, &CleanupScanner::targetScanned,
            this, &MainWindow::onCleanupTargetScanned);
    connect(m_cleanupScanner, &CleanupScanner::largeFileFound,
            this, &MainWindow::onLargeFileFound);
    connect(m_cleanupScanner, &CleanupScanner::finished,
            this, &MainWindow::onCleanupScanDone);
    m_cleanupScanner->start();
}

void MainWindow::onCleanupTargetScanned(CleanupTarget target)
{
    m_cleanupTargets.push_back(target);
    m_cleanupPanel->addTarget(target);
}

void MainWindow::onLargeFileFound(LargeFile lf)
{
    m_largeFiles.push_back(lf);
    m_cleanupPanel->addLargeFile(lf);
}

void MainWindow::onCleanupScanDone(std::vector<CleanupTarget> targets,
                                   std::vector<LargeFile> largeFiles,
                                   qint64 /*totalSize*/, int /*totalCount*/,
                                   qint64 /*largeTotal*/)
{
    m_cleanupScanner = nullptr;
    m_cleanupTargets = std::move(targets);
    m_largeFiles = std::move(largeFiles);

    QString path = m_root ? m_root->path : getHomeDir();
    auto [free, used, total] = WinApi::getDiskFreeSpace(path);
    m_cleanupPanel->loadTargets(m_cleanupTargets, m_largeFiles, free, total);
    m_cleanupPanel->stopScanProgress();
}

void MainWindow::onCleanTargets(
    const std::vector<std::tuple<QString, QString, QString>>& items)
{
    if (items.empty())
        return;

    // Separate targets and files.
    std::vector<std::pair<QString, QString>> targetItems;
    std::vector<QString> fileItems;
    for (const auto& [type, key, path] : items) {
        if (type == "target")
            targetItems.emplace_back(key, path);
        else if (type == "file")
            fileItems.push_back(path);
    }

    // Calculate total size.
    qint64 totalSize = 0;
    for (const auto& [key, path] : targetItems) {
        for (const auto& t : m_cleanupTargets) {
            if (t.key == key && t.path == path) {
                totalSize += t.size;
                break;
            }
        }
    }
    for (const auto& fp : fileItems) {
        for (const auto& lf : m_largeFiles) {
            if (lf.path == fp) {
                totalSize += lf.size;
                break;
            }
        }
    }

    // Build confirmation message.
    int count = static_cast<int>(targetItems.size() + fileItems.size());
    QStringList namesList;
    int limit = std::min(8, count);
    for (int i = 0; i < static_cast<int>(targetItems.size()) && i < limit; ++i) {
        const auto& [key, path] = targetItems[i];
        qint64 sz = 0;
        for (const auto& t : m_cleanupTargets) {
            if (t.key == key && t.path == path) {
                sz = t.size;
                break;
            }
        }
        namesList << QStringLiteral("\u2022 %1 (%2)").arg(I18n::tr(key)).arg(humanSize(sz));
    }
    int fileLimit = limit - static_cast<int>(targetItems.size());
    if (fileLimit > 0) {
        for (int i = 0; i < static_cast<int>(fileItems.size()) && i < fileLimit; ++i) {
            const auto& fp = fileItems[i];
            QString name = QFileInfo(fp).fileName();
            qint64 sz = 0;
            for (const auto& lf : m_largeFiles) {
                if (lf.path == fp) {
                    sz = lf.size;
                    break;
                }
            }
            namesList << QStringLiteral("\u2022 %1 (%2)").arg(name).arg(humanSize(sz));
        }
    }
    QString names = namesList.join("\n");
    QString more = count > 8
                       ? I18n::tr("dialog.recycle.more", QMap<QString, QString>{{"n", QString::number(count - 8)}})
                       : QString();

    // Check for S-level items (auto-clean, no confirmation needed).
    bool hasSOnly = false;
    if (!targetItems.empty() && fileItems.empty()) {
        hasSOnly = true;
        for (const auto& [key, path] : targetItems) {
            bool isS = false;
            for (const auto& t : m_cleanupTargets) {
                if (t.danger == DangerLevel::S && t.key == key && t.path == path) {
                    isS = true;
                    break;
                }
            }
            if (!isS) {
                hasSOnly = false;
                break;
            }
        }
    }

    if (!hasSOnly) {
        auto reply = QMessageBox::question(
            this, I18n::tr("cleanup.clean_title"),
            I18n::tr("cleanup.clean_body", QMap<QString, QString>{
                {"size", humanSize(totalSize)},
                {"names", names},
                {"more", more},
            }),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Yes)
            return;
    }

    // Cancel any existing cleanup worker.
    if (m_cleanupWorker) {
        m_cleanupWorker->cancel();
        if (!m_cleanupWorker->wait(100)) {
            m_statusLabel->setText(I18n::tr("status.cancelling"));
            return;
        }
        delete m_cleanupWorker;
        m_cleanupWorker = nullptr;
    }

    m_cleanupPanel->setCleaning(true);

    // Convert items to CleanupWorker::ItemRef.
    std::vector<CleanupWorker::ItemRef> refs;
    for (const auto& [type, key, path] : items)
        refs.push_back({type, key, path});

    m_cleanupWorker = new CleanupWorker(refs, m_cleanupTargets, m_largeFiles, this);
    connect(m_cleanupWorker, &CleanupWorker::progress, this, &MainWindow::onCleanupProgress);
    connect(m_cleanupWorker, &CleanupWorker::itemDone, this, &MainWindow::onCleanupItemDone);
    connect(m_cleanupWorker, &CleanupWorker::finished, this, &MainWindow::onCleanupFinished);
    m_cleanupWorker->start();
}

void MainWindow::onCleanupProgress(const QString& label)
{
    m_statusLabel->setText(I18n::tr("status.cleaning", QMap<QString, QString>{{"item", label}}));
}

void MainWindow::onCleanupItemDone(const QString& /*key*/, int /*deleted*/,
                                   int /*skipped*/, qint64 /*freed*/)
{
    if (m_root) {
        updateDiskFreeLabel(m_root->path);
        auto [free, used, total] = WinApi::getDiskFreeSpace(m_root->path);
        m_cleanupPanel->updateFreeSpace(free, total);
    }
}

void MainWindow::onCleanupFinished(int totalDeleted, int totalSkipped, qint64 totalFreed,
                                   int /*totalItems*/,
                                   std::vector<CleanupWorker::ItemRef> successItems,
                                   std::vector<CleanupWorker::ItemRef> failedItems)
{
    m_cleanupWorker = nullptr;
    m_cleanupPanel->setCleaning(false);
    m_cleanupPanel->removeCleanedItems(successItems);
    syncTreeAfterCleanup(successItems);

    m_statusLabel->setText(I18n::tr("cleanup.clean_done", QMap<QString, QString>{
        {"freed", humanSize(totalFreed)},
        {"deleted", humanCount(totalDeleted)},
        {"skipped", humanCount(totalSkipped)},
    }));

    // Update disk free space.
    if (m_root) {
        updateDiskFreeLabel(m_root->path);
        auto [free, used, total] = WinApi::getDiskFreeSpace(m_root->path);
        m_cleanupPanel->updateFreeSpace(free, total);
    }

    if (!failedItems.empty()) {
        QStringList failedParts;
        int failLimit = std::min<int>(10, static_cast<int>(failedItems.size()));
        for (int i = 0; i < failLimit; ++i) {
            const auto& item = failedItems[i];
            QString label = item.type == "target"
                                ? I18n::tr(item.key)
                                : QFileInfo(item.path).fileName();
            failedParts << QStringLiteral("\u2022 %1").arg(label);
        }
        QString failedMsg = failedParts.join("\n");
        if (static_cast<int>(failedItems.size()) > 10)
            failedMsg += I18n::tr("cleanup.failed_more", QMap<QString, QString>{
                {"n", QString::number(static_cast<int>(failedItems.size()) - 10)}});
        QMessageBox::warning(this, I18n::tr("cleanup.failed_title"),
            I18n::tr("cleanup.failed_body", QMap<QString, QString>{{"items", failedMsg}}));
    }
}

void MainWindow::onCleanupRescan()
{
    if (m_root)
        startCleanupScan(m_root->path);
}

// --------------------------------------------------------------------------- //
// Language switching
// --------------------------------------------------------------------------- //
void MainWindow::switchLanguage(const QString& lang)
{
    if (lang == I18n::currentLanguage())
        return;
    I18n::setLanguage(lang);
    retranslateUI();
}

void MainWindow::retranslateUI()
{
    setWindowTitle(I18n::tr("app.title"));
    m_subtitleLabel->setText(I18n::tr("app.subtitle"));

    // Top bar
    m_browseBtn->setText(I18n::tr("button.browse"));
    m_scanBtn->setText(I18n::tr("button.scan"));
    m_cancelBtn->setText(I18n::tr("button.cancel"));
    m_skipCheckbox->setText(I18n::tr("button.skip_heavy"));
    m_skipCheckbox->setToolTip(I18n::tr("tooltip.skip_heavy"));

    // Toolbar
    m_searchBox->setPlaceholderText(I18n::tr("search.placeholder"));
    m_upBtn->setText(I18n::tr("button.up"));
    m_upBtn->setToolTip(I18n::tr("menu.view.up"));
    m_refreshBtn->setText(I18n::tr("button.refresh"));
    m_refreshBtn->setToolTip(I18n::tr("menu.view.refresh"));

    // Status bar
    if (!m_current)
        m_statusLabel->setText(I18n::tr("status.ready"));

    // File list headers
    setHeaderLabels();

    // Menu bar
    menuBar()->clear();
    buildMenu();

    // Legend
    m_legend->refreshLabels();

    // Right panel tabs
    m_rightTabs->setTabText(0, I18n::tr("tab.treemap"));
    m_rightTabs->setTabText(1, I18n::tr("tab.cleanup"));

    // Cleanup panel
    m_cleanupPanel->retranslate();

    // Repopulate the list so row tooltips / ".." parent update.
    if (m_current) {
        populateList(m_current);
        updateStatusForCurrent();
        updateDiskFreeLabel(m_current->path);
        m_breadcrumb->setNode(m_current);
    } else if (!m_lastScanPath.isEmpty()) {
        updateDiskFreeLabel(m_lastScanPath);
    }
}
