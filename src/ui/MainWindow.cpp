#include "MainWindow.h"

#include <QApplication>
#include <QStyleHints>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QCursor>
#include <QDialog>
#include <QDateTime>
#include <QButtonGroup>
#include <QRadioButton>
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
#include <QProgressDialog>
#include <QtConcurrent>
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
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>
#include <QApplication>
#include <QFile>
#include <QScreen>
#include <QGuiApplication>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <memory>
#include <stack>
#include <unordered_set>
#include <set>
#include <QtConcurrent>

#include "Style.h"
#include "I18n.h"
#include "DialogI18n.h"
#include "Identify.h"
#include "WinApi.h"
#include "Logger.h"
#include "FormatHelpers.h"
#include "SizeBarDelegate.h"
#include "TreemapWidget.h"
#include "BreadcrumbBar.h"
#include "LegendBar.h"
#include "CleanupPanel.h"
#include "SystemOptPanel.h"
#include "SafeMoveWorker.h"
#include "AppPathSyncDialog.h"
#include "AppDataMovePanel.h"
#include "LockerDialog.h"
#include "OpGuard.h"
#include "MftScanner.h"
#include "version.h"
#include "AiService.h"
#include "AiSettingsDialog.h"
#include "AiAnalysisDialog.h"

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

static QString sortKeyForColumn(int col, bool searchMode = false) {
    if (searchMode) {
        if (col == 0) return QStringLiteral("name");
        if (col == 1) return QStringLiteral("path");
        if (col == 2) return QStringLiteral("size");
        if (col == 5) return QStringLiteral("type");
        return QStringLiteral("size");
    }
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

// File-scope icon cache (cleared on theme switch so type colors re-render).
static QMap<QString, QIcon> g_typeIconCache;

static void clearTypeIconCache() {
    g_typeIconCache.clear();
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
    auto it = g_typeIconCache.find(cacheKey);
    if (it != g_typeIconCache.end())
        return it.value();

    QString color = typeColor(node->name, node->isDir(), node->isHidden);
    QIcon icon = makeTypeIcon(color, node->isDir());
    g_typeIconCache[cacheKey] = icon;
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
    qRegisterMetaType<DuplicateGroup>("DuplicateGroup");
    qRegisterMetaType<std::vector<DuplicateGroup>>("std::vector<DuplicateGroup>");

    buildUI();
    buildMenu();
    wireSignals();
    m_ai = new AiService(this);
    populatePathCombo();
    reflectSortIndicator();

    // Silently fetch-and-save the default AI Base URL / API Key when none has
    // been configured yet. Runs in the background; never shows UI or blocks.
    m_ai->ensureConfigured();

    // Follow the OS color scheme at runtime when the user picked "system".
    // Switching the Windows light/dark setting then re-applies the palette
    // without restarting the app.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this]() {
                if (Theme::current() == QStringLiteral("system")) {
                    Theme::applyEffective();
                    refreshTheme();
                }
            });

    // No auto-scan on startup: the user picks a path and clicks Scan. The
    // status bar shows "Ready" until then (set in retranslateUI).

    // Check for updates shortly after launch. Runs silently: only surfaces a
    // result when a newer version is available (bottom-right toast). Uses the
    // same version endpoint + comparison as Help -> Check for Updates.
    QTimer::singleShot(3000, this, [this]() { checkForUpdate(true); });
}

MainWindow::~MainWindow()
{
    // The save-location dialog borrows the move tab and hands it back in its own
    // destructor. Take it down first, while this window is still whole, so that
    // hand-back happens before the widget tree comes apart. closeEvent normally
    // keeps this state from being reached; a forced shutdown does not.
    delete m_syncDialog;
    // Threads are cancelled and waited in closeEvent; nothing extra here.
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    // The save-location dialog no longer holds this window back, which means it
    // can still be open — and it can be in the middle of a copy. Its window is
    // the one that owns that job, so closing this one while it is up would pull
    // the ground out from under the user. Ask them to close it first.
    if (m_syncDialog) {
        e->ignore();
        if (m_syncDialog->isMinimized())
            m_syncDialog->setWindowState(m_syncDialog->windowState() & ~Qt::WindowMinimized);
        m_syncDialog->show();
        m_syncDialog->raise();
        m_syncDialog->activateWindow();
        Dialogs::info(this, I18n::tr("app_sync.title"), I18n::tr("app_sync.close_first"));
        return;
    }

    if (m_scanner) {
        if (auto* ds = dynamic_cast<DiskScanner*>(m_scanner))
            ds->cancel();
        else if (auto* ms = dynamic_cast<MftScanner*>(m_scanner))
            ms->cancel();
        m_scanner->wait(3000);
    }
    if (m_cleanupScanner) {
        m_cleanupScanner->cancel();
        m_cleanupScanner->wait(3000);
    }
    if (m_dupScanner) {
        m_dupScanner->cancel();
        m_dupScanner->wait(3000);
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
                                .arg(QString::fromLatin1(C::PRIMARY())));
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
        // No leading margin: the search box lines up with the file tree's left
        // edge below it, which starts at 0 as well.
        lay->setContentsMargins(0, 4, 18, 4);
        lay->setSpacing(12);

        m_searchBox = new QLineEdit;
        m_searchBox->setObjectName(QStringLiteral("searchBox"));
        m_searchBox->setPlaceholderText(I18n::tr("search.placeholder"));
        m_searchBox->setMinimumWidth(280);
        m_searchBox->setMaximumWidth(380);
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

        m_breadcrumb = new BreadcrumbBar;
        lay->addWidget(m_breadcrumb, 1);

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
        m_tree->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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

        // Tab 3: System optimization
        m_systemOptPanel = new SystemOptPanel;
        m_rightTabs->addTab(m_systemOptPanel, I18n::tr("tab.sysopt"));
        connect(m_systemOptPanel, &SystemOptPanel::requestAppPathSync,
                this, &MainWindow::showAppPathSync);

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

    m_actions["file.app_sync"] = fileMenu->addAction(I18n::tr("app_sync.menu"));
    connect(m_actions["file.app_sync"], &QAction::triggered, this, &MainWindow::showAppPathSync);

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

    // --- Theme ---
    auto* themeMenu = mb->addMenu(I18n::tr("menu.theme"));
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    for (const auto& code : {QStringLiteral("light"), QStringLiteral("dark"),
                             QStringLiteral("system"), QStringLiteral("custom")}) {
        auto* act = themeMenu->addAction(Theme::displayName(code));
        act->setCheckable(true);
        act->setChecked(code == Theme::current());
        themeGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, code]() { switchTheme(code); });
    }
    themeMenu->addSeparator();
    m_actions["theme.customize"] = themeMenu->addAction(I18n::tr("menu.theme.customize"));
    connect(m_actions["theme.customize"], &QAction::triggered, this, &MainWindow::showThemeCustomize);

    // --- AI ---
    auto* aiMenu = mb->addMenu(I18n::tr("menu.ai"));
    m_actions["ai.settings"] = aiMenu->addAction(I18n::tr("menu.ai.settings"));
    connect(m_actions["ai.settings"], &QAction::triggered, this, &MainWindow::onAiSettings);

    m_actions["ai.analyze_current"] = aiMenu->addAction(I18n::tr("menu.ai.analyze_current"));
    m_actions["ai.analyze_current"]->setEnabled(m_current != nullptr);
    connect(m_actions["ai.analyze_current"], &QAction::triggered,
            this, &MainWindow::onAiAnalyzeCurrent);

    m_actions["ai.qa"] = aiMenu->addAction(I18n::tr("menu.ai.qa"));
    connect(m_actions["ai.qa"], &QAction::triggered, this, &MainWindow::onAiQa);

    // --- Help ---
    auto* helpMenu = mb->addMenu(I18n::tr("menu.help"));
    m_actions["help.about"] = helpMenu->addAction(I18n::tr("menu.help.about"));
    connect(m_actions["help.about"], &QAction::triggered, this, QOverload<>::of(&MainWindow::showAbout));

    m_actions["help.homepage"] = helpMenu->addAction(I18n::tr("menu.help.documentation"));
    connect(m_actions["help.homepage"], &QAction::triggered, this, QOverload<>::of(&MainWindow::openHomepage));

    helpMenu->addSeparator();

    m_actions["help.check_update"] = helpMenu->addAction(I18n::tr("menu.help.check_update"));
    connect(m_actions["help.check_update"], &QAction::triggered, this, &MainWindow::checkForUpdate);

    m_actions["help.publish_log"] = helpMenu->addAction(I18n::tr("menu.help.publish_log"));
    connect(m_actions["help.publish_log"], &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(QUrl(QString(HOMEPAGE) + QStringLiteral("#publishlog")));
    });

    m_nam = new QNetworkAccessManager(this);
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

    // Debounce timer — delays search until the user stops typing (150ms).
    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(150);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, &MainWindow::onSearchDebounceTimeout);

    // Async search watcher — runs search on a background thread so the UI stays responsive.
    m_searchWatcher = new QFutureWatcher<std::vector<std::shared_ptr<FileNode>>>(this);
    connect(m_searchWatcher, &QFutureWatcher<std::vector<std::shared_ptr<FileNode>>>::finished,
            this, [this]() {
        // Only apply results if search mode is still active and text hasn't changed.
        if (!m_inSearchMode || m_searchText != m_searchQueryPending)
            return;
        m_searchResults = m_searchWatcher->result();
        populateList(m_current);
        updateStatusForCurrent();
    });

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
    connect(m_cleanupPanel, &CleanupPanel::aiAnalyzeRequested,
            this, &MainWindow::runAiAnalysis);

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
    // Keep the user home directory as a selectable dropdown option, but default
    // to the system drive (C:\) so a fresh launch targets the whole C: volume.
    m_pathCombo->addItem(getHomeDir());
    const QStringList drives = listDrives();
    for (const auto& d : drives)
        m_pathCombo->addItem(d);
    const QString cDrive = QStringLiteral("C:\\");
    const QString def = drives.contains(cDrive) ? cDrive
                        : (!drives.isEmpty() ? drives.first() : getHomeDir());
    m_pathCombo->setEditText(def);
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
        Dialogs::warn(this, APP_NAME,
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

// Decide whether to use the volume-wide MFT scanner for `path`. MFT reads the
// entire $MFT in one sequential pass, which beats per-directory DiskScanner
// walking ONLY for very large targets: drive roots and a few well-known huge
// system directories. Smaller subdirectories use DiskScanner so they finish
// quickly instead of always paying the full-volume MFT cost.
// LARGE_MFT_FOLDER_NAMES lists direct children of a drive root (any drive
// letter) that are big enough to warrant MFT. Add names here as needed —
// matched case-insensitively against the path's final segment.
namespace {
const QStringList LARGE_MFT_FOLDER_NAMES = {
    "users",                   // C:\Users — all user profiles
    "windows",                 // C:\Windows — system files (~100k+)
    "program files",           // C:\Program Files
    "program files (x86)",     // C:\Program Files (x86)
    "programdata",             // C:\ProgramData
};
}

bool shouldUseMftScanner(const QString& path)
{
    const QFileInfo info(path);
    if (info.isRoot())
        return true;
    // Only direct children of a drive root qualify (e.g. C:\Users, not
    // C:\Users\john) — deeper paths are scoped enough for DiskScanner.
    if (!QFileInfo(info.absolutePath()).isRoot())
        return false;
    return LARGE_MFT_FOLDER_NAMES.contains(info.fileName().toLower());
}

void MainWindow::startScan(const QString& path)
{
    // Cancel and wait for any in-flight scanner before starting a new one.
    // The old scanner is scheduled for deletion via deleteLater (connected
    // to QThread::finished when the scanner was created), so we just drop
    // our pointer here — do NOT delete it manually.
    if (m_scanner) {
        if (auto* ds = dynamic_cast<DiskScanner*>(m_scanner))
            ds->cancel();
        else if (auto* ms = dynamic_cast<MftScanner*>(m_scanner))
            ms->cancel();
        if (!m_scanner->wait(100)) {
            m_statusLabel->setText(I18n::tr("status.cancelling"));
            return;
        }
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
    // Cancel any in-flight search and clear search state so the old
    // FileNode tree is fully released (search results hold shared_ptrs
    // to nodes in the old tree and would otherwise keep it alive).
    m_searchDebounceTimer->stop();
    if (m_searchWatcher->isRunning())
        m_searchWatcher->cancel();
    m_searchResults.clear();
    m_inSearchMode = false;
    m_searchBox->clear();
    m_root.reset();
    m_current.reset();
    m_breadcrumb->setNode(nullptr);
    m_scanLowMemory = false;

    // Started here, not when the tree arrives: the number the status bar shows
    // is how long the user waited for this scan, whichever scanner runs it.
    m_lastScanMs = -1;
    m_scanClock.start();

    // Use the fast MFT scanner for drive roots and well-known huge system
    // directories; for smaller subdirectories a recursive DiskScanner is faster
    // (scoped to the chosen folder) instead of always paying the full-volume
    // MFT cost. See shouldUseMftScanner() / LARGE_MFT_FOLDER_NAMES.
    if (shouldUseMftScanner(path) && MftScanner::isSupported(path)) {
        auto* scanner = new MftScanner(path, this);
        scanner->setSkipHeavyDirs(m_skipHeavyDirs);
        // Auto-delete the scanner once its run() returns. This is the Qt-
        // recommended way to manage QThread lifetimes and prevents the
        // memory leak where each completed scan left a scanner object
        // (holding its internal entries/sizes maps) alive forever.
        connect(scanner, &QThread::finished, scanner, &QObject::deleteLater);
        connect(scanner, &MftScanner::progress, this, &MainWindow::onScanProgress);
        connect(scanner, &MftScanner::finishedTree, this, &MainWindow::onScanDone);
        connect(scanner, &MftScanner::error, this, &MainWindow::onScanError);
        m_scanner = scanner;
        m_scanner->start();
    } else {
        auto* scanner = new DiskScanner(path, this);
        scanner->setSkipHeavyDirs(m_skipHeavyDirs);
        connect(scanner, &QThread::finished, scanner, &QObject::deleteLater);
        connect(scanner, &DiskScanner::progress, this, &MainWindow::onScanProgress);
        connect(scanner, &DiskScanner::finishedTree, this, &MainWindow::onScanDone);
        connect(scanner, &DiskScanner::error, this, &MainWindow::onScanError);
        m_scanner = scanner;
        m_scanner->start();
    }
}

void MainWindow::onScanProgress(const QString& key, const QMap<QString, QString>& args)
{
    // disk.progress.scanning / mft.progress.* — translate and show in status bar.
    // For path-style progress, use the unified "scanning {path}" template so the
    // status bar reads naturally regardless of which scanner emitted it.
    if (key == QLatin1String("disk.progress.scanning")) {
        m_statusLabel->setText(I18n::tr("status.scanning", args));
    } else if (key == QLatin1String("mft.progress.enumerating_usn")
               || key == QLatin1String("mft.progress.reading_mft")
               || key == QLatin1String("mft.progress.building_tree")) {
        // Generic in-progress message; the i18n value itself is a verb phrase.
        m_statusLabel->setText(I18n::tr(key, args));
    } else if (key == QLatin1String("scanner.warn.low_memory")) {
        // Scanner hit the low-memory threshold and will finish with what it has.
        // Remember it so onScanDone can surface a clear notice (the status-bar
        // text alone is transient and easy to miss).
        m_scanLowMemory = true;
        m_statusLabel->setText(I18n::tr("status.low_memory"));
    } else {
        // Records-scanned / mft-parse milestones: translate verbatim.
        m_statusLabel->setText(I18n::tr(key, args));
    }
}

void MainWindow::onScanDone(std::shared_ptr<FileNode> root)
{
    m_scanner = nullptr;
    m_scanBtn->setVisible(true);
    m_cancelBtn->setVisible(false);
    m_progress->setVisible(false);
    m_root = root;
    m_lastScanPath = root->path;
    // Frozen here rather than read on demand: the status bar is re-asserted
    // after later phases (cleanup, duplicate scan) and must keep reporting this
    // scan's duration, not however long the app has been open since.
    m_lastScanMs = m_scanClock.isValid() ? m_scanClock.elapsed() : -1;
    navigateTo(root);

    updateScannedStatus();
    updateDiskFreeLabel(root->path);

    // If the scanner ran out of memory, tell the user clearly instead of
    // leaving them staring at an empty/incomplete tree with no explanation.
    if (m_scanLowMemory) {
        Dialogs::warn(this, APP_NAME, I18n::tr("msg.low_memory"));
        m_scanLowMemory = false;
    }

    // Auto-start cleanup scan on the scanned path.
    startCleanupScan(root->path);
}

void MainWindow::onScanError(const QString& key, const QMap<QString, QString>& args)
{
    m_scanner = nullptr;
    m_scanBtn->setVisible(true);
    m_cancelBtn->setVisible(false);
    m_progress->setVisible(false);
    if (key == QLatin1String("scanner.err.cancelled")) {
        m_statusLabel->setText(I18n::tr("status.cancelled"));
        if (!m_lastScanPath.isEmpty())
            updateDiskFreeLabel(m_lastScanPath);
    } else {
        // Translate the scanner error key into a user-facing message.
        QString msg = I18n::tr(key, args);
        m_statusLabel->setText(I18n::tr("status.scan_failed", QMap<QString, QString>{{"msg", msg}}));
        Dialogs::warn(this, APP_NAME,
            I18n::tr("msg.scan_failed", QMap<QString, QString>{{"msg", msg}}));
    }
}

void MainWindow::onCancel()
{
    if (m_scanner) {
        if (auto* ds = dynamic_cast<DiskScanner*>(m_scanner))
            ds->cancel();
        else if (auto* ms = dynamic_cast<MftScanner*>(m_scanner))
            ms->cancel();
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
    // Clear search when navigating.
    if (m_inSearchMode) {
        m_searchBox->clear();
        m_inSearchMode = false;
        m_searchResults.clear();
    }
    m_current = node;
    m_breadcrumb->setNode(node);
    populateList(node);
    m_treemap->setNode(node, m_showFiles, 1);
    updateStatusForCurrent();

    // P1c: evict off-path subtrees to release memory. Only safe when no
    // cleanup scan/worker is running (they hold m_rootNode and walk the
    // full tree) and we're not in search mode (already cleared above).
    evictOffPathSubtrees();
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

// P1c: Evict off-path subtrees to release memory when navigating into a
// deep node. Walks from m_root down to m_current; for each level, clears
// the children vectors of siblings that are NOT on the active path. The
// sibling nodes themselves remain in their parent's children vector (so
// navigation/sort still work) — only their subtrees are released. If the
// user navigates back to an evicted node, it shows as empty (F5 rescans).
//
// Safety: tryEvictSubtree first walks the subtree checking use_count() > 1
// on every node. If ANY node is held externally (QTreeWidgetItem UserRole,
// TreemapCell, search results, cleanup scanner), the whole subtree is left
// untouched — no dangling references, no crashes.
void MainWindow::evictOffPathSubtrees()
{
    if (!m_evictionEnabled || !m_root || !m_current || m_current == m_root)
        return;
    // Guard: never evict while a cleanup scan/worker is running — they
    // hold m_rootNode and walk the full tree.
    if (m_cleanupScanner || m_cleanupWorker)
        return;
    if (m_inSearchMode)
        return;

    // Build the set of nodes on the active path (m_current up to m_root).
    std::set<FileNode*> activePath;
    for (auto n = m_current; n; n = n->parent.lock()) {
        activePath.insert(n.get());
        if (n.get() == m_root.get())
            break;
    }
    if (activePath.count(m_root.get()) == 0)
        return;  // m_current is not under m_root — shouldn't happen, bail.

    // Walk from root down along the active path. At each level, evict
    // siblings' subtrees (off-path children).
    auto cur = m_root;
    while (cur && cur.get() != m_current.get()) {
        std::shared_ptr<FileNode> nextOnPath;
        for (auto& child : cur->children) {
            if (!child)
                continue;
            if (activePath.count(child.get())) {
                nextOnPath = child;  // descend into this one
            } else {
                tryEvictSubtree(child);
            }
        }
        if (!nextOnPath)
            break;  // active path broken — stop
        cur = nextOnPath;
    }
}

void MainWindow::tryEvictSubtree(std::shared_ptr<FileNode>& node)
{
    if (!node || !node->isDir())
        return;

    // Safety pass: walk the subtree via shared_ptr ref counts and verify every
    // node's use_count == 1 (only held by its parent's children vector). The
    // subtree root (`node`) is skipped — its use_count is >= 2 here (cur's
    // children vector + the `child` ref in the caller's loop), which is
    // expected. If ANY descendant has use_count > 1, something external
    // (QTreeWidgetItem UserRole, TreemapCell, search results, cleanup scanner)
    // holds a reference — abort to avoid dangling pointers.
    bool safe = true;
    std::stack<std::shared_ptr<FileNode>> verify;
    for (auto& c : node->children) {
        if (c) verify.push(c);
    }
    while (!verify.empty()) {
        auto n = verify.top();
        verify.pop();
        if (!n) continue;
        // use_count() == 1 means only the parent's children vector holds it.
        // If > 1, something external (QTreeWidgetItem, TreemapCell, etc.)
        // holds a reference — abort to avoid dangling pointers.
        if (n.use_count() > 1) {
            safe = false;
            break;
        }
        for (auto& c : n->children) {
            if (c) verify.push(c);
        }
    }
    if (!safe)
        return;

    // Second pass: safe to evict. Clear each descendant's children vector
    // and shrink_to_fit to actually release memory.
    std::stack<std::shared_ptr<FileNode>> clear;
    for (auto& c : node->children) {
        if (c) clear.push(c);
    }
    while (!clear.empty()) {
        auto n = clear.top();
        clear.pop();
        if (!n) continue;
        for (auto& c : n->children) {
            if (c) clear.push(c);
        }
        n->children.clear();
        n->children.shrink_to_fit();
    }
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
        m_inSearchMode = false;
        m_searchResults.clear();
        if (m_current)
            populateList(m_current);
        updateStatusForCurrent();
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
    Dialogs::info(this, APP_NAME, I18n::tr("msg.skipped_contents"));
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

void MainWindow::updateScannedStatus()
{
    // Re-assert the "scan complete" summary in the status bar. Used after the
    // cleanup/duplicate scan finishes, since those phases temporarily overwrite
    // the status bar with their own progress text.
    if (!m_root)
        return;
    QMap<QString, QString> args{
        {"path", m_root->path},
        {"size", humanSize(m_root->size)},
        {"files", humanCount(m_root->fileCount)},
        {"folders", humanCount(m_root->dirCount)},
    };
    // The duration is part of the summary, so an entry that has no measured time
    // (a tree restored from cache, say) simply leaves the token out rather than
    // printing a placeholder.
    const QString key = m_lastScanMs >= 0 ? QStringLiteral("status.scanned")
                                          : QStringLiteral("status.scanned_notime");
    if (m_lastScanMs >= 0)
        args.insert(QStringLiteral("time"), humanDuration(m_lastScanMs));
    m_statusLabel->setText(I18n::tr(key, args));
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

    // Search-mode: show flat results with path column.
    if (m_inSearchMode) {
        populateSearchList();
        return;
    }

    if (!node)
        return;

    // Restore normal headers (in case we were in search mode).
    setHeaderLabels();
    m_tree->setColumnHidden(3, false);
    m_tree->setColumnHidden(4, false);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    qint64 total = std::max<qint64>(1, node->size);
    auto children = node->children;

    // Sort using the current column + order.
    children = sortChildren(children);

    // ".." parent entry for ncdu feel.
    auto parent = node->parent.lock();
    if (parent) {
        auto* upItem = new QTreeWidgetItem(QStringList{
            I18n::tr("breadcrumb.parent"), QString(), QString(), QString(), QString(), QString()});
        upItem->setData(0, Qt::UserRole, QVariant::fromValue(parent));
        upItem->setForeground(0, QColor(QString::fromLatin1(C::TEXT_SEC())));
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
        item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC())));
        item->setToolTip(2, I18n::tr("size.bytes", QMap<QString, QString>{{"n", withCommas(c->size)}}));

        double pct = total > 0 ? 100.0 * c->size / total : 0.0;
        item->setText(3, pct >= 0.05 ? QString::number(pct, 'f', 1) + "%" : QString());
        item->setTextAlignment(3, Qt::AlignRight);
        item->setForeground(3, QColor(QString::fromLatin1(C::TEXT_SEC())));

        if (c->isDir())
            item->setText(4, humanCount(c->totalItems()));
        else
            item->setText(4, QStringLiteral("\u2014"));
        item->setTextAlignment(4, Qt::AlignRight);
        item->setForeground(4, QColor(QString::fromLatin1(C::TEXT_MUTED())));

        item->setText(5, I18n::tr(typeKey(c->name, c->isDir())));
        item->setForeground(5, QColor(QString::fromLatin1(C::TEXT_MUTED())));

        if (c->error != NodeError::None)
            item->setForeground(0, QColor(QString::fromLatin1(C::TEXT_MUTED())));
        if (c->skipped) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, QColor(QString::fromLatin1(C::TEXT_SEC())));
        }

        m_tree->addTopLevelItem(item);
    }

    m_tree->header()->setSectionsClickable(true);
    reflectSortIndicator();
}

// --------------------------------------------------------------------------- //
// Search-mode: flat list with path column ("Everything-like")
// --------------------------------------------------------------------------- //
void MainWindow::populateSearchList()
{
    if (m_searchResults.empty())
        return;

    // Set headers for search mode: Name | Path | Size | % | Items | Type
    // Columns 3(%) and 4(Items) are hidden in search mode.
    QStringList labels = {
        I18n::tr("column.name"),
        I18n::tr("column.path"),
        I18n::tr("column.size"),
        QString(),
        QString(),
        I18n::tr("column.type"),
    };
    m_tree->setHeaderLabels(labels);
    m_tree->setColumnHidden(3, true);
    m_tree->setColumnHidden(4, true);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->resizeSection(1, 200);

    auto sorted = sortChildren(m_searchResults);
    for (const auto& c : sorted) {
        auto* item = new QTreeWidgetItem;

        // Column 0: Name
        item->setText(0, c->name);
        item->setData(0, Qt::UserRole, QVariant::fromValue(c));
        item->setIcon(0, typeIcon(c));
        item->setToolTip(0, buildRowTooltip(c));

        // Column 1: Path (relative to search root) — no BAR_ROLE, so delegate shows text.
        auto searchRoot = m_current ? m_current : m_root;
        QString relPath = c->path;
        if (searchRoot && c->path.startsWith(searchRoot->path, Qt::CaseInsensitive)) {
            relPath = c->path.mid(searchRoot->path.length());
            if (relPath.startsWith('/') || relPath.startsWith('\\'))
                relPath = relPath.mid(1);
        }
        item->setText(1, relPath);
        item->setToolTip(1, c->path);
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED())));

        // Column 2: Size
        item->setText(2, humanSize(c->size));
        item->setTextAlignment(2, Qt::AlignRight);
        item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC())));
        item->setToolTip(2, I18n::tr("size.bytes", QMap<QString, QString>{{"n", withCommas(c->size)}}));

        // Column 5: Type
        item->setText(5, I18n::tr(typeKey(c->name, c->isDir())));
        item->setForeground(5, QColor(QString::fromLatin1(C::TEXT_MUTED())));

        if (c->skipped) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, QColor(QString::fromLatin1(C::TEXT_SEC())));
        }

        m_tree->addTopLevelItem(item);
    }

    m_tree->header()->setSectionsClickable(true);
    reflectSortIndicator();
}

void MainWindow::onSearchDebounceTimeout()
{
    auto searchRoot = m_current ? m_current : m_root;
    if (!searchRoot || m_searchText.isEmpty())
        return;

    QString q = m_searchText.toLower();
    m_searchQueryPending = m_searchText;

    // Run the search on a background thread so typing stays responsive.
    auto future = QtConcurrent::run([searchRoot, q]() {
        std::vector<std::shared_ptr<FileNode>> results;
        int limit = 500;

        bool hasWildcard = q.contains(QLatin1Char('*'));
        QRegularExpression re;
        if (hasWildcard) {
            re.setPattern(QRegularExpression::wildcardToRegularExpression(q));
            re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
        }
        const QString lowerQuery = hasWildcard ? QString() : q;

        // Iterative DFS (explicit node stack) — avoids C++ stack overflow on
        // deeply nested directory trees. The limit check stays inside the loop.
        std::stack<std::shared_ptr<FileNode>> pending;
        pending.push(searchRoot);
        while (!pending.empty() && limit > 0) {
            auto n = pending.top();
            pending.pop();
            if (!n)
                continue;
            for (const auto& child : n->children) {
                if (limit <= 0)
                    break;
                bool match = hasWildcard
                    ? re.match(child->name).hasMatch()
                    : child->name.toLower().contains(lowerQuery);
                if (match) {
                    results.push_back(child);
                    --limit;
                }
                if (child->isDir())
                    pending.push(child);
            }
        }
        return results;
    });

    m_searchWatcher->setFuture(future);
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
    if (node->error != NodeError::None) {
        lines << QString();
        lines << I18n::tr("properties.note") + ": " + node->errorText();
    }
    return lines.join("\n");
}

std::vector<std::shared_ptr<FileNode>> MainWindow::sortChildren(
    const std::vector<std::shared_ptr<FileNode>>& children) const
{
    QString key = sortKeyForColumn(m_sortCol, m_inSearchMode);
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
    } else if (key == "path") {
        auto cmp = [&](const std::shared_ptr<FileNode>& a,
                       const std::shared_ptr<FileNode>& b) {
            QString pathA = a->path;
            QString pathB = b->path;
            if (m_root) {
                pathA = a->path.mid(m_root->path.length());
                pathB = b->path.mid(m_root->path.length());
            }
            int r = pathA.compare(pathB, Qt::CaseInsensitive);
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
    if (m_inSearchMode) {
        int n = static_cast<int>(m_searchResults.size());
        m_statusLabel->setText(I18n::tr("status.search_results", QMap<QString, QString>{
            {"n", QString::number(n)},
            {"query", m_searchText},
        }));
        return;
    }
    auto n = m_current;
    if (!n)
        return;
    m_statusLabel->setText(I18n::tr("status.current", QMap<QString, QString>{
        {"path", n->path},
        {"size", humanSize(n->size)},
        {"files", humanCount(n->fileCount)},
        {"folders", humanCount(n->dirCount)},
    }));
    if (m_actions.contains("ai.analyze_current"))
        m_actions["ai.analyze_current"]->setEnabled(true);
}

// --------------------------------------------------------------------------- //
// Search / sort
// --------------------------------------------------------------------------- //
void MainWindow::onSearchChanged(const QString& text)
{
    m_searchText = text.trimmed();
    if (m_searchText.isEmpty()) {
        m_searchDebounceTimer->stop();
        m_inSearchMode = false;
        m_searchResults.clear();
        m_searchQueryPending.clear();
        if (m_current)
            populateList(m_current);
        updateStatusForCurrent();
        return;
    }

    m_inSearchMode = true;
    // Restart debounce timer — search only fires after the user stops typing.
    m_searchDebounceTimer->start();
}

void MainWindow::onHeaderClicked(int logicalIndex)
{
    if (m_inSearchMode) {
        static const QMap<int, Qt::SortOrder> searchDefaults = {
            {0, Qt::AscendingOrder},
            {1, Qt::AscendingOrder},
            {2, Qt::DescendingOrder},
            {5, Qt::AscendingOrder},
        };
        if (logicalIndex == m_sortCol) {
            m_sortOrder = (m_sortOrder == Qt::AscendingOrder)
                              ? Qt::DescendingOrder
                              : Qt::AscendingOrder;
        } else {
            m_sortCol = logicalIndex;
            m_sortOrder = searchDefaults.value(logicalIndex, Qt::DescendingOrder);
        }
        reflectSortIndicator();
        if (m_current)
            populateList(m_current);
        return;
    }

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

    QAction* actMove = menu->addAction(I18n::tr("ctx.move_to"));
    connect(actMove, &QAction::triggered, this, [this, node]() { moveToOtherDrive(node); });

    menu->addSeparator();

    QAction* actRecycle = menu->addAction(I18n::tr("ctx.recycle"));
    connect(actRecycle, &QAction::triggered, this, QOverload<>::of(&MainWindow::recycleSelected));

    QAction* actPerm = menu->addAction(I18n::tr("ctx.delete_permanent"));
    connect(actPerm, &QAction::triggered, this, QOverload<>::of(&MainWindow::deletePermanentSelected));

    menu->addSeparator();

    QAction* actSync = menu->addAction(I18n::tr("app_sync.menu"));
    connect(actSync, &QAction::triggered, this, &MainWindow::showAppPathSync);

    QAction* actProp = menu->addAction(I18n::tr("ctx.properties"));
    connect(actProp, &QAction::triggered, this, [this, node]() { showProperties(node); });

    menu->addSeparator();

    QAction* actAi = menu->addAction(I18n::tr("ctx.ai_analyze"));
    connect(actAi, &QAction::triggered, this,
            [this, node]() { runAiAnalysis(node->name, buildDirPrompt(node)); });

    menu->exec(QCursor::pos());
    delete menu;
}

void MainWindow::moveToOtherDrive(const std::shared_ptr<FileNode>& node)
{
    if (!node || node->parent.expired()) {
        Dialogs::warn(this, APP_NAME, I18n::tr("dialog.delete.warn_root"));
        return;
    }
    // The Windows directory itself is off limits for every direct action; the
    // cleanup categories are the only route into it.
    if (WinApi::isWindowsRoot(node->path)) {
        Dialogs::warn(this, APP_NAME, I18n::tr("dialog.protect.windows"));
        return;
    }

    // Determine default move directory from QSettings or another drive
    QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\NcduWin"), QSettings::NativeFormat);
    QString defaultDir = settings.value(QStringLiteral("defaultMovePath")).toString();
    if (defaultDir.isEmpty() || !QDir(defaultDir).exists()) {
        QString curDrive = node->path.left(2).toUpper();
        const QStringList drives = listDrives();
        QString otherDrive;
        for (const auto& d : drives) {
            if (!d.startsWith(curDrive, Qt::CaseInsensitive)) {
                otherDrive = d;
                break;
            }
        }
        if (otherDrive.isEmpty()) {
            defaultDir = QDir::homePath() + QStringLiteral("/NcduWinMoved");
        } else {
            defaultDir = otherDrive + QStringLiteral("NcduWinMoved");
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(I18n::tr("move.title"));
    dlg.setMinimumWidth(500);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(12);

    auto* title = new QLabel(I18n::tr("move.title"));
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    lay->addWidget(title);

    auto* tip = new QLabel(I18n::tr("move.tip"));
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; background: %2; border: 1px solid %3; border-radius: 6px; padding: 8px;")
        .arg(QString::fromLatin1(C::TEXT_SEC()), QString::fromLatin1(C::SURFACE_ALT()), QString::fromLatin1(C::BORDER())));
    lay->addWidget(tip);

    auto* srcLabel = new QLabel(QStringLiteral("<b>%1</b> %2 (%3)")
        .arg(I18n::tr("move.source"), node->path, humanSize(node->size)));
    srcLabel->setWordWrap(true);
    lay->addWidget(srcLabel);

    auto* targetRow = new QHBoxLayout;
    auto* targetLabel = new QLabel(I18n::tr("move.target"));
    auto* targetEdit = new QLineEdit(defaultDir);
    auto* browseBtn = new QPushButton(I18n::tr("button.browse"));
    browseBtn->setCursor(Qt::PointingHandCursor);
    connect(browseBtn, &QPushButton::clicked, &dlg, [&dlg, targetEdit]() {
        QString dir = QFileDialog::getExistingDirectory(&dlg, I18n::tr("move.select_folder"), targetEdit->text());
        if (!dir.isEmpty()) targetEdit->setText(QDir::toNativeSeparators(dir));
    });
    targetRow->addWidget(targetLabel);
    targetRow->addWidget(targetEdit, 1);
    targetRow->addWidget(browseBtn);
    lay->addLayout(targetRow);

    auto* chkDefault = new QCheckBox(I18n::tr("move.set_default"));
    chkDefault->setChecked(true);
    lay->addWidget(chkDefault);

    lay->addSpacing(8);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* cancelBtn = new QPushButton(I18n::tr("button.cancel"));
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    auto* startBtn = new QPushButton(I18n::tr("move.btn_start"));
    startBtn->setObjectName(QStringLiteral("primary"));
    startBtn->setCursor(Qt::PointingHandCursor);
    connect(startBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(startBtn);
    lay->addLayout(btnRow);

    if (dlg.exec() != QDialog::Accepted)
        return;

    QString chosenDir = targetEdit->text().trimmed();
    if (chosenDir.isEmpty())
        return;

    if (chkDefault->isChecked()) {
        settings.setValue(QStringLiteral("defaultMovePath"), chosenDir);
    }
    m_lastMoveTarget = chosenDir;

    // A move renames the folder aside and links it back, and a rename fails for
    // as long as any program holds a file inside it. Learning that after the copy
    // means throwing the copy away and starting over, so the question is asked
    // here — before a single byte moves, where answering it costs the user
    // nothing at all.
    guardThen(QStringList{node->path}, /*fatal=*/true,
              [this, node, chosenDir]() { startSafeMove(node, chosenDir); });
}

void MainWindow::startSafeMove(const std::shared_ptr<FileNode>& node,
                               const QString& targetDir)
{
    auto* progress = new QProgressDialog(I18n::tr("move.progress_title"), I18n::tr("button.cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowTitle(I18n::tr("move.title"));
    progress->setMinimumDuration(0);
    progress->setValue(0);

    auto* worker = new SafeMoveWorker(node->path, targetDir, this);
    connect(worker, &SafeMoveWorker::progress, progress, [progress](int pct, qint64 copied, qint64 total, const QString& file) {
        progress->setValue(pct);
        progress->setLabelText(QStringLiteral("[%1% - %2 / %3]\n%4")
            .arg(pct).arg(humanSize(copied)).arg(humanSize(total)).arg(file));
    });
    connect(worker, &SafeMoveWorker::statusMessage, progress, [progress](const QString& msg) {
        progress->setLabelText(msg);
    });
    connect(progress, &QProgressDialog::canceled, worker, [worker]() {
        worker->cancel();
    });

    connect(worker, &QThread::finished, this, [this, worker, progress, node]() {
        progress->close();
        progress->deleteLater();

        if (worker->isSuccess()) {
            QString finalDst = worker->finalDestPath();
            afterDelete({node});
            if (m_root)
                updateDiskFreeLabel(m_root->path);

            QMessageBox box(this);
            box.setIcon(QMessageBox::Information);
            box.setWindowTitle(I18n::tr("move.title"));
            box.setText(I18n::tr("move.success", QMap<QString, QString>{{"path", finalDst}}));
            box.setInformativeText(I18n::tr("move.sync_ask"));
            auto* syncBtn = box.addButton(I18n::tr("app_sync.menu"), QMessageBox::AcceptRole);
            box.addButton(I18n::tr("button.close"), QMessageBox::RejectRole);
            box.setDefaultButton(qobject_cast<QPushButton*>(syncBtn));
            box.exec();

            if (box.clickedButton() == syncBtn) {
                showAppPathSync();
            }
        } else {
            Dialogs::warn(this, I18n::tr("move.title"),
                I18n::tr("move.failed", QMap<QString, QString>{{"msg", worker->errorString()}}));
        }
        worker->deleteLater();
    });

    worker->start();
}


void MainWindow::showAppPathSync()
{
    // Shown non-modally: picking a target drive, listing a hundred thousand data
    // folders and copying tens of gigabytes is a long business, and there is no
    // reason for it to block scanning, cleaning up or minimising the main
    // window. Only closing the main window waits for it (see closeEvent).
    if (m_syncDialog) {
        if (m_syncDialog->isMinimized())
            m_syncDialog->setWindowState(m_syncDialog->windowState() & ~Qt::WindowMinimized);
        m_syncDialog->show();
        m_syncDialog->raise();
        m_syncDialog->activateWindow();
        return;
    }

    // The move tab is created once and outlives the dialog: a folder with tens of
    // gigabytes in it takes far longer to copy than a dialog stays open, and the
    // work has to keep going (with its progress) after the user closes it.
    if (!m_movePanel)
        m_movePanel = new AppDataMovePanel(this);

    auto* dlg = new AppPathSyncDialog(this, m_lastMoveTarget, m_movePanel);
    m_syncDialog = dlg;
    // Deletes itself on close; m_syncDialog goes null with it, because QPointer
    // watches the object rather than this window's destruction order.
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
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
        if (n->parent.expired() || WinApi::isWindowsRoot(n->path)) {
            // Top-level drive/folder, and the Windows directory itself: the only
            // thing allowed to touch what is inside it is the cleanup categories.
            rejected.push_back(n);
        } else {
            deletable.push_back(n);
        }
    }
}

// True when the refusal was about a protected system folder rather than a plain
// top-level entry, so the user gets the matching explanation.
static bool hasProtectedSystemDir(const std::vector<std::shared_ptr<FileNode>>& rejected)
{
    for (const auto& n : rejected) {
        if (n && WinApi::isWindowsRoot(n->path))
            return true;
    }
    return false;
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
        Dialogs::warn(this, APP_NAME,
            I18n::tr(hasProtectedSystemDir(rejected) ? "dialog.protect.windows"
                                                     : "dialog.delete.warn_root"));
        if (nodes.empty())
            return;
    }

    QString names, more;
    formatNames(nodes, names, more);
    const bool confirmed = Dialogs::confirm(this, I18n::tr("dialog.recycle.title"),
        I18n::tr("dialog.recycle.body", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()))},
            {"names", names},
            {"more", more},
        }));
    if (!confirmed)
        return;

    QStringList paths;
    for (const auto& n : nodes)
        paths << n->path;

    // Ask before anything moves: is a program holding one of these open? A
    // Recycle Bin operation that stops halfway leaves some items moved and some
    // still there, and the user is left to work out which by hand. The question
    // is asked here, where closing the holder still costs nothing at all.
    guardThen(paths, /*fatal=*/true, [this, nodes]() { doRecycle(nodes); });
}

// The Recycle Bin half of the job, run once the pre-flight has cleared the way.
void MainWindow::doRecycle(const std::vector<std::shared_ptr<FileNode>>& nodes)
{
    QStringList paths;
    for (const auto& n : nodes)
        paths << n->path;

    // Run in the background, one path at a time, so the dialog can show a real
    // percentage and the item being worked on — an indeterminate spinner over a
    // big selection reads exactly like a freeze.
    const int n = static_cast<int>(nodes.size());
    auto* progress = new QProgressDialog(
        I18n::tr("dialog.recycle.progress", QMap<QString, QString>{{"n", QString::number(n)}}),
        I18n::tr("button.cancel"), 0, n, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowTitle(I18n::tr("dialog.recycle.title"));
    progress->setMinimumDuration(0);
    progress->setValue(0);

    const auto cancelledFlag = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, progress,
            [cancelledFlag]() { cancelledFlag->store(true); });

    auto* watcher = new QFutureWatcher<QVector<WinApi::DeleteResult>>(this);
    auto nodeCopy = nodes;
    connect(watcher, &QFutureWatcher<QVector<WinApi::DeleteResult>>::finished, this,
            [this, nodeCopy, n, progress, watcher, cancelledFlag]() {
                progress->deleteLater();
                const QVector<WinApi::DeleteResult> results = watcher->result();
                watcher->deleteLater();

                // Whatever happened, the tree is brought in line with the disk
                // first: some items may have moved and others not at all, and the
                // sizes on screen still describe the state before.
                resyncNodesFromDisk(nodeCopy);
                if (m_root)
                    updateDiskFreeLabel(m_root->path);

                if (cancelledFlag->load()) {
                    // Whatever was already recycled stays recycled; the rest
                    // was deliberately left untouched.
                    m_statusLabel->setText(I18n::tr("dialog.recycle.cancelled"));
                    return;
                }

                const OpGuard::Tally t = OpGuard::tally(results);
                if (t.bad == 0) {
                    m_statusLabel->setText(I18n::tr("status.moved_recycle",
                        QMap<QString, QString>{{"n", QString::number(n)}}));
                    return;
                }

                // Offer permanent deletion only where it can help. The Recycle
                // Bin refuses for reasons permanent deletion does not care about
                // (an item too big for the bin, a path past the length limit), and
                // for items that are simply not there any more there is nothing
                // left to offer at all.
                if (OpGuard::onlyAlreadyGone(results)) {
                    Dialogs::info(this, I18n::tr("op_result.title"),
                                  OpGuard::resultBody(results, /*recycled=*/true));
                    return;
                }
                if (Dialogs::confirm(this, I18n::tr("dialog.recycle.fallback_title"),
                                     I18n::tr("dialog.recycle.fallback_body",
                                              QMap<QString, QString>{
                                                  {"lines",
                                                   OpGuard::resultLines(results, true)}}))) {
                    deletePermanentAsync(nodeCopy);
                }
            });

    watcher->setFuture(QtConcurrent::run([paths, progress, cancelledFlag]() {
        QVector<WinApi::DeleteResult> results;
        const int total = paths.size();
        for (int i = 0; i < total; ++i) {
            if (cancelledFlag->load())
                break;
            QMetaObject::invokeMethod(progress, [progress, i, total, path = paths.at(i)]() {
                progress->setValue(i);
                progress->setLabelText(I18n::tr("dialog.recycle.progress_item",
                    QMap<QString, QString>{{"done", QString::number(i + 1)},
                                           {"total", QString::number(total)},
                                           {"path", path}}));
            }, Qt::QueuedConnection);

            WinApi::DeleteResult r;
            r.path = paths.at(i);
            if (!QFileInfo::exists(r.path)) {
                // The list the user was looking at no longer matches the disk.
                // Reporting that as a failure would be both wrong and useless.
                r.gone = true;
                r.reason = WinApi::DeleteReason::Missing;
                results << r;
                continue;
            }
            int shellCode = 0;
            WinApi::sendToRecycleBin(QStringList{r.path}, &shellCode);
            r.gone = !QFileInfo::exists(r.path);
            if (!r.gone) {
                r.winError = static_cast<quint32>(shellCode);
                r.reason = WinApi::classifyShellError(shellCode);
                if (r.reason == WinApi::DeleteReason::None)
                    r.reason = WinApi::DeleteReason::Unknown;
            }
            results << r;
        }
        return results;
    }));
}


void MainWindow::deletePermanentSelected()
{
    std::vector<std::shared_ptr<FileNode>> nodes, rejected;
    collectDeletable(nodes, rejected);
    if (nodes.empty() && rejected.empty())
        return;
    if (!rejected.empty()) {
        Dialogs::warn(this, APP_NAME,
            I18n::tr(hasProtectedSystemDir(rejected) ? "dialog.protect.windows"
                                                     : "dialog.delete.warn_root"));
        if (nodes.empty())
            return;
    }

    QString names, more;
    formatNames(nodes, names, more);
    const bool confirmed = Dialogs::confirm(this, I18n::tr("dialog.delete.title"),
        I18n::tr("dialog.delete.body", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(nodes.size()))},
            {"names", names},
            {"more", more},
        }));
    if (!confirmed)
        return;

    deletePermanentAsync(nodes);
}

void MainWindow::deletePermanentAsync(
    const std::vector<std::shared_ptr<FileNode>>& nodes)
{
    QStringList paths;
    for (const auto& n : nodes)
        paths << n->path;

    // The same question the Recycle Bin path asks, and it matters more here:
    // permanent deletion does not fail as a unit. It removes everything it can
    // reach and leaves the rest, so a folder nobody can account for — half its
    // files gone, still sitting in the row at its old size — is the usual outcome
    // of asking while a program has one of those files open.
    guardThen(paths, /*fatal=*/true,
              [this, nodes]() { doDeletePermanent(nodes); });
}

void MainWindow::doDeletePermanent(
    const std::vector<std::shared_ptr<FileNode>>& nodes)
{
    QStringList paths;
    for (const auto& n : nodes)
        paths << n->path;

    const int n = static_cast<int>(nodes.size());
    auto* progress = new QProgressDialog(
        I18n::tr("dialog.delete.progress", QMap<QString, QString>{{"n", QString::number(n)}}),
        I18n::tr("button.cancel"), 0, n, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowTitle(I18n::tr("dialog.delete.title"));
    progress->setMinimumDuration(0);
    progress->setValue(0);

    const auto cancelledFlag = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, progress,
            [cancelledFlag]() { cancelledFlag->store(true); });

    auto* watcher = new QFutureWatcher<QVector<WinApi::DeleteResult>>(this);
    connect(watcher, &QFutureWatcher<QVector<WinApi::DeleteResult>>::finished, this,
            [this, nodes, progress, watcher, cancelledFlag]() {
                progress->deleteLater();
                const QVector<WinApi::DeleteResult> results = watcher->result();
                watcher->deleteLater();

                // Before anything is said about the outcome, the tree has to
                // match the disk. Which rows survived and how big they are now is
                // not something the result list can answer on its own — a folder
                // that lost most of its files is still the same folder.
                resyncNodesFromDisk(nodes);
                if (m_root)
                    updateDiskFreeLabel(m_root->path);

                if (cancelledFlag->load()) {
                    m_statusLabel->setText(I18n::tr("dialog.delete.cancelled"));
                    return;
                }

                const OpGuard::Tally t = OpGuard::tally(results);
                m_statusLabel->setText(t.bad == 0
                    ? I18n::tr("status.deleted",
                               QMap<QString, QString>{{"n", QString::number(t.ok)}})
                    : I18n::tr("op_result.status_partial",
                               QMap<QString, QString>{{"ok", QString::number(t.ok)},
                                                      {"freed", humanSize(t.freed)},
                                                      {"bad", QString::number(t.bad)}}));

                if (t.bad > 0) {
                    // Which item is still there, and why. "部分项目无法删除" was the
                    // whole message before, and it named neither.
                    Dialogs::info(this, I18n::tr("op_result.title"),
                                  OpGuard::resultBody(results, /*recycled=*/false));
                }
            });

    watcher->setFuture(QtConcurrent::run([paths, progress, cancelledFlag]() {
        QVector<WinApi::DeleteResult> results;
        const int total = paths.size();
        for (int i = 0; i < total; ++i) {
            if (cancelledFlag->load())
                break;
            QMetaObject::invokeMethod(progress, [progress, i, total, path = paths.at(i)]() {
                progress->setValue(i);
                progress->setLabelText(I18n::tr("dialog.delete.progress_item",
                    QMap<QString, QString>{{"done", QString::number(i + 1)},
                                           {"total", QString::number(total)},
                                           {"path", path}}));
            }, Qt::QueuedConnection);
            const QVector<WinApi::DeleteResult> one =
                WinApi::deletePermanentDetailed(QStringList{paths.at(i)});
            if (!one.isEmpty())
                results << one.first();
        }
        return results;
    }));
}


std::shared_ptr<FileNode> MainWindow::findNodeByPath(const QString& path) const
{
    if (!m_root)
        return nullptr;
    // The relative part, rather than a prefix built here. A whole-drive scan
    // has the root "D:/", and comparing a path against that plus a separator
    // asks about "d://" — which nothing is ever under. Every lookup came back
    // empty, so a deleted row was never corrected: it stayed on screen at its
    // old size, and deleting it again could only report it as already gone.
    QString rel;
    if (!WinApi::splitInside(path, m_root->path, &rel))
        return nullptr;
    if (rel.isEmpty())
        return m_root;

    auto node = m_root;
    auto parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
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

// --------------------------------------------------------------------------- //
// Keep the tree honest about what is on the disk
// --------------------------------------------------------------------------- //

// Deleting only partly works all the time — one file open in some program, a
// folder Windows still holds a handle on — and the old refresh only ran when the
// operation reported complete success. A partly deleted folder therefore kept
// whatever size the last scan had measured, a number that now described files
// that were gone. So the answer comes from the disk: drop the rows that are no
// longer there, and re-measure the ones that are.
//
// Called after *every* destructive operation, successful or not, because "some
// of it worked" is the case that needs it most.
//
// Two ways in, one body. Deleting and recycling already hold the very nodes
// they worked on, so they hand those over and nothing can fail to resolve on the
// way; the cleanup categories report paths, and those get looked up.
void MainWindow::resyncPathsFromDisk(const QStringList& paths)
{
    if (paths.isEmpty())
        return;

    std::vector<std::shared_ptr<FileNode>> nodes;
    for (const QString& path : paths) {
        if (auto node = findNodeByPath(path))
            nodes.push_back(node);
    }
    resyncNodesFromDisk(nodes);
}

void MainWindow::resyncNodesFromDisk(
    const std::vector<std::shared_ptr<FileNode>>& nodes)
{
    if (nodes.empty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_statusLabel->setText(I18n::tr("status.resyncing"));

    // Nodes that have to have their total added up again because a child of
    // theirs changed. Nodes measured straight from the disk are excluded from
    // that: recomputing one of those from its children would put back the very
    // staleness this exists to remove, since a child's size is itself a number
    // from the last scan.
    std::set<FileNode*> measured;
    std::set<FileNode*> seen;
    std::set<FileNode*> handled;
    std::vector<std::shared_ptr<FileNode>> propagate;

    auto markPropagate = [&propagate, &seen](const std::shared_ptr<FileNode>& n) {
        if (n && seen.insert(n.get()).second)
            propagate.push_back(n);
    };

    for (const auto& node : nodes) {
        if (!node || !handled.insert(node.get()).second)
            continue;   // already detached from the tree, or named twice
        auto parent = node->parent.lock();

        if (!QFileInfo::exists(node->path)) {
            // Gone: the row goes with it. This covers both "this operation
            // removed it" and "the row described a path that was already gone".
            if (parent) {
                auto it = std::find(parent->children.begin(), parent->children.end(), node);
                if (it != parent->children.end())
                    parent->children.erase(it);
                markPropagate(parent);
            }
            continue;
        }
        if (!node->isDir())
            continue;

        // Still there, and at most partly deleted. What is under it right now is
        // the only honest size for the row.
        pruneMissingFromNode(node);
        const WinApi::DirStat stat = WinApi::dirStatNoReparse(node->path);
        node->size = stat.size;
        node->fileCount = stat.fileCount;
        node->dirCount = stat.dirCount;
        measured.insert(node.get());
        markPropagate(parent);
    }

    for (const auto& node : propagate) {
        if (!measured.count(node.get()))
            recomputeSizes(node);
    }

    QApplication::restoreOverrideCursor();

    if (m_current)
        navigateTo(m_current);
}

// --------------------------------------------------------------------------- //
// Pre-flight: who is holding what we are about to delete, recycle or move
// --------------------------------------------------------------------------- //
// Deleting used to be a one-way street. If a program had a file open, the delete
// either half-succeeded — everything but that file went, and the folder stayed —
// or failed with a sentence that named nobody. Both are the same missing
// question, asked too late; the software-data panel has been asking it up front
// all along. This is that question, for the operations that never asked it.
//
//   * Fatal    — deleting and moving. If the holders cannot be closed, nothing
//                is touched at all: half a delete is worse than no delete, since
//                it leaves a folder the user cannot account for.
//   * Advisory — cleanup. Skipping a file a program is using is normal there
//                (Temp and caches are never idle), so the holders are named and
//                the run goes ahead.

void MainWindow::guardThen(const QStringList& paths, bool fatal,
                           std::function<void()> action)
{
    if (m_guardWatcher) {
        // One guarded operation at a time: two probes in flight would leave the
        // second one's answer attached to the first one's action.
        m_statusLabel->setText(I18n::tr("op_guard.busy"));
        return;
    }
    m_guardPaths = paths;
    m_guardFatal = fatal;
    m_guardAction = std::move(action);
    m_guardOffered = false;
    m_guardClosedImages.clear();
    runGuardProbe();
}

void MainWindow::runGuardProbe()
{
    // A progress dialog that only appears when the check is slow. Empty folders
    // and small ones answer at once, and flashing a dialog before every delete
    // would be worse than the pause it is there to explain.
    //
    // It is deliberately not a member: it lives exactly as long as the probe
    // does, so there is nothing about it that could go stale. The finished
    // handler holds it (by QPointer, so a teardown that takes the window along
    // cannot leave the capture dangling), which is the whole of what keeps it
    // alive — a cached member would only be one more thing retranslateUI had to
    // remember, for a window the user cannot even reach past its own modality.
    QPointer<QProgressDialog> progress =
        new QProgressDialog(I18n::tr("op_guard.checking"),
                            QString(), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowTitle(I18n::tr("op_guard.title"));
    progress->setMinimumDuration(350);
    progress->setCancelButton(nullptr);

    m_guardWatcher = new QFutureWatcher<QVector<WinApi::LockingProcess>>(this);
    const QStringList paths = m_guardPaths;
    connect(m_guardWatcher, &QFutureWatcher<QVector<WinApi::LockingProcess>>::finished,
            this, [this, progress]() {
                // Closed before the answer is dealt with: the answer may raise
                // the "these programs are using this folder" prompt, and two
                // windows stacking here would make the second look like a
                // reaction to the first.
                if (progress) {
                    progress->close();
                    progress->deleteLater();
                }
                onGuardProbed();
            });
    m_guardWatcher->setFuture(QtConcurrent::run(
        [paths]() { return OpGuard::probe(paths); }));
}

void MainWindow::onGuardProbed()
{
    const QVector<WinApi::LockingProcess> procs = m_guardWatcher->result();
    m_guardWatcher->deleteLater();
    m_guardWatcher = nullptr;

    if (OpGuard::blocksOperation(procs)) {
        const int closable = LockerDialog::closableCount(procs);

        if (closable > 0 && !m_guardOffered) {
            // Ask once. The offer is worth making exactly while closing the
            // programs costs the user nothing, which is now — nothing has been
            // deleted and nothing has been copied.
            const LockerDialog::Answer answer = LockerDialog::ask(
                this, I18n::tr("op_guard.title"),
                I18n::tr("op_guard.intro"),
                procs,
                I18n::tr("op_guard.btn_close"),
                QString(),                 /*skipText unused*/
                /*allowSkip=*/false);
            if (answer != LockerDialog::CloseAndContinue) {
                // "Not now". Nothing was touched, so there is nothing to explain.
                finishGuard(false);
                return;
            }

            m_guardOffered = true;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const QVector<WinApi::CloseOutcome> outcomes = WinApi::closeProcesses(procs);
            QApplication::restoreOverrideCursor();

            // Remember what actually went away, by image path: the only thing
            // that survives a restart, and therefore the only way to tell "a
            // program is holding it" apart from "a program restarts itself".
            for (const WinApi::CloseOutcome& o : outcomes) {
                if (o.result == WinApi::CloseOutcome::Survived)
                    continue;
                for (const WinApi::LockingProcess& p : procs) {
                    if (p.pid != o.pid)
                        continue;
                    const QString key = p.exePath.isEmpty() ? p.name : p.exePath;
                    if (!key.isEmpty() && !m_guardClosedImages.contains(key))
                        m_guardClosedImages << key;
                    break;
                }
            }
            const QString report = LockerDialog::outcomeText(outcomes);
            if (!report.isEmpty()) {
                Logger::info(QStringLiteral("[guard] programs holding %1:\n%2")
                                 .arg(m_guardPaths.join(QStringLiteral(", ")), report));
            }

            // Ask the disk again rather than assume. A program that came back is
            // a program that will come back, and that is worth saying instead of
            // discovering it halfway through a delete.
            runGuardProbe();
            return;
        }

        // Still held. Either nothing could be closed, or closing was already
        // tried — and either way the user needs to know who is in the way, since
        // "被占用" on its own gives them nowhere to go.
        const QStringList names = OpGuard::namesOf(procs);
        if (m_guardFatal) {
            const QStringList back = WinApi::respawnedAmong(procs, m_guardClosedImages);
            const QString body =
                !back.isEmpty()
                    ? I18n::tr("op_guard.blocked_respawn",
                               QMap<QString, QString>{{"procs",
                                                       back.join(QStringLiteral("、"))}})
                    : (closable == 0
                           ? I18n::tr("op_guard.blocked_noclose",
                                      QMap<QString, QString>{
                                          {"procs", names.join(QStringLiteral("、"))}})
                           : I18n::tr("op_guard.blocked_body",
                                      QMap<QString, QString>{
                                          {"procs", names.join(QStringLiteral("、"))}}));
            Dialogs::warn(this, I18n::tr("op_guard.blocked_title"), body);
            finishGuard(false);
            return;
        }

        // Advisory: name them, then do what can be done. Refusing the whole
        // cleanup over a file a program happens to be holding would leave the
        // user with nothing at all.
        Logger::info(QStringLiteral("[guard] cleaning anyway, held by: %1")
                         .arg(names.join(QStringLiteral(", "))));
        m_statusLabel->setText(I18n::tr("op_guard.advisory",
                                        QMap<QString, QString>{
                                            {"procs", names.join(QStringLiteral("、"))}}));
    }

    finishGuard(true);
}

void MainWindow::finishGuard(bool proceed)
{
    std::function<void()> action = std::move(m_guardAction);
    m_guardAction = nullptr;
    m_guardPaths.clear();
    m_guardClosedImages.clear();
    m_guardOffered = false;
    if (proceed && action)
        action();
}


void MainWindow::syncTreeAfterCleanup(const std::vector<CleanupWorker::ItemRef>& items)
{
    // A cleanup reports items, not paths, and both lists matter here: a category
    // that was cleaned can still hold files a program had open, and those are
    // exactly the ones that change what its row should say.
    QStringList touched;
    for (const auto& item : items)
        touched << item.path;
    resyncPathsFromDisk(touched);
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
                             .arg(QString::fromLatin1(C::FG())));
    lay->addWidget(title);
    lay->addSpacing(4);

    auto addRow = [&](const QString& labelKey, const QString& value) {
        auto* rowW = new QWidget;
        auto* rlay = new QHBoxLayout(rowW);
        rlay->setContentsMargins(0, 0, 0, 0);
        rlay->setSpacing(12);
        auto* lab = new QLabel(I18n::tr(labelKey));
        lab->setStyleSheet(QStringLiteral("color: %1; min-width: 90px;")
                               .arg(QString::fromLatin1(C::TEXT_MUTED())));
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

    QFileInfo fi(node->path);
    if (fi.exists()) {
        auto fmtTime = [](const QDateTime& dt) -> QString {
            return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")) : QStringLiteral("-");
        };
        QDateTime birth = fi.birthTime();
        if (birth.isValid())
            addRow("properties.created", fmtTime(birth));
        QDateTime mod = fi.lastModified();
        if (mod.isValid())
            addRow("properties.modified", fmtTime(mod));
        QDateTime acc = fi.lastRead();
        if (acc.isValid())
            addRow("properties.accessed", fmtTime(acc));
    }

    QString desc = Identify::describe(node, [](const QString& key) { return I18n::tr(key); });
    if (!desc.isEmpty())
        addRow("properties.identify", desc);
    if (node->error != NodeError::None)
        addRow("properties.note", node->errorText());

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
            .arg(QString::fromLatin1(C::PRIMARY()))
            .arg(I18n::tr("about.homepage", QMap<QString, QString>{{"url", HOMEPAGE}})));
    homepageLbl->setOpenExternalLinks(true);
    homepageLbl->setWordWrap(true);
    lay->addWidget(homepageLbl);

    auto* srcLbl = new QLabel(
        QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none;\">%3</a>")
            .arg(SOURCE_URL)
            .arg(QString::fromLatin1(C::PRIMARY()))
            .arg(I18n::tr("about.source", QMap<QString, QString>{{"url", SOURCE_URL}})));
    srcLbl->setOpenExternalLinks(true);
    srcLbl->setWordWrap(true);
    lay->addWidget(srcLbl);

    auto* thanks = new QLabel(I18n::tr("about.thanks"));
    thanks->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                              .arg(QString::fromLatin1(C::TEXT_MUTED())));
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

void MainWindow::showThemeCustomize()
{
    QDialog dlg(this);
    dlg.setWindowTitle(I18n::tr("theme_dialog.title"));
    dlg.setMinimumWidth(460);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(12);

    // ---- Working copy of the custom theme ----
    const QStringList ftTokens = Theme::editableFileTypeTokens();
    const QStringList uiTokens = Theme::editableUITokens();
    const QStringList allTokens = uiTokens + ftTokens;

    QString base = Theme::hasCustom() ? Theme::customBase() : QStringLiteral("dark");
    if (base != QStringLiteral("light") && base != QStringLiteral("dark"))
        base = QStringLiteral("dark");
    QMap<QString, QString> colors;
    if (Theme::hasCustom()) {
        for (const auto& t : allTokens)
            colors[t] = Theme::customColor(t);
    } else {
        const ThemeColors& src = (base == QStringLiteral("light")) ? LIGHT_THEME : DARK_THEME;
        for (const auto& t : allTokens)
            colors[t] = Theme::colorFromTheme(src, t);
    }

    // ---- Base theme selector ----
    auto* baseRow = new QHBoxLayout;
    auto* baseLbl = new QLabel(I18n::tr("theme_dialog.base_label"));
    auto* lightRadio = new QRadioButton(I18n::tr("theme.light"));
    auto* darkRadio = new QRadioButton(I18n::tr("theme.dark"));
    auto* baseGroup = new QButtonGroup(this);
    baseGroup->addButton(lightRadio);
    baseGroup->addButton(darkRadio);
    if (base == QStringLiteral("light"))
        lightRadio->setChecked(true);
    else
        darkRadio->setChecked(true);
    baseRow->addWidget(baseLbl);
    baseRow->addWidget(lightRadio);
    baseRow->addWidget(darkRadio);
    baseRow->addStretch(1);
    lay->addLayout(baseRow);

    // ---- Color rows ----
    QMap<QString, QPushButton*> swatches;
    QMap<QString, QLabel*> hexLabels;

    auto addSection = [&](const QString& titleKey, const QStringList& tokens) {
        auto* title = new QLabel(I18n::tr(titleKey));
        title->setStyleSheet(QStringLiteral("color:%1; font-weight:600;")
                                 .arg(QString::fromLatin1(C::TEXT_SEC())));
        lay->addWidget(title);
        for (const auto& token : tokens) {
            auto* row = new QHBoxLayout;
            auto* swatch = new QPushButton;
            swatch->setFixedSize(48, 24);
            swatch->setCursor(Qt::PointingHandCursor);
            swatches[token] = swatch;
            auto* nameLbl = new QLabel(I18n::tr(Theme::tokenLabelKey(token)));
            auto* hexLbl = new QLabel(colors.value(token));
            hexLbl->setStyleSheet(QStringLiteral("color:%1; font-family:Consolas,monospace;")
                                      .arg(QString::fromLatin1(C::TEXT_MUTED())));
            hexLabels[token] = hexLbl;
            row->addWidget(swatch);
            row->addSpacing(8);
            row->addWidget(nameLbl);
            row->addStretch(1);
            row->addWidget(hexLbl);
            lay->addLayout(row);
        }
    };

    addSection(QStringLiteral("theme_dialog.filetype_colors"), ftTokens);
    addSection(QStringLiteral("theme_dialog.ui_colors"), uiTokens);

    auto refreshSwatch = [&colors, &swatches, &hexLabels](const QString& token) {
        const QString c = colors.value(token);
        swatches[token]->setStyleSheet(QStringLiteral(
            "QPushButton { background-color:%1; border:1px solid %2; border-radius:4px; }")
            .arg(c).arg(QString::fromLatin1(C::BORDER())));
        hexLabels[token]->setText(c);
    };
    auto refreshAllSwatches = [&]() {
        for (const auto& token : allTokens)
            refreshSwatch(token);
    };
    refreshAllSwatches();

    // Wire swatch clicks to the color picker.
    for (const auto& token : allTokens) {
        QPushButton* swatch = swatches[token];
        connect(swatch, &QPushButton::clicked, this, [this, token, &colors, &refreshSwatch]() {
            const QColor initial(colors.value(token));
            const QColor picked = QColorDialog::getColor(
                initial, this, I18n::tr(Theme::tokenLabelKey(token)));
            if (picked.isValid()) {
                colors[token] = picked.name().toUpper();
                refreshSwatch(token);
            }
        });
    }

    // Base radio change -> reset editable colors from the new base.
    auto resetFromBase = [&]() {
        const ThemeColors& src = (base == QStringLiteral("light")) ? LIGHT_THEME : DARK_THEME;
        for (const auto& t : allTokens)
            colors[t] = Theme::colorFromTheme(src, t);
        refreshAllSwatches();
    };
    connect(lightRadio, &QRadioButton::toggled, this, [&](bool checked) {
        if (!checked) return;
        base = QStringLiteral("light");
        resetFromBase();
    });
    connect(darkRadio, &QRadioButton::toggled, this, [&](bool checked) {
        if (!checked) return;
        base = QStringLiteral("dark");
        resetFromBase();
    });

    lay->addStretch(1);

    // ---- Buttons ----
    auto* btnRow = new QHBoxLayout;
    auto* resetBtn = new QPushButton(I18n::tr("theme_dialog.reset"));
    auto* okBtn = new QPushButton(I18n::tr("button.ok"));
    okBtn->setObjectName("primary");
    okBtn->setCursor(Qt::PointingHandCursor);
    auto* cancelBtn = new QPushButton(I18n::tr("button.cancel"));
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, [&]() { resetFromBase(); });
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    btnRow->addWidget(resetBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    lay->addLayout(btnRow);

    if (dlg.exec() == QDialog::Accepted) {
        Theme::saveCustom(base, colors);   // populate g_customTheme + persist
        Theme::set(QStringLiteral("custom"));  // switch + apply
        refreshTheme();
        // Rebuild the menu so the "custom" radio shows as checked.
        menuBar()->clear();
        buildMenu();
    }
}

void MainWindow::openHomepage()
{
    QDesktopServices::openUrl(QUrl(HOMEPAGE));
}

void MainWindow::checkForUpdate(bool silent)
{
    if (!silent)
        m_statusLabel->setText(I18n::tr("update.checking"));
    QNetworkRequest req(QUrl(QString(HOMEPAGE) + QStringLiteral("version")));
    req.setTransferTimeout(10000);
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, silent]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (!silent) {
                m_statusLabel->setText(QString());
                Dialogs::warn(this, I18n::tr("update.check_failed"),
                    I18n::tr("update.check_failed_body",
                        QMap<QString, QString>{{"error", reply->errorString()}}));
            } else {
                Logger::warn(QStringLiteral("Silent update check failed: %1")
                                 .arg(reply->errorString()));
            }
            return;
        }
        QString remoteVer = QString::fromUtf8(reply->readAll()).trimmed();
        QVersionNumber local = QVersionNumber::fromString(APP_VERSION);
        QVersionNumber remote = QVersionNumber::fromString(remoteVer);
        if (remote.isNull()) {
            if (!silent) {
                m_statusLabel->setText(QString());
                Dialogs::warn(this, I18n::tr("update.check_failed"),
                    I18n::tr("update.check_failed_body",
                        QMap<QString, QString>{{"error", QStringLiteral("Invalid version response")}}));
            } else {
                Logger::warn(QStringLiteral("Silent update check: invalid version response"));
            }
            return;
        }
        if (!silent)
            m_statusLabel->setText(QString());
        if (remote > local) {
            if (silent) {
                // Non-intrusive bottom-right toast; user can dismiss or click
                // "download & install" to auto-update.
                showUpdateToast(remoteVer);
            } else {
                QDialog dlg(this);
                dlg.setWindowTitle(I18n::tr("update.new_title"));
                dlg.setMinimumWidth(420);
                auto* lay = new QVBoxLayout(&dlg);
                lay->setContentsMargins(20, 20, 20, 16);
                lay->setSpacing(10);

                auto* body = new QLabel(I18n::tr("update.new_body",
                    QMap<QString, QString>{
                        {"version", remoteVer},
                        {"local", APP_VERSION},
                    }));
                body->setWordWrap(true);
                lay->addWidget(body);

                auto* urlLbl = new QLabel(
                    QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none;\">%3</a>")
                        .arg(HOMEPAGE)
                        .arg(QString::fromLatin1(C::PRIMARY()))
                        .arg(I18n::tr("update.download_url",
                            QMap<QString, QString>{{"url", HOMEPAGE}})));
                urlLbl->setOpenExternalLinks(true);
                urlLbl->setTextInteractionFlags(Qt::TextBrowserInteraction);
                urlLbl->setWordWrap(true);
                urlLbl->setCursor(Qt::PointingHandCursor);
                lay->addWidget(urlLbl);

                lay->addStretch(1);
                auto* btnRow = new QHBoxLayout;
                btnRow->addStretch(1);
                auto* closeBtn = new QPushButton(I18n::tr("button.close"));
                closeBtn->setCursor(Qt::PointingHandCursor);
                connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
                btnRow->addWidget(closeBtn);
                auto* downloadBtn = new QPushButton(I18n::tr("update.download_install"));
                downloadBtn->setObjectName("primary");
                downloadBtn->setCursor(Qt::PointingHandCursor);
                btnRow->addWidget(downloadBtn);
                lay->addLayout(btnRow);

                bool wantDownload = false;
                connect(downloadBtn, &QPushButton::clicked, this, [&]() {
                    wantDownload = true;
                    dlg.accept();
                });
                dlg.exec();

                if (wantDownload) {
                    // Hand off to the same toast-based download/install flow
                    // used by the silent startup check. The toast surfaces
                    // progress, status, and installer launch.
                    m_updateRemoteVer = remoteVer;
                    showUpdateToast(remoteVer);
                    downloadAndInstall();
                }
            }
        } else {
            if (!silent) {
                Dialogs::info(this, I18n::tr("update.current"),
                    I18n::tr("update.current_body",
                        QMap<QString, QString>{{"version", APP_VERSION}}));
            }
            // silent + up-to-date: nothing to show.
        }
    });
}

// --------------------------------------------------------------------------- //
// Auto-update toast
// --------------------------------------------------------------------------- //
// A frameless top-level tool window pinned to the bottom-right of the screen.
// Shown when a silent startup update check finds a newer version. The user can
// dismiss it with the "x" button or click "download & install" to fetch the
// installer and run it silently. The app quits right after launching the
// installer so the .exe lock is released; the installer (built with the
// [Code] relaunch step in installer.iss) restarts the new version when done.

void MainWindow::buildUpdateToast()
{
    m_updateToast = new QFrame(this,
        Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    m_updateToast->setObjectName("updateToast");
    m_updateToast->setFixedWidth(340);
    m_updateToast->setAttribute(Qt::WA_ShowWithoutActivating);
    m_updateToast->setFocusPolicy(Qt::NoFocus);
    m_updateToast->setVisible(false);

    auto* lay = new QVBoxLayout(m_updateToast);
    lay->setContentsMargins(16, 12, 12, 12);
    lay->setSpacing(8);

    // Title row.
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    m_updateToastTitle = new QLabel(QStringLiteral("🔄 ") + I18n::tr("update.new_title"));
    m_updateToastTitle->setObjectName("toast-title");
    titleRow->addWidget(m_updateToastTitle);
    titleRow->addStretch(1);
    m_updateToastCloseBtn = new QPushButton(QStringLiteral("×"));
    m_updateToastCloseBtn->setObjectName("ghost");
    m_updateToastCloseBtn->setFixedSize(24, 24);
    m_updateToastCloseBtn->setCursor(Qt::PointingHandCursor);
    m_updateToastCloseBtn->setToolTip(I18n::tr("button.close"));
    connect(m_updateToastCloseBtn, &QPushButton::clicked, this, &MainWindow::closeUpdateToast);
    titleRow->addWidget(m_updateToastCloseBtn);
    lay->addLayout(titleRow);

    m_updateToastBody = new QLabel;
    m_updateToastBody->setObjectName("toast-body");
    m_updateToastBody->setWordWrap(true);
    m_updateToastBody->setTextFormat(Qt::PlainText);
    lay->addWidget(m_updateToastBody);

    m_updateToastStatus = new QLabel;
    m_updateToastStatus->setObjectName("toast-status");
    m_updateToastStatus->setVisible(false);
    m_updateToastStatus->setWordWrap(true);
    lay->addWidget(m_updateToastStatus);

    m_updateToastProgress = new QProgressBar;
    m_updateToastProgress->setRange(0, 100);
    m_updateToastProgress->setValue(0);
    m_updateToastProgress->setVisible(false);
    lay->addWidget(m_updateToastProgress);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_updateToastInstallBtn = new QPushButton(I18n::tr("update.download_install"));
    m_updateToastInstallBtn->setObjectName("primary");
    m_updateToastInstallBtn->setCursor(Qt::PointingHandCursor);
    connect(m_updateToastInstallBtn, &QPushButton::clicked, this, [this]() {
        if (!m_updateRemoteVer.isEmpty())
            downloadAndInstall();
    });
    btnRow->addWidget(m_updateToastInstallBtn);
    lay->addLayout(btnRow);

    applyUpdateToastStyle();
}

void MainWindow::applyUpdateToastStyle()
{
    if (!m_updateToast)
        return;
    m_updateToast->setStyleSheet(QStringLiteral(
        "QFrame#updateToast { background-color: %1; border: 1px solid %2; border-radius: 10px; }"
        "QLabel#toast-title { font-size: 14px; font-weight: 600; color: %3; }"
        "QLabel#toast-body { font-size: 12px; color: %4; }"
        "QLabel#toast-status { font-size: 11px; color: %5; }"
    ).arg(QString::fromLatin1(C::SURFACE()))
     .arg(QString::fromLatin1(C::BORDER()))
     .arg(QString::fromLatin1(C::FG()))
     .arg(QString::fromLatin1(C::TEXT_SEC()))
     .arg(QString::fromLatin1(C::TEXT_MUTED())));
}

void MainWindow::showUpdateToast(const QString& remoteVer)
{
    if (!m_updateToast)
        buildUpdateToast();
    m_updateRemoteVer = remoteVer;
    m_updateToastBody->setText(I18n::tr("update.toast_body",
        QMap<QString, QString>{{"version", remoteVer}, {"local", APP_VERSION}}));
    // Reset interactive state so a previously-dismissed toast can be reused.
    m_updateToastProgress->setVisible(false);
    m_updateToastProgress->setValue(0);
    m_updateToastStatus->setVisible(false);
    m_updateToastStatus->setText(QString());
    m_updateToastInstallBtn->setVisible(true);
    m_updateToastInstallBtn->setEnabled(true);
    m_updateToastInstallBtn->setText(I18n::tr("update.download_install"));
    m_updateToast->adjustSize();
    m_updateToast->show();
    repositionUpdateToast();
}

void MainWindow::repositionUpdateToast()
{
    if (!m_updateToast || !m_updateToast->isVisible())
        return;
    auto* screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect avail = screen->availableGeometry();
    m_updateToast->adjustSize();
    const QSize sz = m_updateToast->size();
    const int x = avail.right() - sz.width() - 16;
    const int y = avail.bottom() - sz.height() - 8;
    m_updateToast->move(x, y);
}

void MainWindow::closeUpdateToast()
{
    if (m_updateDownloadReply)
        m_updateDownloadReply->abort();  // -> finished() cleans up + resets UI
    if (m_updateToast)
        m_updateToast->hide();
}

void MainWindow::retranslateUpdateToast()
{
    if (!m_updateToast)
        return;
    m_updateToastTitle->setText(QStringLiteral("🔄 ") + I18n::tr("update.new_title"));
    m_updateToastCloseBtn->setToolTip(I18n::tr("button.close"));
    m_updateToastInstallBtn->setText(I18n::tr("update.download_install"));
    if (!m_updateRemoteVer.isEmpty())
        m_updateToastBody->setText(I18n::tr("update.toast_body",
            QMap<QString, QString>{{"version", m_updateRemoteVer}, {"local", APP_VERSION}}));
    repositionUpdateToast();
}

void MainWindow::downloadAndInstall()
{
    if (m_updateDownloadReply)
        return;  // already downloading

    // Build the download URL from the REMOTE version so it matches the
    // installer artifact produced by `installer.iss`
    // (OutputBaseFilename=NcduWin_{version}_Setup). Using the remote version
    // (not APP_VERSION) is required: the remote host serves the newer build,
    // whose filename embeds the newer version number.
    const QString url = QString(HOMEPAGE)
        + QStringLiteral("downloads/NcduWin_%1_Setup.exe").arg(m_updateRemoteVer);
    const QString path = QDir::tempPath() + QStringLiteral("/NcduWin_Setup.exe");
    QFile::remove(path);

    m_updateDownloadFile = std::make_unique<QFile>(path);
    if (!m_updateDownloadFile->open(QIODevice::WriteOnly)) {
        m_updateToastStatus->setVisible(true);
        m_updateToastStatus->setText(I18n::tr("update.download_failed",
            QMap<QString, QString>{{"error", m_updateDownloadFile->errorString()}}));
        return;
    }

    QNetworkRequest req((QUrl(url)));
    req.setTransferTimeout(120000);
    m_updateDownloadReply = m_nam->get(req);
    connect(m_updateDownloadReply, &QNetworkReply::readyRead,
            this, &MainWindow::onUpdateDownloadReady);
    connect(m_updateDownloadReply, &QNetworkReply::downloadProgress,
            this, &MainWindow::onUpdateDownloadProgress);
    connect(m_updateDownloadReply, &QNetworkReply::finished,
            this, &MainWindow::onUpdateDownloadFinished);

    m_updateToastInstallBtn->setEnabled(false);
    m_updateToastProgress->setValue(0);
    m_updateToastProgress->setVisible(true);
    m_updateToastStatus->setText(I18n::tr("update.downloading",
        QMap<QString, QString>{{"percent", QStringLiteral("0")}}));
    m_updateToastStatus->setVisible(true);
}

void MainWindow::onUpdateDownloadReady()
{
    if (m_updateDownloadReply && m_updateDownloadFile)
        m_updateDownloadFile->write(m_updateDownloadReply->readAll());
}

void MainWindow::onUpdateDownloadProgress(qint64 received, qint64 total)
{
    if (total <= 0)
        return;
    const int pct = int(received * 100 / total);
    m_updateToastProgress->setValue(pct);
    m_updateToastStatus->setText(I18n::tr("update.downloading",
        QMap<QString, QString>{{"percent", QString::number(pct)}}));
}

void MainWindow::onUpdateDownloadFinished()
{
    auto* reply = m_updateDownloadReply;
    m_updateDownloadReply = nullptr;
    if (!reply)
        return;
    reply->deleteLater();

    if (m_updateDownloadFile) {
        m_updateDownloadFile->close();
        m_updateDownloadFile.reset();
    }

    const bool cancelled = (reply->error() == QNetworkReply::OperationCanceledError);
    if (reply->error() != QNetworkReply::NoError && !cancelled) {
        m_updateToastProgress->setVisible(false);
        m_updateToastStatus->setText(I18n::tr("update.download_failed",
            QMap<QString, QString>{{"error", reply->errorString()}}));
        m_updateToastInstallBtn->setEnabled(true);
        return;
    }
    if (cancelled) {
        // Toast was dismissed by the user; reset for a possible retry.
        m_updateToastProgress->setVisible(false);
        m_updateToastStatus->setVisible(false);
        m_updateToastInstallBtn->setEnabled(true);
        return;
    }

    // Download complete: launch the installer silently and quit. The app runs
    // elevated (manifest requireAdministrator), so no UAC prompt appears. The
    // installer closes the running app (/CLOSEAPPLICATIONS), installs, then
    // relaunches the new version via installer.iss [Code] -> CurStepChanged.
    m_updateToastProgress->setVisible(false);
    m_updateToastStatus->setText(I18n::tr("update.installing"));
    m_updateToastInstallBtn->setVisible(false);

    const QString path = QDir::tempPath() + QStringLiteral("/NcduWin_Setup.exe");
    const QString params = QStringLiteral(
        "/SILENT /NOCANCEL /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART");

#ifdef _WIN32
    const HINSTANCE h = ShellExecuteW(nullptr, L"runas",
        reinterpret_cast<LPCWSTR>(path.utf16()),
        reinterpret_cast<LPCWSTR>(params.utf16()),
        nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        m_updateToastStatus->setText(I18n::tr("update.launch_failed"));
        m_updateToastInstallBtn->setVisible(true);
        m_updateToastInstallBtn->setEnabled(true);
        return;
    }
#else
    Q_UNUSED(path) Q_UNUSED(params)
#endif
    // Quit so the installer can replace the running executable.
    QApplication::quit();
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
        m_cleanupScanner = nullptr;
    }
    // Cancel any in-flight duplicate scan so its signals don't leak into the
    // freshly-cleared panel.
    if (m_dupScanner) {
        m_dupScanner->cancel();
        m_dupScanner->wait(100);
        m_dupScanner = nullptr;
    }

    m_cleanupPanel->startScanProgress();
    m_cleanupTargets.clear();
    m_largeFiles.clear();
    m_duplicateGroups.clear();

    m_cleanupScanner = new CleanupScanner(scanPath, m_root, this);
    // Auto-delete the cleanup scanner once its run() returns (same pattern
    // as the main scanner — prevents leak on every rescan).
    connect(m_cleanupScanner, &QThread::finished,
            m_cleanupScanner, &QObject::deleteLater);
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

    // Start the duplicate-file scan after the cleanup scan finishes so the
    // two don't compete for disk I/O. The dup scan reuses the same FileNode
    // tree (m_root) built by the main scan.
    if (m_root && !m_lastScanPath.isEmpty()) {
        m_dupScanner = new DuplicateScanner(m_lastScanPath, m_root, this);
        connect(m_dupScanner, &QThread::finished,
                m_dupScanner, &QObject::deleteLater);
        connect(m_dupScanner, &DuplicateScanner::groupFound,
                m_cleanupPanel, &CleanupPanel::addDuplicateGroup);
        connect(m_dupScanner, &DuplicateScanner::progress,
                this, &MainWindow::onDupScanProgress);
        connect(m_dupScanner, &DuplicateScanner::finished,
                this, &MainWindow::onDupScanDone);
        m_cleanupPanel->setDupScanStarted();
        m_dupScanner->start();
    }
}

void MainWindow::onDupScanProgress(int phase, int processed, int total)
{
    // Forward to the Duplicates tab's status + progress bar.
    m_cleanupPanel->setDupScanProgress(phase, processed, total);

    // Also reflect progress in the main status bar so it's visible even when
    // the user is on another tab.
    QString phaseKey;
    switch (phase) {
        case 1:  phaseKey = QStringLiteral("cleanup.dup_phase_size"); break;
        case 2:  phaseKey = QStringLiteral("cleanup.dup_phase_partial"); break;
        case 3:  phaseKey = QStringLiteral("cleanup.dup_phase_full"); break;
        default: return;
    }
    QString text = I18n::tr(phaseKey);
    if (total > 0)
        text = QStringLiteral("%1 %2/%3").arg(text).arg(processed).arg(total);
    m_statusLabel->setText(text);
}

void MainWindow::onDupScanDone(std::vector<DuplicateGroup> groups,
                               qint64 /*totalWasted*/, int /*totalFiles*/)
{
    m_dupScanner = nullptr;
    m_duplicateGroups = std::move(groups);
    m_cleanupPanel->loadDuplicates(m_duplicateGroups);
    // Restore the main scan summary that the dup-scan progress text overwrote.
    updateScannedStatus();
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

    // Look up a file path's size across large files and duplicate groups.
    auto lookupFileSize = [this](const QString& fp) -> qint64 {
        for (const auto& lf : m_largeFiles) {
            if (lf.path == fp)
                return lf.size;
        }
        for (const auto& dg : m_duplicateGroups) {
            for (const auto& df : dg.files) {
                if (df.path == fp)
                    return df.size;
            }
        }
        return 0;
    };

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
    for (const auto& fp : fileItems)
        totalSize += lookupFileSize(fp);

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
            qint64 sz = lookupFileSize(fp);
            namesList << QStringLiteral("\u2022 %1 (%2)").arg(name).arg(humanSize(sz));
        }
    }
    QString names = namesList.join("\n");
    QString more = count > 8
                       ? I18n::tr("dialog.recycle.more", QMap<QString, QString>{{"n", QString::number(count - 8)}})
                       : QString();

    // Ask the user how to clean: move to the Recycle Bin, delete permanently,
    // or cancel. This is shown for every selection (including S-level items).
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(I18n::tr("cleanup.clean_title"));
    box.setText(I18n::tr("cleanup.clean_body",
        QMap<QString, QString>{{"size", humanSize(totalSize)},
                               {"names", names},
                               {"more", more}}));
    auto* recycleBtn = box.addButton(I18n::tr("cleanup.clean_recycle"),
                                     QMessageBox::AcceptRole);
    auto* permBtn = box.addButton(I18n::tr("cleanup.clean_permanent"),
                                  QMessageBox::DestructiveRole);
    auto* cancelBtn = box.addButton(I18n::tr("button.cancel"),
                                    QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == cancelBtn || box.clickedButton() == nullptr)
        return;
    const CleanupWorker::DeleteMode mode =
        (box.clickedButton() == recycleBtn)
            ? CleanupWorker::DeleteMode::RecycleBin
            : CleanupWorker::DeleteMode::Permanent;

    // Cleanup is the one place where a program holding a file open is not a
    // reason to stop: Temp, caches, logs and Downloads are never fully idle, and
    // skipping what is in use is what this mode is for. The holders are still
    // named — the difference is that this pre-flight informs rather than refuses.
    QStringList guardPaths;
    for (const auto& [key, path] : targetItems) {
        // Emptying the Recycle Bin is not an operation on that path, so there is
        // nothing there to be held open.
        if (key == QLatin1String("cleanup.b_recycle"))
            continue;
        guardPaths << path;
    }
    for (const QString& fp : fileItems)
        guardPaths << fp;

    guardThen(guardPaths, /*fatal=*/false,
              [this, items, mode]() { startCleanup(items, mode); });
}

// The cleanup run itself, started once the pre-flight has had its say.
void MainWindow::startCleanup(
    const std::vector<std::tuple<QString, QString, QString>>& items,
    CleanupWorker::DeleteMode mode)
{
    // Cancel any existing cleanup worker.
    if (m_cleanupWorker) {
        m_cleanupWorker->cancel();
        if (!m_cleanupWorker->wait(100)) {
            m_statusLabel->setText(I18n::tr("status.cancelling"));
            return;
        }
        m_cleanupWorker = nullptr;
    }

    m_cleanupPanel->setCleaning(true);

    // Convert items to CleanupWorker::ItemRef.
    std::vector<CleanupWorker::ItemRef> refs;
    for (const auto& [type, key, path] : items)
        refs.push_back({type, key, path});

    m_cleanupWorker = new CleanupWorker(refs, m_cleanupTargets, m_largeFiles, mode, this);
    connect(m_cleanupWorker, &QThread::finished,
            m_cleanupWorker, &QObject::deleteLater);
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
    // Intentionally no per-item disk-space query here: GetDiskFreeSpaceExW
    // is synchronous I/O on the UI thread and causes freezing when many items
    // are cleaned. Disk free space is updated once in onCleanupFinished.
}

void MainWindow::onCleanupFinished(int totalDeleted, int totalSkipped, qint64 totalFreed,
                                   int /*totalItems*/,
                                   std::vector<CleanupWorker::ItemRef> successItems,
                                   std::vector<CleanupWorker::ItemRef> failedItems)
{
    m_cleanupWorker = nullptr;
    m_cleanupPanel->setCleaning(false);
    m_cleanupPanel->endCleanProgress();

    // Only the items that were actually finished leave the list. One that still
    // holds files because a program is using them stays where the user will look
    // to try again — taking the row away would claim a cleanup that did not
    // happen, and there would be nothing left to press the second time.
    std::vector<CleanupWorker::ItemRef> finishedItems;
    int leftovers = 0;
    for (const auto& item : successItems) {
        if (item.skipped > 0)
            ++leftovers;
        else
            finishedItems.push_back(item);
    }
    m_cleanupPanel->removeCleanedItems(finishedItems);

    // Every path that was touched goes to the tree, both lists: the size of a
    // row has to match the disk whether the item was cleaned, partly cleaned or
    // not cleaned at all.
    std::vector<CleanupWorker::ItemRef> touching = successItems;
    touching.insert(touching.end(), failedItems.begin(), failedItems.end());
    syncTreeAfterCleanup(touching);

    QString status = I18n::tr("cleanup.clean_done", QMap<QString, QString>{
        {"freed", humanSize(totalFreed)},
        {"deleted", humanCount(totalDeleted)},
        {"skipped", humanCount(totalSkipped)},
    });
    if (leftovers > 0) {
        // Not a dialog: a Temp folder keeping a few files that are open is
        // normal, and interrupting the user over it every time would train them
        // to dismiss the dialog that matters. It is said, though.
        status += QStringLiteral("  ")
                  + I18n::tr("cleanup.kept_status",
                             QMap<QString, QString>{{"n", QString::number(leftovers)}});
    }
    m_statusLabel->setText(status);

    // Update disk free space.
    if (m_root) {
        updateDiskFreeLabel(m_root->path);
        auto [free, used, total] = WinApi::getDiskFreeSpace(m_root->path);
        m_cleanupPanel->updateFreeSpace(free, total);
    }

    if (failedItems.empty())
        return;

    // What did not get cleaned, and why. The old summary listed the items under
    // "可能是文件被占用或权限不足" and left the user to work out which — while the
    // answer was already known to whoever tried to delete it.
    QStringList failedParts;
    for (const auto& item : failedItems) {
        const QString label = item.type == QLatin1String("target")
                                  ? I18n::tr(item.key)
                                  : QFileInfo(item.path).fileName();
        const QString why = OpGuard::reasonSentence(item.reason, item.winError);
        failedParts << QStringLiteral("\u2022 ")
                           + (why.isEmpty()
                                  ? label
                                  : I18n::tr("cleanup.failed_item",
                                             QMap<QString, QString>{{"name", label},
                                                                    {"reason", why}}));
    }
    const int failLimit = std::min<int>(10, static_cast<int>(failedParts.size()));
    QString failedMsg = failedParts.mid(0, failLimit).join(QStringLiteral("\n"));
    if (static_cast<int>(failedParts.size()) > failLimit) {
        failedMsg += I18n::tr("cleanup.failed_more", QMap<QString, QString>{
            {"n", QString::number(static_cast<int>(failedParts.size()) - failLimit)}});
    }
    Dialogs::warn(this, I18n::tr("cleanup.failed_title"),
        I18n::tr("cleanup.failed_body", QMap<QString, QString>{{"items", failedMsg}}));
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

// --------------------------------------------------------------------------- //
// Theme switching
// --------------------------------------------------------------------------- //
void MainWindow::switchTheme(const QString& code)
{
    // Selecting "custom" without a saved custom theme opens the editor instead
    // of applying an unconfigured palette.
    if (code == QStringLiteral("custom") && !Theme::hasCustom()) {
        showThemeCustomize();
        return;
    }
    if (code == Theme::current())
        return;
    Theme::set(code);           // persist + update g_currentTheme
    refreshTheme();
}

void MainWindow::refreshTheme()
{
    // Re-apply the global stylesheet (loadQSS re-reads the active palette).
    qApp->setStyleSheet(loadQSS());
    // Panels with baked inline styles need to recompute them.
    m_cleanupPanel->refreshTheme();
    if (m_systemOptPanel)
        m_systemOptPanel->refreshTheme();
    // The move tab can be out of sight while it works, so it is refreshed here
    // too instead of only when its dialog is next opened.
    if (m_movePanel)
        m_movePanel->refreshTheme();
    m_legend->refreshTheme();
    applyUpdateToastStyle();
    // Type icons cache by color, so clear before rebuilding the list.
    clearTypeIconCache();
    // List/treemap items cache colors on the item, so rebuild them.
    if (m_current) {
        populateList(m_current);
        m_treemap->setNode(m_current, m_showFiles, 1);
    }
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
    m_rightTabs->setTabText(2, I18n::tr("tab.sysopt"));

    // Cleanup panel
    m_cleanupPanel->retranslate();
    if (m_systemOptPanel)
        m_systemOptPanel->retranslate();

    // Widgets and windows that outlive a language change. None of these is
    // rebuilt when it is shown again, so this is the only chance to relabel
    // them: the tab panels above are recreated per session, these are not.
    if (m_syncDialog) {
        // The move panel sits inside that dialog right now, and the dialog's own
        // retranslate() forwards to it.
        m_syncDialog->retranslate();
    } else if (m_movePanel) {
        // Closed: the panel is back with this window and out of sight, but it is
        // shown again on the next open — with its rows already built.
        m_movePanel->retranslate();
    }
    if (m_aiDialog)
        m_aiDialog->retranslate();

    // Update toast (if built).
    retranslateUpdateToast();

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

// --------------------------------------------------------------------------- //
// AI analysis (optional feature)
// --------------------------------------------------------------------------- //
void MainWindow::onAiSettings()
{
    AiSettingsDialog dlg(m_ai, this);
    dlg.exec();
}

void MainWindow::onAiAnalyzeCurrent()
{
    if (!m_current)
        return;
    runAiAnalysis(m_current->name, buildDirPrompt(m_current));
}

QString MainWindow::buildDirPrompt(const std::shared_ptr<FileNode>& node) const
{
    if (!node)
        return QString();
    const QString intro = I18n::tr("ai.prompt.intro",
        QMap<QString, QString>{{"lang", I18n::currentLanguage()}});
    const QString kind = node->isDir()
        ? I18n::tr("ai.prompt.kind_dir")
        : I18n::tr("ai.prompt.kind_file");

    QStringList children;
    const int limit = 20;
    for (int i = 0; i < static_cast<int>(node->children.size()) && i < limit; ++i) {
        const auto& c = node->children[i];
        children << I18n::tr("ai.prompt.children_line",
            QMap<QString, QString>{{"name", c->name}, {"size", humanSize(c->size)}});
    }

    return I18n::tr("ai.prompt.dir", QMap<QString, QString>{
        {"intro", intro},
        {"path", node->path},
        {"kind", kind},
        {"size", humanSize(node->size)},
        {"files", QString::number(node->fileCount)},
        {"folders", QString::number(node->dirCount)},
        {"limit", QString::number(limit)},
        {"children", children.join("\n")},
    });
}

void MainWindow::runAiAnalysis(const QString& subject, const QString& prompt)
{
    if (!m_ai)
        return;

    const AiConfig cfg = m_ai->load();
    if (!m_ai->isConfigured(cfg)) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(I18n::tr("ai.analysis.title"));
        box.setText(I18n::tr("ai.analysis.not_configured"));
        auto* cfgBtn = box.addButton(I18n::tr("ai.analysis.configure"), QMessageBox::AcceptRole);
        box.addButton(I18n::tr("button.cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == cfgBtn)
            onAiSettings();
        return;
    }

    // Show (or reuse) the conversational Q&A dialog in loading state.
    if (!m_aiDialog) {
        m_aiDialog = new AiAnalysisDialog(this);
        m_aiDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_aiDialog, &AiAnalysisDialog::questionSubmitted,
                this, &MainWindow::runAiFollowUp);
    }
    m_aiDialog->setSubject(subject);
    m_aiDialog->beginAnalysis(prompt);
    m_aiDialog->show();
    m_aiDialog->raise();

    // One-shot handlers for the streamed chat-completion result.
    auto onAnalysisChunk = [this](const QString& delta) {
        if (m_aiDialog)
            m_aiDialog->appendChunk(delta);
    };
    auto onAnalysisFailed = [this](const QString& error) {
        if (m_aiDialog)
            m_aiDialog->setError(error);
    };
    auto performAnalysis = [this, prompt, onAnalysisChunk, onAnalysisFailed](const QString& model) {
        m_aiModel = model;
        const QMetaObject::Connection c1 =
            connect(m_ai, &AiService::analysisChunk, this, onAnalysisChunk);
        const QMetaObject::Connection c2 =
            connect(m_ai, &AiService::analysisFailed, this, onAnalysisFailed);
        const QMetaObject::Connection c3 =
            connect(m_ai, &AiService::analysisStreamEnded, this, [this]() {
                if (m_aiDialog)
                    m_aiDialog->finalizeAnswer();
            });
        auto detach = [c1, c2, c3]() {
            disconnect(c1);
            disconnect(c2);
            disconnect(c3);
        };
        connect(m_ai, &AiService::analysisStreamEnded, this, detach);
        connect(m_ai, &AiService::analysisFailed, this, detach);
        m_ai->analyze(prompt, model);
    };

    if (cfg.model.trimmed() == QLatin1String("auto")) {
        // Resolve "auto" to the first model reported by the endpoint.
        const QMetaObject::Connection cM =
            connect(m_ai, &AiService::modelsFetched, this,
                    [this, performAnalysis](const QStringList& ids) {
                        if (ids.isEmpty()) {
                            if (m_aiDialog)
                                m_aiDialog->setError(I18n::tr("ai.analysis.model_fetch_failed",
                                    QMap<QString, QString>{
                                        {"error", I18n::tr("ai.settings.no_models")}}));
                            return;
                        }
                        performAnalysis(ids.first());
                    });
        const QMetaObject::Connection cF =
            connect(m_ai, &AiService::modelsFailed, this, [this](const QString& error) {
                if (m_aiDialog)
                    m_aiDialog->setError(I18n::tr("ai.analysis.model_fetch_failed",
                        QMap<QString, QString>{{"error", error}}));
            });
        connect(m_ai, &AiService::modelsFetched, this, [cM, cF]() { disconnect(cM); disconnect(cF); });
        connect(m_ai, &AiService::modelsFailed, this, [cM, cF]() { disconnect(cM); disconnect(cF); });
        m_ai->fetchModels();
    } else {
        performAnalysis(cfg.model.trimmed());
    }
}

void MainWindow::onAiQa()
{
    if (!m_ai)
        return;

    const AiConfig cfg = m_ai->load();
    if (!m_ai->isConfigured(cfg)) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(I18n::tr("ai.analysis.title"));
        box.setText(I18n::tr("ai.analysis.not_configured"));
        auto* cfgBtn = box.addButton(I18n::tr("ai.analysis.configure"), QMessageBox::AcceptRole);
        box.addButton(I18n::tr("button.cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == cfgBtn)
            onAiSettings();
        return;
    }

    if (!m_aiDialog) {
        m_aiDialog = new AiAnalysisDialog(this);
        m_aiDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_aiDialog, &AiAnalysisDialog::questionSubmitted,
                this, &MainWindow::runAiFollowUp);
    }
    m_aiDialog->setSubject(QString());
    m_aiDialog->setSystemPrompt(buildQaSystemPrompt());
    m_aiDialog->beginConversation();
    m_aiDialog->show();
    m_aiDialog->raise();
}

QString MainWindow::buildQaSystemPrompt() const
{
    return I18n::tr("ai.qa.system");
}

void MainWindow::runAiFollowUp(const QJsonArray& messages)
{
    if (!m_ai || !m_aiDialog)
        return;

    // Run a streamed request against the given message history, wiring one-shot
    // handlers so each follow-up gets a clean, detachable connection.
    auto perform = [this, messages](const QString& model) {
        auto onAnalysisChunk = [this](const QString& delta) {
            if (m_aiDialog)
                m_aiDialog->appendChunk(delta);
        };
        auto onAnalysisFailed = [this](const QString& error) {
            if (m_aiDialog)
                m_aiDialog->setError(error);
        };
        const QMetaObject::Connection c1 =
            connect(m_ai, &AiService::analysisChunk, this, onAnalysisChunk);
        const QMetaObject::Connection c2 =
            connect(m_ai, &AiService::analysisFailed, this, onAnalysisFailed);
        const QMetaObject::Connection c3 =
            connect(m_ai, &AiService::analysisStreamEnded, this, [this]() {
                if (m_aiDialog)
                    m_aiDialog->finalizeAnswer();
            });
        auto detach = [c1, c2, c3]() {
            disconnect(c1);
            disconnect(c2);
            disconnect(c3);
        };
        connect(m_ai, &AiService::analysisStreamEnded, this, detach);
        connect(m_ai, &AiService::analysisFailed, this, detach);
        m_aiModel = model;
        m_ai->analyzeMessages(messages, model);
    };

    // Reuse the model from the initial analysis; otherwise (open-ended Q&A or
    // a fresh dialog) resolve it now — including the "auto" case.
    if (!m_aiModel.isEmpty()) {
        perform(m_aiModel);
        return;
    }
    const AiConfig cfg = m_ai->load();
    if (cfg.model.trimmed() != QLatin1String("auto")) {
        perform(cfg.model.trimmed());
        return;
    }
    const QMetaObject::Connection cM =
        connect(m_ai, &AiService::modelsFetched, this, [this, perform](const QStringList& ids) {
            if (ids.isEmpty()) {
                if (m_aiDialog)
                    m_aiDialog->setError(I18n::tr("ai.analysis.model_fetch_failed",
                        QMap<QString, QString>{
                            {"error", I18n::tr("ai.settings.no_models")}}));
                return;
            }
            perform(ids.first());
        });
    const QMetaObject::Connection cF =
        connect(m_ai, &AiService::modelsFailed, this, [this](const QString& error) {
            if (m_aiDialog)
                m_aiDialog->setError(I18n::tr("ai.analysis.model_fetch_failed",
                    QMap<QString, QString>{{"error", error}}));
        });
    auto detachModels = [cM, cF]() {
        disconnect(cM);
        disconnect(cF);
    };
    connect(m_ai, &AiService::modelsFetched, this, detachModels);
    connect(m_ai, &AiService::modelsFailed, this, detachModels);
    m_ai->fetchModels();
}
