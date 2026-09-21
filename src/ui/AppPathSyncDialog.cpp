#include "AppPathSyncDialog.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTabWidget>
#include <QFileDialog>
#include <QSettings>
#include <QDir>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QProgressDialog>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>
#include <atomic>
#include <memory>

#include "Style.h"
#include "I18n.h"
#include "DialogI18n.h"
#include "AppDataMovePanel.h"
#include "KnownFolderTable.h"
#include "KnownFolderPath.h"
#include "LockerDialog.h"
#include "WinApi.h"
#include "Logger.h"
#include "FormatHelpers.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <objbase.h>   // CLSIDFromString / CoTaskMemFree (winlean skips ole2.h)
#  include <shlobj.h>
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "ole32.lib")
#endif

namespace {

// How many times a move may be re-run after the user closed the programs holding
// files back. One retry covers the real case (a program releases its handles and
// the rest of the files follow); anything beyond that is a program that comes
// straight back, and an endless prompt loop is worse than an honest report.
constexpr int kMaxCloseRounds = 2;

// One spelling for every path this dialog shows, compares or stores.
// (KnownFolderPath.h carries the resolution itself, so the same code the app
// runs is the one probe_lab/run5 exercises.)
QString normalizedDir(const QString& raw)
{
    if (raw.isEmpty())
        return QString();
    return QDir::toNativeSeparators(QDir::cleanPath(raw));
}

// The FOLDERID every relocation call needs, taken from the same table the row
// was built from — the id alone can never select the wrong folder.
#ifdef _WIN32
bool knownFolderClsid(const QString& appId, CLSID* out)
{
    const KnownFolderEntry* entry = knownFolderById(appId);
    if (!entry)
        return false;
    return SUCCEEDED(CLSIDFromString(reinterpret_cast<const OLECHAR*>(entry->guid.utf16()), out));
}
#endif

const QString kUserShellFolders =
    QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders");

// The action column holds a single small button ("change..."). Anything wider
// than that is space taken away from the path column.
constexpr int kActionColWidth = 56;

#ifdef _WIN32
// Explorer's own "Location" tab changes a library folder through
// SHSetKnownFolderPath: the call rewrites the registry value AND refreshes the
// running session, so the change takes effect right away instead of at the
// next logon (a plain registry write would need an Explorer restart).
bool setKnownFolderPathRealtime(const QString& appId, const QString& nativePath)
{
    CLSID id{};
    if (!knownFolderClsid(appId, &id))
        return false;

    QDir().mkpath(nativePath);
    // SHSetKnownFolderPath goes through COM; initialise it for this call the
    // same way the shell does. An already-initialised thread returns S_FALSE,
    // which is fine — the matching CoUninitialize is balanced either way.
    const bool comHere = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const HRESULT hr = SHSetKnownFolderPath(
        id, KF_FLAG_DEFAULT, nullptr,
        reinterpret_cast<const wchar_t*>(nativePath.utf16()));
    if (comHere)
        CoUninitialize();
    if (FAILED(hr))
        Logger::warn(QStringLiteral("[app_sync] SHSetKnownFolderPath(%1) failed, hr=0x%2")
                         .arg(appId).arg(static_cast<ulong>(hr), 8, 16, QChar('0')));
    return SUCCEEDED(hr);
}
#endif

// Recursively moves the contents of *src* into *dst* and removes every source
// file the moment its copy is confirmed on the new drive — a move in Explorer
// semantics, safe to interrupt: whatever has not been moved yet stays exactly
// where it was. Reparse points are left alone (they are links, not data).
// Returns the number of bytes it relocated.
qint64 moveContentsRecursive(const QString& src, const QString& dst,
                             std::atomic<qint64>* copied, std::atomic<bool>* cancelled)
{
    const QFileInfo info(src);
    if (WinApi::isReparsePointAt(src))
        return 0;

    // Shortcuts (and true symlinks that carry no reparse attribute: .lnk
    // files) must bypass every Qt existence test below: Qt resolves .lnk to
    // its TARGET, so a dangling shortcut reports exists()==false and a valid
    // one reports the target's size and content — either way the shortcut
    // file itself would be skipped or corrupted. Copy it byte for byte with
    // the native API instead, exactly like Explorer's move does.
    const bool isShortcut = info.isSymLink()
        || info.fileName().endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
    if (isShortcut) {
        if (cancelled->load())
            return 0;
        const QString nativeSrc = QDir::toNativeSeparators(src);
        const QString nativeDst = QDir::toNativeSeparators(dst);
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(reinterpret_cast<const wchar_t*>(nativeSrc.utf16()), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;
        const qint64 sz = (qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        FindClose(hFind);
        if (!CopyFileW(reinterpret_cast<const wchar_t*>(nativeSrc.utf16()),
                       reinterpret_cast<const wchar_t*>(nativeDst.utf16()), FALSE))
            return 0;   // in use / access denied: the shortcut stays put
        DeleteFileW(reinterpret_cast<const wchar_t*>(nativeSrc.utf16()));
#else
        const qint64 sz = info.size();
        if (!QFile::copy(src, dst))
            return 0;
        QFile::remove(src);
#endif
        copied->fetch_add(sz);
        return sz;
    }

    if (!info.exists())
        return 0;

    if (info.isFile()) {
        if (cancelled->load())
            return 0;
        // dst is this entry's FULL target path (the caller already appended
        // this file's name); appending it again here produced
        // "newDir\name\name" — a path inside a folder that does not exist —
        // so every plain file silently failed to move while the folder tree
        // around it was rebuilt fine.
        const QString target = QDir::toNativeSeparators(dst);
        if (QFile::exists(target))
            QFile::remove(target);
        if (QFile::copy(src, target)) {
            QFile::remove(src);          // the file now lives at the target
            copied->fetch_add(info.size());
            return info.size();
        }
        return 0;  // in use / access denied: the file stays where it was
    }

    QDir().mkpath(dst);
    qint64 moved = 0;
    const auto entries = QDir(src).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& e : entries) {
        if (cancelled->load())
            return moved;
        moved += moveContentsRecursive(e.absoluteFilePath(),
                                       dst + QLatin1Char('/') + e.fileName(),
                                       copied, cancelled);
    }
    // Empty by now, unless some files could not be moved — those keep the
    // folder (and everything above it) alive on purpose.
    QDir().rmdir(src);
    return moved;
}

// Explorer's "Move Folder" confirmation, shown before a library folder's
// location changes. Returns 0 = cancel, 1 = move the files, 2 = change the
// location only (leaving every file where it was).
int askMoveFolderDialog(QWidget* parent, const QString& oldDir, const QString& newDir)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(I18n::tr("app_sync.move_dlg_title"));
    dlg.setModal(true);
    dlg.setMinimumWidth(460);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 18, 20, 14);
    lay->setSpacing(8);

    auto* question = new QLabel(I18n::tr("app_sync.move_dlg_question"));
    question->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    lay->addWidget(question);

    auto makePath = [&dlg](const QString& text) {
        auto* lab = new QLabel(text, &dlg);
        lab->setWordWrap(true);
        lab->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lab->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
            .arg(QString::fromLatin1(C::TEXT_SEC())));
        return lab;
    };
    lay->addWidget(makePath(I18n::tr("app_sync.move_dlg_new",
        QMap<QString, QString>{{"new", newDir}})));
    lay->addWidget(makePath(I18n::tr("app_sync.move_dlg_old",
        QMap<QString, QString>{{"old", oldDir}})));

    auto* hint = new QLabel(I18n::tr("app_sync.move_dlg_hint"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
        .arg(QString::fromLatin1(C::TEXT_MUTED())));
    lay->addWidget(hint);

    lay->addSpacing(6);
    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancelBtn = new QPushButton(I18n::tr("button.cancel"));
    cancelBtn->setCursor(Qt::PointingHandCursor);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    btns->addWidget(cancelBtn);
    auto* noBtn = new QPushButton(I18n::tr("app_sync.move_dlg_no"));
    noBtn->setCursor(Qt::PointingHandCursor);
    QObject::connect(noBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(2); });
    btns->addWidget(noBtn);
    auto* yesBtn = new QPushButton(I18n::tr("app_sync.move_dlg_yes"));
    yesBtn->setObjectName(QStringLiteral("primary"));
    yesBtn->setCursor(Qt::PointingHandCursor);
    yesBtn->setDefault(true);
    QObject::connect(yesBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(1); });
    btns->addWidget(yesBtn);
    lay->addLayout(btns);

    return dlg.exec();
}

} // namespace

AppPathSyncDialog::AppPathSyncDialog(QWidget* parent, const QString& suggestedPath,
                                     AppDataMovePanel* movePanel)
    : QDialog(parent)
    , m_suggestedPath(suggestedPath)
    , m_movePanel(movePanel)
{
    setWindowTitle(I18n::tr("app_sync.title"));
    // The path list benefits from more room than the default dialog width.
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);
    resize(780, 520);
    setMinimumSize(640, 420);

    if (m_movePanel) {
        // Borrowed: it belongs to whoever handed it over, and it is handed back
        // in the destructor. That is what lets a move keep running, and keep its
        // progress, while this dialog is closed.
        m_movePanelOwner = parent;
        // Whatever the move tab pointed at last is a better guess for "where do
        // you keep your data" than the folder this dialog was opened with.
        if (!m_movePanel->lastTargetRoot().isEmpty())
            m_suggestedPath = m_movePanel->lastTargetRoot();
    } else {
        m_movePanel = new AppDataMovePanel;
        m_ownsMovePanel = true;
    }

    buildUI();
    detectAllApps();
    refreshTable();
}

AppPathSyncDialog::~AppPathSyncDialog()
{
    if (m_ownsMovePanel || !m_movePanel)
        return;

    // A panel still parented to the tab widget would be destroyed together with
    // this dialog — which, mid-move, means killing the copy and taking the
    // process down with it. Take it out of the tab first, then hand it back.
    const int index = m_tabs ? m_tabs->indexOf(m_movePanel) : -1;
    if (index >= 0)
        m_tabs->removeTab(index);
    m_movePanel->hide();
    m_movePanel->setParent(m_movePanelOwner);
}

void AppPathSyncDialog::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    // No big bold heading here: the window title already says what this is,
    // and the heading pushed the tabs — the actual content — below the fold.
    // The one-line description stays, kept short so it does not repeat what
    // each tab explains on its own.
    m_descLabel = new QLabel(I18n::tr("app_sync.desc"));
    m_descLabel->setWordWrap(true);
    m_descLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.5;")
        .arg(QString::fromLatin1(C::TEXT_MUTED())));
    // The paragraph wraps over several lines, so it gets its own inset: flush
    // against the dialog border it reads as if it were cut off.
    auto* descRow = new QHBoxLayout;
    descRow->setContentsMargins(10, 0, 10, 0);
    descRow->addWidget(m_descLabel);
    outer->addLayout(descRow);

    auto* tabs = new QTabWidget;
    m_tabs = tabs;
    outer->addWidget(tabs, 1);

    // Tab 1: Windows' own library folders and the registry value behind each.
    auto* pathsPage = new QWidget;
    auto* mainLay = new QVBoxLayout(pathsPage);
    mainLay->setContentsMargins(4, 12, 4, 0);
    mainLay->setSpacing(12);

    m_summaryLabel = new QLabel;
    m_summaryLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 500; color: %1;")
        .arg(QString::fromLatin1(C::PRIMARY())));
    mainLay->addWidget(m_summaryLabel);

    // Top action bar (redirect every folder at once + rescan)
    auto* bar = new QHBoxLayout;
    bar->setSpacing(8);
    m_batchBtn = new QPushButton(I18n::tr("app_sync.batch"));
    m_batchBtn->setObjectName(QStringLiteral("primary"));
    m_batchBtn->setCursor(Qt::PointingHandCursor);
    m_batchBtn->setToolTip(I18n::tr("app_sync.batch_tip"));
    connect(m_batchBtn, &QPushButton::clicked, this, &AppPathSyncDialog::onBatchSetTargetFolder);
    bar->addWidget(m_batchBtn);

    m_rescanBtn = new QPushButton(I18n::tr("app_sync.rescan"));
    m_rescanBtn->setObjectName(QStringLiteral("ghost"));
    m_rescanBtn->setCursor(Qt::PointingHandCursor);
    connect(m_rescanBtn, &QPushButton::clicked, this, &AppPathSyncDialog::onRescan);
    bar->addWidget(m_rescanBtn);
    bar->addStretch(1);
    mainLay->addLayout(bar);

    // Tree list
    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("syncTree"));
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setHeaderLabels({
        I18n::tr("app_sync.col_app"),
        I18n::tr("app_sync.col_path"),
        I18n::tr("app_sync.col_action")
    });

    auto* hdr = m_tree->header();
    hdr->setSectionResizeMode(0, QHeaderView::Interactive);
    hdr->resizeSection(0, 180);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionResizeMode(2, QHeaderView::Fixed);
    // Narrow to start with; refreshTable() widens it to exactly what the one
    // button in it needs and no more.
    hdr->resizeSection(2, kActionColWidth);

    // Clicking the path cell opens it in Explorer.
    connect(m_tree, &QTreeWidget::itemClicked, this, &AppPathSyncDialog::onPathClicked);

    mainLay->addWidget(m_tree, 1);

    tabs->addTab(pathsPage, I18n::tr("app_sync.tab_paths"));

    // Tab 2: relocate any software's data folder where it has no save-path
    // setting at all. The panel is created by the caller (see the constructor);
    // adding it to the tab widget only borrows it for as long as the dialog is
    // open.
    tabs->addTab(m_movePanel, I18n::tr("app_move.tab"));

    // Bottom close button
    auto* btm = new QHBoxLayout;
    btm->addStretch(1);
    m_closeBtn = new QPushButton(I18n::tr("button.close"));
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btm->addWidget(m_closeBtn);
    outer->addLayout(btm);
}

void AppPathSyncDialog::retranslate()
{
    setWindowTitle(I18n::tr("app_sync.title"));
    m_descLabel->setText(I18n::tr("app_sync.desc"));

    m_batchBtn->setText(I18n::tr("app_sync.batch"));
    m_batchBtn->setToolTip(I18n::tr("app_sync.batch_tip"));
    m_rescanBtn->setText(I18n::tr("app_sync.rescan"));
    m_closeBtn->setText(I18n::tr("button.close"));

    m_tabs->setTabText(0, I18n::tr("app_sync.tab_paths"));
    m_tabs->setTabText(1, I18n::tr("app_move.tab"));
    m_tree->setHeaderLabels({
        I18n::tr("app_sync.col_app"),
        I18n::tr("app_sync.col_path"),
        I18n::tr("app_sync.col_action")
    });

    // Folder names, their tooltips, the per-row button and the column width all
    // depend on the language, so the list is rebuilt rather than patched.
    refreshTable();

    // The second tab is a panel this dialog only borrows: it is long-lived and
    // will not be constructed again, so nothing else would ever relabel it.
    if (m_movePanel)
        m_movePanel->retranslate();
}

void AppPathSyncDialog::detectAllApps()
{
    m_apps.clear();
    QSettings regShell(kUserShellFolders, QSettings::NativeFormat);

    for (const KnownFolderEntry& entry : knownFolderEntries()) {
        AppSaveInfo info;
        info.id = entry.id;
        info.nameKey = entry.nameKey;
        info.folder = entry.folder;
        info.currentPath = resolveKnownFolderPath(entry, regShell);

        // Windows creates these on demand and plenty of machines never did
        // (3D Objects, Saved Games, Searches, ...). A row whose folder is not
        // there is a row that cannot be moved, so it is left out; the everyday
        // folders stay listed with an empty path exactly as before, so the tab
        // keeps its familiar shape.
        if (!entry.alwaysList && info.currentPath.isEmpty())
            continue;

        m_apps.push_back(info);
    }
}

void AppPathSyncDialog::refreshTable()
{
    m_tree->clear();

    // Measured from a real button rather than guessed: font, DPI and the
    // translated wording all move the number, and a clipped "change" button is
    // worse than a slightly wider column.
    int actionWidth = kActionColWidth;

    for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) {
        const auto& app = m_apps[i];
        auto* item = new QTreeWidgetItem(m_tree);

        item->setText(0, I18n::tr(app.nameKey));
        item->setToolTip(0, I18n::tr(app.nameKey));

        // Path: only what really exists on the disk is shown, and clicking it
        // opens the folder in Explorer.
        const bool hasPath = !app.currentPath.isEmpty() && QDir(app.currentPath).exists();
        item->setText(1, hasPath ? app.currentPath : QStringLiteral("—"));
        item->setToolTip(1, hasPath
            ? I18n::tr("app_sync.open_tip", QMap<QString, QString>{{"path", app.currentPath}})
            : I18n::tr("app_sync.path_unknown"));
        item->setForeground(1, QColor(QString::fromLatin1(hasPath ? C::PRIMARY() : C::TEXT_MUTED())));

        auto* changeBtn = new QPushButton(I18n::tr("app_sync.change"));
        changeBtn->setObjectName(QStringLiteral("ghost"));
        changeBtn->setCursor(Qt::PointingHandCursor);
        changeBtn->setFixedHeight(22);
        changeBtn->setStyleSheet(QStringLiteral("font-size: 11px; padding: 1px 8px;"));
        changeBtn->setToolTip(I18n::tr("app_sync.change_tip"));
        // Sized from the label rather than from QPushButton::sizeHint(): the
        // native style pads a push button generously, and here the cell is only
        // ever going to hold this one small button.
        const QFontMetrics fm(changeBtn->font());
        actionWidth = qMax(actionWidth, fm.horizontalAdvance(changeBtn->text()) + 18);
        connect(changeBtn, &QPushButton::clicked, this, [this, i]() {
            onChangeSingleApp(i);
        });
        m_tree->setItemWidget(item, 2, changeBtn);
    }

    // Fixed mode resizeSection is the only thing that overrides the initial
    // width, and it must be applied after the buttons exist to be measured.
    m_tree->header()->resizeSection(2, actionWidth);

    m_summaryLabel->setText(I18n::tr("app_sync.summary", QMap<QString, QString>{
        {"total", QString::number(static_cast<int>(m_apps.size()))},
        {"time", QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))}}));
    m_batchBtn->setEnabled(!m_apps.empty());
}

void AppPathSyncDialog::onRescan()
{
    detectAllApps();
    refreshTable();
}

void AppPathSyncDialog::onPathClicked(QTreeWidgetItem* item, int column)
{
    if (!item || column != 1)
        return;
    const QString path = item->text(1);
    if (path.isEmpty() || path == QStringLiteral("—") || !QDir(path).exists())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void AppPathSyncDialog::onChangeSingleApp(int row)
{
    if (row < 0 || row >= static_cast<int>(m_apps.size()))
        return;

    auto& app = m_apps[row];
    QString startPath = QDir(app.currentPath).exists() ? app.currentPath : (m_suggestedPath.isEmpty() ? QDir::homePath() : m_suggestedPath);
    QString newDir = QFileDialog::getExistingDirectory(this,
        I18n::tr("app_sync.change_title", QMap<QString, QString>{{"name", I18n::tr(app.nameKey)}}),
        startPath);
    if (newDir.isEmpty())
        return;

    newDir = QDir::toNativeSeparators(newDir);
    QDir().mkpath(newDir);

    const QString oldDir = QDir(app.currentPath).exists()
        ? QDir::toNativeSeparators(QDir(app.currentPath).absolutePath()) : QString();
    if (!oldDir.isEmpty() && oldDir.compare(newDir, Qt::CaseInsensitive) == 0)
        return;   // picked the folder's own current location: nothing to do

    // The Explorer semantics: changing the location asks whether the files
    // should follow. Nothing to ask when the folder is empty or missing.
    int choice = 1;   // 1 = move the files
    if (!oldDir.isEmpty() && !QDir(oldDir).isEmpty()) {
        choice = askMoveFolderDialog(this, oldDir, newDir);
        if (choice == 0)
            return;
    }

    if (!updateAppPath(app.id, newDir)) {
        Dialogs::warn(this, I18n::tr("app_sync.title"), I18n::tr("app_sync.fail"));
        return;
    }
    app.currentPath = newDir;
    refreshTable();

    // "Move the files" goes STRAIGHT into the progress — the location change
    // and the move are one operation to the user, so the completion notice
    // (passed to moveContentsAsync) is the only popup after the confirmation.
    // "Location only" keeps the plain single success popup.
    if (choice == 1 && !oldDir.isEmpty())
        moveContentsAsync({{oldDir, newDir}}, I18n::tr("app_sync.success"), newDir);
    else
        Dialogs::info(this, I18n::tr("app_sync.title"), I18n::tr("app_sync.success"));
}

void AppPathSyncDialog::onBatchSetTargetFolder()
{
    QString startPath = m_suggestedPath.isEmpty() ? QDir::homePath() : m_suggestedPath;
    QString targetRoot = QFileDialog::getExistingDirectory(this, I18n::tr("app_sync.select_target_root"), startPath);
    if (targetRoot.isEmpty())
        return;

    targetRoot = QDir::toNativeSeparators(targetRoot);
    // Shared with the move tab: both answer "where does this machine keep its
    // data", so the next picker — whichever tab opens it — starts here.
    if (m_movePanel)
        m_movePanel->rememberTargetRoot(targetRoot);

    // One question covers every folder, same semantics as the single change:
    // the files either follow their folders or stay behind.
    const bool moveFiles = Dialogs::confirm(this, I18n::tr("app_sync.move_dlg_title"),
                                            I18n::tr("app_sync.batch_move_ask"));

    QVector<QPair<QString, QString>> moves;
    int count = 0;
    for (auto& app : m_apps) {
        QString subDir = targetRoot + QLatin1Char('\\') + app.folder;
        const QString oldDir = QDir(app.currentPath).exists()
            ? QDir::toNativeSeparators(QDir(app.currentPath).absolutePath()) : QString();
        if (updateAppPath(app.id, subDir)) {
            app.currentPath = subDir;
            count++;
            if (moveFiles && !oldDir.isEmpty() && !QDir(oldDir).isEmpty()
                && oldDir.compare(QDir::toNativeSeparators(subDir), Qt::CaseInsensitive) != 0) {
                moves.append({oldDir, QDir::toNativeSeparators(subDir)});
            }
        }
    }

    refreshTable();
    if (count == 0) {
        Dialogs::warn(this, I18n::tr("app_sync.title"), I18n::tr("app_sync.batch_none"));
        return;
    }
    // Same one-step rule as the single change: when the files follow their
    // folders the progress dialog takes over and the summary is only shown
    // once the move is done; without a move the summary is the only popup.
    if (!moves.isEmpty())
        moveContentsAsync(moves,
                          I18n::tr("app_sync.batch_done",
                                   QMap<QString, QString>{{"count", QString::number(count)}}),
                          targetRoot);
    else
        Dialogs::info(this, I18n::tr("app_sync.title"),
            I18n::tr("app_sync.batch_done", QMap<QString, QString>{{"count", QString::number(count)}}));
}

bool AppPathSyncDialog::updateAppPath(const QString& appId, const QString& newPath)
{
    // Every row is one of Windows' own library folders, and the table holds the
    // registry value that defines where it lives.
    const KnownFolderEntry* entry = knownFolderById(appId);
    if (!entry)
        return false;
    const QString valueName = entry->regValue;
    if (valueName.isEmpty())
        return false;

    const QString native = normalizedDir(newPath);

#ifdef _WIN32
    // What Explorer's Location tab calls: the registry value is rewritten and
    // the running session picks the change up immediately, so "实时生效" is
    // real rather than a promise of the next logon.
    if (setKnownFolderPathRealtime(appId, native))
        return true;
#endif

    // Fallback: write the registry value directly. The change then applies at
    // the next logon or Explorer restart, like a plain regedit would.
    QSettings reg(kUserShellFolders, QSettings::NativeFormat);
    reg.setValue(valueName, native);
    return reg.status() == QSettings::NoError;
}

void AppPathSyncDialog::moveContentsAsync(const QVector<QPair<QString, QString>>& pairs,
                                          const QString& doneNotice,
                                          const QString& markerDir, int closeRound)
{
    if (pairs.isEmpty())
        return;

    // The progress denominator: everything the folders hold right now, before
    // anything moves. Measured without descending into reparse points, which
    // is exactly the set the mover refuses to touch too.
    qint64 total = 0;
    for (const auto& p : pairs)
        total += WinApi::dirSizeNoReparse(p.first);
    // Nothing measurable left — an empty tree, or one holding nothing but the
    // reparse points the mover will not touch. There is no progress to report,
    // but there is still an outcome to explain, so it is reported right away
    // instead of the window going quiet.
    if (total <= 0) {
        reportMoveOutcome(pairs, doneNotice, markerDir, closeRound);
        return;
    }

    auto* progress = new QProgressDialog(
        I18n::tr("app_sync.move_moving", QMap<QString, QString>{
            {"done", QStringLiteral("0")}, {"total", QString::number(total)}}),
        I18n::tr("button.cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setWindowTitle(I18n::tr("app_sync.move_dlg_title"));
    progress->setMinimumDuration(0);
    progress->setValue(0);

    // Shared with the worker thread: the dialog (and even this whole dialog
    // window) can go away while the move keeps going, and the counters must
    // outlive both.
    auto copied = std::make_shared<std::atomic<qint64>>(0);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto pairsCopy = pairs;

    connect(progress, &QProgressDialog::canceled, progress, [cancelled]() {
        cancelled->store(true);
    });

    // Polls the shared counter; parented to the progress dialog so it dies
    // with it and cannot tick into a dangling label.
    auto* timer = new QTimer(progress);
    connect(timer, &QTimer::timeout, progress, [progress, total, copied]() {
        const qint64 done = qMin(copied->load(), total);
        progress->setValue(total > 0 ? static_cast<int>(done * 100 / total) : 100);
        progress->setLabelText(I18n::tr("app_sync.move_moving", QMap<QString, QString>{
            {"done", humanSize(done)},
            {"total", humanSize(total)}}));
    });
    timer->start(200);

    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this,
            [this, progress, watcher, cancelled, pairsCopy, doneNotice, markerDir, closeRound]() {
                progress->close();
                progress->deleteLater();
                watcher->deleteLater();
                if (cancelled->load())
                    return;
                // The move has landed (fully or partly) at the destination:
                // that folder is now a live data home, so the warning marker
                // goes in whatever happened.
                if (!markerDir.isEmpty())
                    WinApi::writeDontDeleteMarker(markerDir);
                reportMoveOutcome(pairsCopy, doneNotice, markerDir, closeRound);
            });
    watcher->setFuture(QtConcurrent::run([pairsCopy, copied, cancelled]() {
        for (const auto& p : pairsCopy) {
            if (cancelled->load())
                break;
            moveContentsRecursive(p.first, p.second, copied.get(), cancelled.get());
        }
    }));
}

void AppPathSyncDialog::reportMoveOutcome(const QVector<QPair<QString, QString>>& pairs,
                                          const QString& doneNotice,
                                          const QString& markerDir, int closeRound)
{
    // Whatever is still sitting in an old folder is what did not make it.
    // Its presence is not an error in itself — Explorer leaves files behind the
    // same way — but WHICH files, and who is holding them, has to reach the user.
    QVector<QPair<QString, QString>> left;
    for (const auto& p : pairs) {
        if (QDir(p.first).exists() && !QDir(p.first).isEmpty())
            left.append(p);
    }

    if (left.isEmpty()) {
        // One popup after the move: the completion summary the caller handed in
        // (it deliberately skipped its pre-move success box).
        if (!doneNotice.isEmpty())
            Dialogs::info(this, I18n::tr("app_sync.title"), doneNotice);
        return;
    }

    if (closeRound < kMaxCloseRounds) {
        // Ask who is holding them, and let the user clear it. Naming the program
        // is the whole difference between "some files stayed behind" and a user
        // who can actually do something about it.
        QVector<WinApi::LockingProcess> procs;
        for (const auto& p : left) {
            QVector<WinApi::LockingProcess> found;
            WinApi::processesLockingDir(p.first, &found, 400);
            for (const WinApi::LockingProcess& lp : found) {
                bool seen = false;
                for (const WinApi::LockingProcess& already : procs)
                    if (already.pid == lp.pid)
                        seen = true;
                if (!seen)
                    procs.append(lp);
            }
        }

        if (!procs.isEmpty()) {
            // Nothing closable means nothing to offer: the only honest thing is
            // the report at the end of this function, which names them.
            bool closable = false;
            for (const WinApi::LockingProcess& lp : procs)
                if (lp.safeToClose)
                    closable = true;
            if (closable) {
                // No "skip" button here: the location change has already
                // happened, so there is no folder to skip — either the files
                // follow, or they stay where they are.
                const LockerDialog::Answer answer = LockerDialog::ask(
                    this, I18n::tr("app_sync.title"),
                    left.size() == 1
                        ? I18n::tr(QStringLiteral("app_sync.locked_intro"),
                                   QMap<QString, QString>{{"name", left.first().first}})
                        : I18n::tr(QStringLiteral("app_sync.locked_intro_many"),
                                   QMap<QString, QString>{{"count", QString::number(left.size())}}),
                    procs,
                    I18n::tr(QStringLiteral("app_sync.btn_close_retry")),
                    QString(), /*allowSkip=*/false);

                if (answer == LockerDialog::CloseAndContinue) {
                    QApplication::setOverrideCursor(Qt::WaitCursor);
                    const QVector<WinApi::CloseOutcome> outcomes =
                        WinApi::closeProcesses(procs);
                    QApplication::restoreOverrideCursor();

                    const QString report = LockerDialog::outcomeText(outcomes);
                    if (!report.isEmpty())
                        Logger::info(QStringLiteral("[app_sync] programs holding the old "
                                                   "locations:\n%1").arg(report));

                    if (LockerDialog::anyClosed(outcomes)) {
                        // moveContentsRecursive is a plain "move what is still
                        // here" walk, so running it again IS the retry: it picks
                        // up exactly the files that stayed behind, and nothing
                        // else.
                        moveContentsAsync(left, doneNotice, markerDir, closeRound + 1);
                        return;
                    }
                    // Nothing went away. Asking again would get the same answer,
                    // so the round ends with what could not be done — which is
                    // what the report below says.
                }
            }
        }
    }

    Dialogs::warn(this, I18n::tr("app_sync.title"),
                  closeRound == 0 ? I18n::tr("app_sync.move_partial")
                                  : I18n::tr("app_sync.retry_give_up"));
}
