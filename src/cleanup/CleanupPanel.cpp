#include "CleanupPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QFont>
#include <QColor>
#include <QMap>
#include <QVariantList>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QLabel>
#include <QTreeWidget>

#include <algorithm>
#include <stack>

#include "Style.h"
#include "I18n.h"
#include "FormatHelpers.h"

// --------------------------------------------------------------------------- //
// File-local helpers
// --------------------------------------------------------------------------- //
namespace {

// Danger level colors.
//   S: green   - safe auto-clean
//   A: yellow  - user cache, default delete
//   B: blue    - potentially useful, opt-in
//   C: red     - dangerous, disabled
//   D: gray    - user data, hidden
const char* const DANGER_COLORS[] = {
    "#22c55e",  // S
    "#eab308",  // A
    "#3b82f6",  // B
    "#ef4444",  // C
    "#6b7280",  // D
};

QString dangerLevelToString(DangerLevel lvl)
{
    switch (lvl) {
        case DangerLevel::S: return QStringLiteral("S");
        case DangerLevel::A: return QStringLiteral("A");
        case DangerLevel::B: return QStringLiteral("B");
        case DangerLevel::C: return QStringLiteral("C");
        case DangerLevel::D: return QStringLiteral("D");
    }
    return QString();
}

int dangerLevelOrder(DangerLevel lvl)
{
    switch (lvl) {
        case DangerLevel::S: return 0;
        case DangerLevel::A: return 1;
        case DangerLevel::B: return 2;
        case DangerLevel::C: return 3;
        case DangerLevel::D: return 4;
    }
    return 9;
}

QString dangerColor(DangerLevel lvl)
{
    int idx = static_cast<int>(lvl);
    if (idx >= 0 && idx < 5)
        return QString::fromLatin1(DANGER_COLORS[idx]);
    return QStringLiteral("#6b7280");
}

// QTreeWidgetItem subclass that uses stored sort data for comparison, so
// numeric columns (size, count) and the level badge sort correctly instead of
// falling back to lexicographic text comparison.
// SortableTreeWidgetItem.
class SortableTreeWidgetItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    void setSortData(int column, const QVariant& value)
    {
        m_sortData[column] = value;
    }

    QVariant sortData(int column) const
    {
        auto it = m_sortData.constFind(column);
        return it != m_sortData.constEnd() ? it.value() : QVariant();
    }

    bool operator<(const QTreeWidgetItem& other) const override
    {
        int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        const auto* o = dynamic_cast<const SortableTreeWidgetItem*>(&other);
        if (o) {
            QVariant v1 = sortData(col);
            QVariant v2 = o->sortData(col);
            if (v1.isValid() && v2.isValid()) {
                // Numeric comparison when both values look like numbers.
                bool ok1 = false, ok2 = false;
                qlonglong n1 = v1.toLongLong(&ok1);
                qlonglong n2 = v2.toLongLong(&ok2);
                if (ok1 && ok2)
                    return n1 < n2;
                // Fallback: string comparison.
                return v1.toString() < v2.toString();
            }
        }
        return QTreeWidgetItem::operator<(other);
    }

private:
    QMap<int, QVariant> m_sortData;
};

} // namespace

// --------------------------------------------------------------------------- //
// Constructor
// --------------------------------------------------------------------------- //
CleanupPanel::CleanupPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

// --------------------------------------------------------------------------- //
// UI construction
// --------------------------------------------------------------------------- //
void CleanupPanel::buildUI()
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);

    // ── Progress bar (hidden by default) ─────────────────────────
    m_scanProgress = new QProgressBar;
    m_scanProgress->setRange(0, 0);
    m_scanProgress->setFixedHeight(4);
    m_scanProgress->setVisible(false);
    lay->addWidget(m_scanProgress);

    // ── Tab widget: Categories + Large Files ─────────────────────
    m_tabs = new QTabWidget;
    m_tabs->setObjectName("cleanup_tabs");

    // Tab 1: Category targets (S/A/B/C/D)
    {
        auto* catTab = new QWidget;
        auto* catLay = new QVBoxLayout(catTab);
        catLay->setContentsMargins(0, 5, 0, 4);
        catLay->setSpacing(4);

        m_catSelBtn = new QPushButton(I18n::tr("cleanup.select_all"));
        m_catSelBtn->setObjectName("ghost");
        m_catSelBtn->setCursor(Qt::PointingHandCursor);
        m_catSelBtn->setFixedHeight(24);
        m_catSelBtn->setCheckable(true);
        connect(m_catSelBtn, &QPushButton::clicked,
                this, &CleanupPanel::onCatSelectAllToggled);

        m_catTree = makeTree({
            I18n::tr("cleanup.col_level"),
            I18n::tr("cleanup.col_category"),
            I18n::tr("cleanup.col_size"),
            I18n::tr("cleanup.col_items"),
            I18n::tr("cleanup.col_remark"),
        }, true);

        auto* catBar = new QFrame;
        catBar->setFixedHeight(28);
        auto* catHdr = new QHBoxLayout(catBar);
        catHdr->setContentsMargins(0, 1, 0, 1);
        catHdr->setSpacing(8);
        catHdr->addStretch(1);
        catHdr->addWidget(m_catSelBtn);
        catLay->addWidget(catBar);
        catLay->addSpacing(4);
        catLay->addWidget(m_catTree, 1);

        m_tabs->addTab(catTab, I18n::tr("cleanup.tab_categories"));
    }

    // Tab 2: Large files
    {
        auto* lfTab = new QWidget;
        auto* lfLay = new QVBoxLayout(lfTab);
        lfLay->setContentsMargins(0, 5, 0, 4);
        lfLay->setSpacing(4);

        m_lfSelBtn = new QPushButton(I18n::tr("cleanup.select_all"));
        m_lfSelBtn->setObjectName("ghost");
        m_lfSelBtn->setCursor(Qt::PointingHandCursor);
        m_lfSelBtn->setFixedHeight(24);
        m_lfSelBtn->setCheckable(true);
        connect(m_lfSelBtn, &QPushButton::clicked,
                this, &CleanupPanel::onLfSelectAllToggled);

        m_lfTree = makeTree({
            I18n::tr("cleanup.col_level"),
            I18n::tr("cleanup.col_name"),
            I18n::tr("cleanup.col_size"),
            I18n::tr("cleanup.col_path"),
            I18n::tr("cleanup.col_warning"),
            I18n::tr("cleanup.col_type"),
        });
        // Col 5 (Type): fixed width, fits short labels like "Image", "Video".
        m_lfTree->header()->setSectionResizeMode(5, QHeaderView::Fixed);
        m_lfTree->header()->resizeSection(5, 90);

        m_lfTypeFilter = new QComboBox;
        m_lfTypeFilter->setObjectName("typeFilter");
        m_lfTypeFilter->setFixedHeight(22);
        m_lfTypeFilter->setMinimumWidth(120);
        connect(m_lfTypeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &CleanupPanel::onLfTypeFilterChanged);

        m_lfTypeLabel = new QLabel(I18n::tr("cleanup.filter_type"));
        auto* lfBar = new QFrame;
        lfBar->setFixedHeight(28);
        auto* lfHdr = new QHBoxLayout(lfBar);
        lfHdr->setContentsMargins(0, 1, 0, 1);
        lfHdr->setSpacing(8);
        lfHdr->addStretch(1);
        lfHdr->addWidget(m_lfTypeLabel);
        lfHdr->addWidget(m_lfTypeFilter);
        lfHdr->addWidget(m_lfSelBtn);
        lfLay->addWidget(lfBar);
        lfLay->addSpacing(4);
        lfLay->addWidget(m_lfTree, 1);

        m_tabs->addTab(lfTab, I18n::tr("cleanup.tab_large_files"));
    }

    // Tab 3: Duplicate files (grouped, two-level tree: group → files)
    {
        auto* dupTab = new QWidget;
        auto* dupLay = new QVBoxLayout(dupTab);
        dupLay->setContentsMargins(0, 5, 0, 4);
        dupLay->setSpacing(4);

        m_dupSelBtn = new QPushButton(I18n::tr("cleanup.select_all"));
        m_dupSelBtn->setObjectName("ghost");
        m_dupSelBtn->setCursor(Qt::PointingHandCursor);
        m_dupSelBtn->setFixedHeight(24);
        m_dupSelBtn->setCheckable(true);
        connect(m_dupSelBtn, &QPushButton::clicked,
                this, &CleanupPanel::onDupSelectAllToggled);

        m_dupTree = makeTree({
            I18n::tr("cleanup.col_level"),
            I18n::tr("cleanup.col_dup_group"),
            I18n::tr("cleanup.col_size"),
            I18n::tr("cleanup.col_dup_wasted"),
            I18n::tr("cleanup.col_paths"),
            I18n::tr("cleanup.col_type"),
        });
        // Two-level tree: top-level = group, children = files in the group.
        m_dupTree->setRootIsDecorated(true);
        // Child rows need room for: indentation + checkbox + level badge.
        // makeTree sets col 0 to 56px (enough only for flat rows); widen it
        // so the checkbox and badge are both visible on indented child rows.
        m_dupTree->setIndentation(20);
        m_dupTree->header()->resizeSection(0, 96);
        // Col 5 (Type): fixed width, fits short labels like "Image", "Video".
        m_dupTree->header()->setSectionResizeMode(5, QHeaderView::Fixed);
        m_dupTree->header()->resizeSection(5, 90);

        m_dupTypeFilter = new QComboBox;
        m_dupTypeFilter->setObjectName("typeFilter");
        m_dupTypeFilter->setFixedHeight(22);
        m_dupTypeFilter->setMinimumWidth(120);
        connect(m_dupTypeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &CleanupPanel::onDupTypeFilterChanged);

        m_dupTypeLabel = new QLabel(I18n::tr("cleanup.filter_type"));
        auto* dupBar = new QFrame;
        dupBar->setFixedHeight(28);
        auto* dupHdr = new QHBoxLayout(dupBar);
        dupHdr->setContentsMargins(0, 1, 0, 1);
        dupHdr->setSpacing(8);
        dupHdr->addStretch(1);
        dupHdr->addWidget(m_dupTypeLabel);
        dupHdr->addWidget(m_dupTypeFilter);
        dupHdr->addWidget(m_dupSelBtn);
        dupLay->addWidget(dupBar);
        dupLay->addSpacing(4);
        dupLay->addWidget(m_dupTree, 1);

        m_tabs->addTab(dupTab, I18n::tr("cleanup.tab_duplicates"));
    }

    // Connect item-change signals so the selected-size label and select-all
    // button state stay in sync when the user toggles checkboxes.
    connect(m_catTree, &QTreeWidget::itemChanged, this, &CleanupPanel::onCatItemChanged);
    connect(m_lfTree, &QTreeWidget::itemChanged, this, &CleanupPanel::onLfItemChanged);
    connect(m_lfTree, &QTreeWidget::itemClicked, this, &CleanupPanel::onLfItemClicked);
    connect(m_dupTree, &QTreeWidget::itemChanged, this, &CleanupPanel::onDupItemChanged);
    connect(m_dupTree, &QTreeWidget::itemClicked, this, &CleanupPanel::onDupItemClicked);
    // Double-click a category to view its contents (path + file listing).
    connect(m_catTree, &QTreeWidget::itemDoubleClicked,
            this, &CleanupPanel::onCatItemDoubleClicked);

    lay->addWidget(m_tabs, 1);

    // ── Bottom: total label + selected label + rescan + clean ────
    auto* bottom = new QFrame;
    auto* blay = new QHBoxLayout(bottom);
    blay->setContentsMargins(8, 4, 8, 4);
    m_totalLabel = new QLabel(QString());
    m_totalLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(QString::fromLatin1(C::TEXT_MUTED())));
    blay->addWidget(m_totalLabel);
    blay->addSpacing(16);
    m_selectedLabel = new QLabel(I18n::tr("cleanup.selected",
        QMap<QString, QString>{{"size", QStringLiteral("0 B")}, {"count", QStringLiteral("0")}}));
    m_selectedLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(QString::fromLatin1(C::TEXT_SEC())));
    blay->addWidget(m_selectedLabel);
    blay->addStretch(1);
    m_rescanBtn = new QPushButton(I18n::tr("cleanup.rescan"));
    m_rescanBtn->setObjectName("ghost");
    m_rescanBtn->setCursor(Qt::PointingHandCursor);
    connect(m_rescanBtn, &QPushButton::clicked, this, [this]() { emit rescanRequested(); });
    blay->addWidget(m_rescanBtn);
    m_cleanBtn = new QPushButton(I18n::tr("cleanup.clean_selected"));
    m_cleanBtn->setObjectName("danger");
    m_cleanBtn->setCursor(Qt::PointingHandCursor);
    m_cleanBtn->setEnabled(false);
    connect(m_cleanBtn, &QPushButton::clicked, this, &CleanupPanel::onCleanClicked);
    blay->addWidget(m_cleanBtn);
    lay->addWidget(bottom);

    // Update total/selected labels when the active tab changes.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        updateTotalLabel();
        updateSelectedLabel();
    });
}

QTreeWidget* CleanupPanel::makeTree(const QStringList& headers, bool col3Fixed)
{
    auto* tree = new QTreeWidget;
    tree->setObjectName("filelist");
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    tree->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto* hdr = tree->header();
    hdr->setSectionResizeMode(0, QHeaderView::Fixed);
    hdr->resizeSection(0, 56);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionResizeMode(2, QHeaderView::Fixed);
    hdr->resizeSection(2, 72);
    if (col3Fixed) {
        hdr->setSectionResizeMode(3, QHeaderView::Fixed);
        hdr->resizeSection(3, 56);
    } else {
        hdr->setSectionResizeMode(3, QHeaderView::Stretch);
    }
    hdr->setSectionResizeMode(4, QHeaderView::Stretch);
    hdr->setSectionsClickable(true);
    hdr->setStretchLastSection(true);
    tree->setSortingEnabled(true);
    return tree;
}

// --------------------------------------------------------------------------- //
// Public API
// --------------------------------------------------------------------------- //
void CleanupPanel::startScanProgress()
{
    m_scanProgress->setVisible(true);
    m_catTree->setSortingEnabled(false);
    m_lfTree->setSortingEnabled(false);
    m_dupTree->setSortingEnabled(false);
    m_catTree->clear();
    m_lfTree->clear();
    m_dupTree->clear();
    m_targets.clear();
    m_largeFiles.clear();
    m_duplicateGroups.clear();
    m_cleanBtn->setEnabled(false);
}

void CleanupPanel::stopScanProgress()
{
    m_scanProgress->setVisible(false);
    m_catTree->setSortingEnabled(true);
    m_lfTree->setSortingEnabled(true);
    updateSelectedLabel();
    m_cleanBtn->setEnabled(true);
}

void CleanupPanel::addTarget(const CleanupTarget& target)
{
    m_targets.push_back(target);
    QString lvl = dangerLevelToString(target.danger);
    QString color = dangerColor(target.danger);

    auto* item = new SortableTreeWidgetItem();
    // Store (key, path, level) for later cleanup identification.
    item->setData(0, Qt::UserRole, QVariantList{target.key, target.path, lvl});
    item->setSortData(0, dangerLevelOrder(target.danger));
    item->setSortData(1, I18n::tr(target.key));
    item->setSortData(2, target.size);
    item->setSortData(3, target.fileCount);
    item->setSortData(4, target.remark.isEmpty() ? QString() : I18n::tr(target.remark));

    // Col 0: Level badge
    item->setText(0, QStringLiteral(" %1 ").arg(lvl));
    item->setForeground(0, QColor(color));
    QFont f0 = item->font(0);
    f0.setBold(true);
    f0.setPointSize(10);
    item->setFont(0, f0);

    // Col 1: Category name
    QString catName = I18n::tr(target.key);
    item->setText(1, catName);
    // Tooltip: category name + hint that double-click shows details.
    item->setToolTip(1, catName + QStringLiteral("\n\n") +
                     I18n::tr("cleanup.dblclick_hint"));

    // Col 2: Size
    QString sizeText = target.size > 0 ? humanSize(target.size) : QStringLiteral("\u2014");
    item->setText(2, sizeText);
    item->setToolTip(2, sizeText);
    item->setTextAlignment(2, Qt::AlignRight);
    item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC())));

    // Col 3: Item count
    QString countText = target.fileCount > 0 ? humanCount(target.fileCount)
                                              : QStringLiteral("\u2014");
    item->setText(3, countText);
    item->setToolTip(3, countText);
    item->setTextAlignment(3, Qt::AlignRight);
    item->setForeground(3, QColor(QString::fromLatin1(C::TEXT_MUTED())));

    // Col 4: Remark
    QString remark = target.remark.isEmpty() ? QString() : I18n::tr(target.remark);
    item->setText(4, remark);
    item->setToolTip(4, remark);
    item->setForeground(4, QColor(QString::fromLatin1(C::TEXT_MUTED())));

    // Checkbox: enabled targets are user-checkable; disabled (C level) are
    // shown with an unchecked, non-toggleable checkbox and a grayed name.
    if (target.enabled) {
        item->setCheckState(0, target.checked ? Qt::Checked : Qt::Unchecked);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    } else {
        item->setCheckState(0, Qt::Unchecked);
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED())));
    }

    if (target.size == 0)
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED())));

    m_catTree->addTopLevelItem(item);
}

void CleanupPanel::addLargeFile(const LargeFile& lf)
{
    m_largeFiles.push_back(lf);
    QString lvl = dangerLevelToString(lf.danger);
    QString color = dangerColor(lf.danger);

    auto* item = new SortableTreeWidgetItem();
    item->setData(0, Qt::UserRole, lf.path);
    item->setSortData(0, dangerLevelOrder(lf.danger));
    item->setSortData(1, lf.name);
    item->setSortData(2, lf.size);
    item->setSortData(3, lf.path);
    item->setSortData(4, largeFileWarning(lvl));
    // Store the type KEY (e.g. "type.document") — language-independent — so
    // the filter and sort survive retranslate(); the visible text is set
    // from the translated label below and refreshed in retranslate().
    const QString tKey = typeKey(lf.name);
    item->setSortData(5, tKey);

    // Col 0: Level badge
    item->setText(0, QStringLiteral(" %1 ").arg(lvl));
    item->setForeground(0, QColor(color));
    QFont f0 = item->font(0);
    f0.setBold(true);
    f0.setPointSize(10);
    item->setFont(0, f0);

    // Col 1: Name
    item->setText(1, lf.name);
    item->setToolTip(1, lf.name);

    // Col 2: Size
    QString sizeText = humanSize(lf.size);
    item->setText(2, sizeText);
    item->setToolTip(2, sizeText);
    item->setTextAlignment(2, Qt::AlignRight);
    item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC())));

    // Col 3: Path (clickable, reveals in Explorer)
    item->setText(3, lf.path);
    item->setForeground(3, QColor(QString::fromLatin1(C::PRIMARY())));
    item->setToolTip(3, lf.path);

    // Col 4: Warning text + color by level
    if (lvl == QLatin1String("C") || lvl == QLatin1String("D")) {
        applyLargeFileWarning(item, lvl);
        item->setForeground(4, QColor(QStringLiteral("#ef4444")));
    } else if (lvl == QLatin1String("A")) {
        applyLargeFileWarning(item, lvl);
        item->setForeground(4, QColor(QString::fromLatin1(C::TEXT_MUTED())));
    } else if (lvl == QLatin1String("B")) {
        applyLargeFileWarning(item, lvl);
        item->setForeground(4, QColor(QStringLiteral("#eab308")));
    }

    // Col 5: File type label (Document/Image/Video/...) with type color.
    item->setText(5, I18n::tr(tKey.toLatin1().constData()));
    item->setForeground(5, QColor(typeColor(lf.name)));

    // C/D level: checkbox disabled (not user-checkable); others checkable.
    if (lvl == QLatin1String("C") || lvl == QLatin1String("D")) {
        item->setCheckState(0, Qt::Unchecked);
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED())));
    } else {
        item->setCheckState(0, Qt::Unchecked);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    }

    m_lfTree->addTopLevelItem(item);
}

void CleanupPanel::addDuplicateGroup(const DuplicateGroup& group)
{
    int groupIdx = static_cast<int>(m_duplicateGroups.size());
    m_duplicateGroups.push_back(group);

    const DangerLevel dl = DangerLevel::B;  // duplicates are always B-level
    const QString lvl = dangerLevelToString(dl);
    const QString color = dangerColor(dl);
    const QString groupLabel = I18n::tr("cleanup.dup_group_label",
        QMap<QString, QString>{{"count", QString::number(group.files.size())}});
    const QString sizeText = humanSize(group.size);
    const QString wastedText = humanSize(group.wastedSpace());

    // Top-level row = the group.
    auto* topItem = new SortableTreeWidgetItem();
    // Store group index in UserRole for later lookup.
    topItem->setData(0, Qt::UserRole, groupIdx);
    topItem->setSortData(0, dangerLevelOrder(dl));
    topItem->setSortData(2, group.size);
    topItem->setSortData(3, group.wastedSpace());
    // Col 5 (Type): use the first file's type key so the group sorts by type
    // when the user clicks the Type column header. Without this, sortData(5)
    // is invalid and sorting falls back to empty-text comparison → no effect.
    if (!group.files.empty())
        topItem->setSortData(5, typeKey(group.files.front().name));

    topItem->setText(0, QStringLiteral(" %1 ").arg(lvl));
    topItem->setForeground(0, QColor(color));
    QFont f0 = topItem->font(0);
    f0.setBold(true);
    f0.setPointSize(10);
    topItem->setFont(0, f0);

    topItem->setText(1, groupLabel);
    topItem->setToolTip(1, groupLabel);

    topItem->setText(2, sizeText);
    topItem->setToolTip(2, sizeText);
    topItem->setTextAlignment(2, Qt::AlignRight);
    topItem->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC())));

    topItem->setText(3, wastedText);
    topItem->setToolTip(3, wastedText);
    topItem->setTextAlignment(3, Qt::AlignRight);
    topItem->setForeground(3, QColor(QStringLiteral("#eab308")));

    topItem->setText(4, QString());
    // Top-level group row is not checkable (only individual files are).
    topItem->setFlags(topItem->flags() & ~Qt::ItemIsUserCheckable);

    m_dupTree->addTopLevelItem(topItem);

    // Child rows = individual files in the group.
    // Every file is checkable; none is checked by default. The user picks
    // which copies to delete (may be any subset, including all of them).
    for (int i = 0; i < static_cast<int>(group.files.size()); ++i) {
        const auto& df = group.files[i];
        auto* child = new SortableTreeWidgetItem(topItem);
        child->setData(0, Qt::UserRole, df.path);
        child->setSortData(2, df.size);

        // Col 0: per-file danger level badge (S/A/B/C/D).
        // The checkbox renders left of the badge text.
        const QString childLvl = dangerLevelToString(df.danger);
        const QString childColor = dangerColor(df.danger);
        child->setText(0, QStringLiteral(" %1 ").arg(childLvl));
        child->setForeground(0, QColor(childColor));
        child->setSortData(0, dangerLevelOrder(df.danger));

        child->setText(1, df.name);
        child->setToolTip(1, df.name);

        child->setText(2, humanSize(df.size));
        child->setTextAlignment(2, Qt::AlignRight);
        child->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC())));

        child->setText(3, QString());  // wasted only shown on group row
        child->setText(4, df.path);
        child->setForeground(4, QColor(QString::fromLatin1(C::PRIMARY())));
        child->setToolTip(4, df.path);

        // Col 5: File type label with type color. Store the language-
        // independent type key for filter/sort; text is refreshed in
        // retranslate().
        const QString tKey = typeKey(df.name);
        child->setText(5, I18n::tr(tKey.toLatin1().constData()));
        child->setForeground(5, QColor(typeColor(df.name)));
        child->setSortData(5, tKey);

        // All files are checkable and unchecked by default.
        // IMPORTANT: set ItemIsUserCheckable flag BEFORE setCheckState,
        // otherwise the checkbox is not rendered on child items.
        child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
        child->setCheckState(0, Qt::Unchecked);
    }
    topItem->setExpanded(true);
}

void CleanupPanel::loadDuplicates(const std::vector<DuplicateGroup>& groups)
{
    m_dupTree->setSortingEnabled(false);
    m_dupTree->clear();
    m_duplicateGroups.clear();
    for (const auto& g : groups)
        addDuplicateGroup(g);
    repopulateTypeFilters();
    m_dupTree->setSortingEnabled(true);
    updateSelectedLabel();
}

void CleanupPanel::loadTargets(const std::vector<CleanupTarget>& targets,
                                const std::vector<LargeFile>& largeFiles,
                                qint64 freeBytes, qint64 totalBytes)
{
    m_catTree->setSortingEnabled(false);
    m_lfTree->setSortingEnabled(false);
    m_catTree->clear();
    m_lfTree->clear();
    m_targets.clear();
    m_largeFiles.clear();

    // Sort targets: S first, then A, B, C; within each level, larger size first.
    std::vector<CleanupTarget> sortedTargets = targets;
    std::sort(sortedTargets.begin(), sortedTargets.end(),
              [](const CleanupTarget& a, const CleanupTarget& b) {
                  int oa = dangerLevelOrder(a.danger);
                  int ob = dangerLevelOrder(b.danger);
                  if (oa != ob)
                      return oa < ob;
                  return a.size > b.size;
              });

    for (const auto& t : sortedTargets) {
        if (t.danger == DangerLevel::D)
            continue;  // D-level hidden
        addTarget(t);
    }

    for (const auto& lf : largeFiles) {
        // C-level (system) and D-level (user data) are hidden from large files.
        if (lf.danger == DangerLevel::D || lf.danger == DangerLevel::C)
            continue;
        addLargeFile(lf);
    }

    repopulateTypeFilters();
    updateSummary(freeBytes, totalBytes);
    updateSelectedLabel();
    m_catTree->setSortingEnabled(true);
    m_lfTree->setSortingEnabled(true);
    m_cleanBtn->setEnabled(true);
}

void CleanupPanel::removeCleanedItems(const std::vector<CleanupWorker::ItemRef>& items)
{
    for (const auto& ref : items) {
        if (ref.type == QLatin1String("target")) {
            for (int i = m_catTree->topLevelItemCount() - 1; i >= 0; --i) {
                auto* item = m_catTree->topLevelItem(i);
                QVariantList data = item->data(0, Qt::UserRole).toList();
                if (data.size() >= 2 &&
                    data[0].toString() == ref.key &&
                    data[1].toString() == ref.path) {
                    m_catTree->takeTopLevelItem(i);
                    break;
                }
            }
        } else if (ref.type == QLatin1String("file")) {
            for (int i = m_lfTree->topLevelItemCount() - 1; i >= 0; --i) {
                auto* item = m_lfTree->topLevelItem(i);
                if (item->data(0, Qt::UserRole).toString() == ref.path) {
                    m_lfTree->takeTopLevelItem(i);
                    break;
                }
            }
            // Also remove from the duplicates tree (child rows store paths).
            for (int gi = m_dupTree->topLevelItemCount() - 1; gi >= 0; --gi) {
                auto* groupItem = m_dupTree->topLevelItem(gi);
                for (int ci = groupItem->childCount() - 1; ci >= 0; --ci) {
                    auto* child = groupItem->child(ci);
                    if (child->data(0, Qt::UserRole).toString() == ref.path) {
                        groupItem->removeChild(child);
                        delete child;
                        break;
                    }
                }
                // If ≤1 file remains, the group is no longer a duplicate set.
                if (groupItem->childCount() <= 1) {
                    m_dupTree->takeTopLevelItem(gi);
                    delete groupItem;
                }
            }
        }
    }
    updateSelectedLabel();
}

void CleanupPanel::setCleaning(bool cleaning)
{
    m_cleaning = cleaning;
    m_cleanBtn->setEnabled(!cleaning);
    m_cleanBtn->setText(cleaning ? I18n::tr("cleanup.cleaning")
                                  : I18n::tr("cleanup.clean_selected"));
}

void CleanupPanel::updateFreeSpace(qint64 freeBytes, qint64 totalBytes)
{
    updateSummary(freeBytes, totalBytes);
}

// --------------------------------------------------------------------------- //
// Private helpers
// --------------------------------------------------------------------------- //
void CleanupPanel::applyTargetTranslation(QTreeWidgetItem* item,
                                           const CleanupTarget& target)
{
    QString catName = I18n::tr(target.key);
    QString remark = target.remark.isEmpty() ? QString() : I18n::tr(target.remark);
    if (auto* s = dynamic_cast<SortableTreeWidgetItem*>(item)) {
        s->setSortData(1, catName);
        s->setSortData(4, remark);
    }
    item->setText(1, catName);
    item->setToolTip(1, catName + QStringLiteral("\n\n") +
                     I18n::tr("cleanup.dblclick_hint"));
    item->setText(4, remark);
    item->setToolTip(4, remark);
}

QString CleanupPanel::largeFileWarning(const QString& level) const
{
    if (level == QLatin1String("C") || level == QLatin1String("D"))
        return I18n::tr("cleanup.warn_forbidden");
    if (level == QLatin1String("A"))
        return I18n::tr("cleanup.warn_regeneratable");
    if (level == QLatin1String("B"))
        return I18n::tr("cleanup.warn_maybe_useful");
    return QString();
}

void CleanupPanel::applyLargeFileWarning(QTreeWidgetItem* item, const QString& level)
{
    QString warnText = largeFileWarning(level);
    if (auto* s = dynamic_cast<SortableTreeWidgetItem*>(item)) {
        s->setSortData(4, warnText);
    }
    item->setText(4, warnText);
    item->setToolTip(4, warnText);
}

void CleanupPanel::updateSummary(qint64 freeBytes, qint64 totalBytes)
{
    // Disk free space is now shown only in the main window's status bar;
    // the cleanup panel no longer has its own title/summary header. Keep
    // the values cached in case future UI needs them.
    m_freeBytes = freeBytes;
    m_totalBytes = totalBytes;
}

void CleanupPanel::updateSelectedLabel()
{
    qint64 totalSize = 0;
    int totalCount = 0;

    for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
        auto* item = m_catTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QVariantList data = item->data(0, Qt::UserRole).toList();
        if (data.size() < 2)
            continue;
        QString key = data[0].toString();
        QString path = data[1].toString();
        for (const auto& t : m_targets) {
            if (t.key == key && t.path == path) {
                totalSize += t.size;
                totalCount += t.fileCount;
                break;
            }
        }
    }
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QString path = item->data(0, Qt::UserRole).toString();
        for (const auto& lf : m_largeFiles) {
            if (lf.path == path) {
                totalSize += lf.size;
                totalCount += 1;
                break;
            }
        }
    }
    // Duplicate files: child items that are checked.
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            if (child->checkState(0) != Qt::Checked)
                continue;
            QString path = child->data(0, Qt::UserRole).toString();
            for (const auto& dg : m_duplicateGroups) {
                for (const auto& df : dg.files) {
                    if (df.path == path) {
                        totalSize += df.size;
                        totalCount += 1;
                        break;
                    }
                }
            }
        }
    }

    m_selectedLabel->setText(
        I18n::tr("cleanup.selected", QMap<QString, QString>{
            {"size", humanSize(totalSize)},
            {"count", humanCount(totalCount)},
        }));
    // Keep the total-items label in sync with the active tab.
    updateTotalLabel();
}

void CleanupPanel::updateTotalLabel()
{
    // Show total item count + total size for the currently active tab.
    int totalItems = 0;
    qint64 totalSize = 0;
    const int idx = m_tabs ? m_tabs->currentIndex() : 0;

    if (idx == 0) {
        // Categories tab — count targets and sum their sizes.
        for (const auto& t : m_targets) {
            if (t.danger == DangerLevel::D)
                continue;
            ++totalItems;
            totalSize += t.size;
        }
    } else if (idx == 1) {
        // Large files tab.
        for (const auto& lf : m_largeFiles) {
            if (lf.danger == DangerLevel::D || lf.danger == DangerLevel::C)
                continue;
            ++totalItems;
            totalSize += lf.size;
        }
    } else if (idx == 2) {
        // Duplicates tab — count all files across all groups.
        for (const auto& dg : m_duplicateGroups) {
            totalItems += static_cast<int>(dg.files.size());
            totalSize += dg.size * static_cast<qint64>(dg.files.size());
        }
    }

    m_totalLabel->setText(
        I18n::tr("cleanup.total_items", QMap<QString, QString>{
            {"count", humanCount(totalItems)},
            {"size", humanSize(totalSize)},
        }));
}

// ---- Type filter implementation ------------------------------------------- //

// Collect distinct type keys present in the trees and populate the filter
// combos. Each combo item's data holds the language-independent type key
// (e.g. "type.document"); the visible text is the translated label and is
// refreshed in retranslate(). Preserves the user's current selection.
void CleanupPanel::repopulateTypeFilters()
{
    // Large files: collect from top-level items' col-5 sort data (type key).
    if (m_lfTypeFilter) {
        QStringList keys;
        for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
            auto* it = dynamic_cast<SortableTreeWidgetItem*>(m_lfTree->topLevelItem(i));
            if (!it) continue;
            const QString k = it->sortData(5).toString();
            if (!k.isEmpty() && !keys.contains(k))
                keys << k;
        }
        keys.sort();
        const QString prev = m_lfTypeFilter->currentData().toString();
        QSignalBlocker b(m_lfTypeFilter);
        m_lfTypeFilter->clear();
        m_lfTypeFilter->addItem(I18n::tr("cleanup.filter_all_types"), QString());
        for (const QString& k : keys)
            m_lfTypeFilter->addItem(I18n::tr(k.toLatin1().constData()), k);
        const int idx = prev.isEmpty() ? 0 : m_lfTypeFilter->findData(prev);
        m_lfTypeFilter->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    // Duplicates: collect from child items' col-5 sort data (type key).
    if (m_dupTypeFilter) {
        QStringList keys;
        for (int i = 0; i < m_dupTree->topLevelItemCount(); ++i) {
            auto* grp = m_dupTree->topLevelItem(i);
            for (int j = 0; j < grp->childCount(); ++j) {
                auto* it = dynamic_cast<SortableTreeWidgetItem*>(grp->child(j));
                if (!it) continue;
                const QString k = it->sortData(5).toString();
                if (!k.isEmpty() && !keys.contains(k))
                    keys << k;
            }
        }
        keys.sort();
        const QString prev = m_dupTypeFilter->currentData().toString();
        QSignalBlocker b(m_dupTypeFilter);
        m_dupTypeFilter->clear();
        m_dupTypeFilter->addItem(I18n::tr("cleanup.filter_all_types"), QString());
        for (const QString& k : keys)
            m_dupTypeFilter->addItem(I18n::tr(k.toLatin1().constData()), k);
        const int idx = prev.isEmpty() ? 0 : m_dupTypeFilter->findData(prev);
        m_dupTypeFilter->setCurrentIndex(idx >= 0 ? idx : 0);
    }
}

void CleanupPanel::onLfTypeFilterChanged(int)
{
    applyLfTypeFilter();
    updateSelectedLabel();  // counts/labels reflect filtered view
}

void CleanupPanel::onDupTypeFilterChanged(int)
{
    applyDupTypeFilter();
    updateSelectedLabel();
}

void CleanupPanel::applyLfTypeFilter()
{
    if (!m_lfTypeFilter)
        return;
    const QString want = m_lfTypeFilter->currentData().toString();
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* it = m_lfTree->topLevelItem(i);
        auto* sw = dynamic_cast<SortableTreeWidgetItem*>(it);
        const QString lbl = sw ? sw->sortData(5).toString() : QString();
        it->setHidden(!want.isEmpty() && lbl != want);
    }
}

void CleanupPanel::applyDupTypeFilter()
{
    if (!m_dupTypeFilter)
        return;
    const QString want = m_dupTypeFilter->currentData().toString();
    for (int i = 0; i < m_dupTree->topLevelItemCount(); ++i) {
        auto* grp = m_dupTree->topLevelItem(i);
        int visibleChildren = 0;
        for (int j = 0; j < grp->childCount(); ++j) {
            auto* child = grp->child(j);
            auto* sw = dynamic_cast<SortableTreeWidgetItem*>(child);
            const QString lbl = sw ? sw->sortData(5).toString() : QString();
            const bool hide = !want.isEmpty() && lbl != want;
            child->setHidden(hide);
            if (!hide)
                ++visibleChildren;
        }
        // Hide the group row itself if all of its children are filtered out.
        grp->setHidden(visibleChildren == 0);
    }
}

std::vector<std::tuple<QString, QString, QString>> CleanupPanel::getCheckedTargets() const
{
    std::vector<std::tuple<QString, QString, QString>> result;
    for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
        auto* item = m_catTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QVariantList data = item->data(0, Qt::UserRole).toList();
        if (data.size() >= 2) {
            result.emplace_back(QStringLiteral("target"),
                                data[0].toString(),
                                data[1].toString());
        }
    }
    return result;
}

std::vector<std::tuple<QString, QString, QString>> CleanupPanel::getCheckedLargeFiles() const
{
    std::vector<std::tuple<QString, QString, QString>> result;
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            result.emplace_back(QStringLiteral("file"), QString(), path);
    }
    return result;
}

std::vector<std::tuple<QString, QString, QString>> CleanupPanel::getCheckedDuplicates() const
{
    std::vector<std::tuple<QString, QString, QString>> result;
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            if (child->checkState(0) != Qt::Checked)
                continue;
            QString path = child->data(0, Qt::UserRole).toString();
            if (!path.isEmpty())
                result.emplace_back(QStringLiteral("file"), QString(), path);
        }
    }
    return result;
}

qint64 CleanupPanel::getCheckedTotalSize() const
{
    qint64 total = 0;
    for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
        auto* item = m_catTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QVariantList data = item->data(0, Qt::UserRole).toList();
        if (data.size() < 2)
            continue;
        QString key = data[0].toString();
        QString path = data[1].toString();
        for (const auto& t : m_targets) {
            if (t.key == key && t.path == path) {
                total += t.size;
                break;
            }
        }
    }
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QString path = item->data(0, Qt::UserRole).toString();
        for (const auto& lf : m_largeFiles) {
            if (lf.path == path) {
                total += lf.size;
                break;
            }
        }
    }
    // Duplicate files.
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            if (child->checkState(0) != Qt::Checked)
                continue;
            QString path = child->data(0, Qt::UserRole).toString();
            for (const auto& dg : m_duplicateGroups) {
                for (const auto& df : dg.files) {
                    if (df.path == path) {
                        total += df.size;
                        break;
                    }
                }
            }
        }
    }
    return total;
}

// --------------------------------------------------------------------------- //
// Private slots
// --------------------------------------------------------------------------- //
void CleanupPanel::onCatItemChanged()
{
    updateSelectedLabel();

    // Determine if all checkable items are checked.
    bool allChecked = false;
    if (m_catTree->topLevelItemCount() > 0) {
        allChecked = true;
        for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
            auto* item = m_catTree->topLevelItem(i);
            if (item->flags() & Qt::ItemIsUserCheckable) {
                if (item->checkState(0) != Qt::Checked) {
                    allChecked = false;
                    break;
                }
            }
        }
    }

    m_catSelBtn->blockSignals(true);
    m_catSelBtn->setChecked(allChecked);
    m_catSelBtn->setText(allChecked ? I18n::tr("cleanup.deselect_all")
                                     : I18n::tr("cleanup.select_all"));
    m_catSelBtn->blockSignals(false);
}

void CleanupPanel::onLfItemChanged()
{
    updateSelectedLabel();

    bool allChecked = false;
    if (m_lfTree->topLevelItemCount() > 0) {
        allChecked = true;
        for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
            auto* item = m_lfTree->topLevelItem(i);
            if (item->flags() & Qt::ItemIsUserCheckable) {
                if (item->checkState(0) != Qt::Checked) {
                    allChecked = false;
                    break;
                }
            }
        }
    }

    m_lfSelBtn->blockSignals(true);
    m_lfSelBtn->setChecked(allChecked);
    m_lfSelBtn->setText(allChecked ? I18n::tr("cleanup.deselect_all")
                                     : I18n::tr("cleanup.select_all"));
    m_lfSelBtn->blockSignals(false);
}

void CleanupPanel::onLfItemClicked(QTreeWidgetItem* item, int column)
{
    if (column != 3)
        return;
    QString path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty())
        emit pathRevealRequested(path);
}

void CleanupPanel::onCatSelectAllToggled(bool checked)
{
    Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    m_catTree->blockSignals(true);
    for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
        auto* item = m_catTree->topLevelItem(i);
        if (item->flags() & Qt::ItemIsUserCheckable)
            item->setCheckState(0, state);
    }
    m_catTree->blockSignals(false);
    m_catSelBtn->setText(checked ? I18n::tr("cleanup.deselect_all")
                                  : I18n::tr("cleanup.select_all"));
    updateSelectedLabel();
}

void CleanupPanel::onLfSelectAllToggled(bool checked)
{
    Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    m_lfTree->blockSignals(true);
    // Only affect visible items so a type filter is respected: hidden rows
    // (filtered out) keep their current state.
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        if (item->isHidden())
            continue;
        if (item->flags() & Qt::ItemIsUserCheckable)
            item->setCheckState(0, state);
    }
    m_lfTree->blockSignals(false);
    m_lfSelBtn->setText(checked ? I18n::tr("cleanup.deselect_all")
                                  : I18n::tr("cleanup.select_all"));
    updateSelectedLabel();
}

void CleanupPanel::onDupItemChanged()
{
    updateSelectedLabel();

    // Reflect "all visible checkable children checked" state on the select-all
    // button. Hidden (filtered-out) children are ignored so the button reflects
    // the visible subset.
    bool allChecked = false;
    int checkable = 0;
    int checked = 0;
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        if (groupItem->isHidden())
            continue;
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            if (child->isHidden())
                continue;
            if (child->flags() & Qt::ItemIsUserCheckable) {
                ++checkable;
                if (child->checkState(0) == Qt::Checked)
                    ++checked;
            }
        }
    }
    if (checkable > 0 && checkable == checked)
        allChecked = true;

    m_dupSelBtn->blockSignals(true);
    m_dupSelBtn->setChecked(allChecked);
    m_dupSelBtn->setText(allChecked ? I18n::tr("cleanup.deselect_all")
                                     : I18n::tr("cleanup.select_all"));
    m_dupSelBtn->blockSignals(false);
}

void CleanupPanel::onDupItemClicked(QTreeWidgetItem* item, int column)
{
    // Path is in column 4 for child rows; clicking it reveals in Explorer.
    if (column != 4 || !item->parent())
        return;
    QString path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty())
        emit pathRevealRequested(path);
}

void CleanupPanel::onDupSelectAllToggled(bool checked)
{
    Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    m_dupTree->blockSignals(true);
    // Only affect visible children so a type filter is respected: hidden
    // children (filtered out) keep their current state.
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        if (groupItem->isHidden())
            continue;
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            if (child->isHidden())
                continue;
            if (child->flags() & Qt::ItemIsUserCheckable)
                child->setCheckState(0, state);
        }
    }
    m_dupTree->blockSignals(false);
    m_dupSelBtn->setText(checked ? I18n::tr("cleanup.deselect_all")
                                   : I18n::tr("cleanup.select_all"));
    updateSelectedLabel();
}

void CleanupPanel::onCleanClicked()
{
    // Recalculate the selected label before emitting.
    updateSelectedLabel();

    // Collect all checked items as (type, key, path) tuples.
    std::vector<std::tuple<QString, QString, QString>> items;
    for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
        auto* item = m_catTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QVariantList data = item->data(0, Qt::UserRole).toList();
        if (data.size() >= 2) {
            items.emplace_back(QStringLiteral("target"),
                               data[0].toString(),
                               data[1].toString());
        }
    }
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            items.emplace_back(QStringLiteral("file"), QString(), path);
    }
    // Duplicate files (child rows in m_dupTree).
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            if (child->checkState(0) != Qt::Checked)
                continue;
            QString path = child->data(0, Qt::UserRole).toString();
            if (!path.isEmpty())
                items.emplace_back(QStringLiteral("file"), QString(), path);
        }
    }

    if (!items.empty())
        emit cleanRequested(items);
}

// --------------------------------------------------------------------------- //
// Double-click a category to view its contents (path + file listing).
// --------------------------------------------------------------------------- //
void CleanupPanel::onCatItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    QVariantList data = item->data(0, Qt::UserRole).toList();
    if (data.size() < 2)
        return;
    const QString key = data[0].toString();
    const QString path = data[1].toString();
    const QString level = data.size() >= 3 ? data[2].toString() : QString();

    // Find the matching target for size/count info.
    qint64 totalSize = 0;
    int fileCount = 0;
    bool isDir = true;
    for (const auto& t : m_targets) {
        if (t.key == key && t.path == path) {
            totalSize = t.size;
            fileCount = t.fileCount;
            isDir = t.isDir;
            break;
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(I18n::tr("cleanup.details_title"));
    dlg.setMinimumSize(560, 420);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(16, 14, 16, 12);
    lay->setSpacing(8);

    // Title row: category name + level badge.
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto* titleLbl = new QLabel(I18n::tr(key));
    QFont titleFont = titleLbl->font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    titleLbl->setFont(titleFont);
    titleRow->addWidget(titleLbl);
    if (!level.isEmpty()) {
        auto* badge = new QLabel(QStringLiteral(" %1 ").arg(level));
        QFont bf = badge->font();
        bf.setBold(true);
        bf.setPointSize(10);
        badge->setFont(bf);
        // Map level letter to color.
        QString color = QStringLiteral("#6b7280");
        if (level == QLatin1String("S")) color = QStringLiteral("#22c55e");
        else if (level == QLatin1String("A")) color = QStringLiteral("#eab308");
        else if (level == QLatin1String("B")) color = QStringLiteral("#3b82f6");
        else if (level == QLatin1String("C")) color = QStringLiteral("#ef4444");
        badge->setStyleSheet(QStringLiteral("color:%1;").arg(color));
        titleRow->addWidget(badge);
    }
    titleRow->addStretch(1);
    lay->addLayout(titleRow);

    // Path + size + count info.
    auto addInfoRow = [&](const QString& labelKey, const QString& value) {
        auto* row = new QHBoxLayout;
        auto* lab = new QLabel(I18n::tr(labelKey));
        lab->setStyleSheet(QStringLiteral("color: %1; min-width: 70px;")
                               .arg(QString::fromLatin1(C::TEXT_MUTED())));
        auto* val = new QLabel(value);
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(lab);
        row->addWidget(val, 1);
        lay->addLayout(row);
    };
    addInfoRow("cleanup.col_path", path);
    addInfoRow("cleanup.col_size", humanSize(totalSize));
    if (fileCount > 0)
        addInfoRow("cleanup.col_items", humanCount(fileCount));

    // File listing (capped to avoid UI freeze on huge dirs).
    auto* listTitle = new QLabel(I18n::tr("cleanup.details_contents"));
    listTitle->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                 .arg(QString::fromLatin1(C::TEXT_SEC())));
    lay->addWidget(listTitle);

    auto* fileList = new QTreeWidget;
    fileList->setObjectName("filelist");
    fileList->setColumnCount(3);
    fileList->setHeaderLabels({
        I18n::tr("column.name"),
        I18n::tr("column.size"),
        I18n::tr("column.type"),
    });
    fileList->setRootIsDecorated(false);
    fileList->setUniformRowHeights(true);
    fileList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    fileList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    fileList->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    fileList->header()->resizeSection(1, 90);
    fileList->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    fileList->header()->resizeSection(2, 80);

    // Populate the listing.
    const QFileInfo pathInfo(path);

    // Recycle bin: QDir cannot enumerate $Recycle.Bin's per-SID subdirs.
    // Show a message instead of an empty list.
    if (key == "cleanup.b_recycle") {
        auto* msg = new QTreeWidgetItem;
        msg->setText(0, I18n::tr("cleanup.details_recycle_msg"));
        msg->setForeground(0, QColor(QString::fromLatin1(C::TEXT_MUTED())));
        QFont itf = msg->font(0);
        itf.setItalic(true);
        msg->setFont(0, itf);
        fileList->addTopLevelItem(msg);
    }
    // Downloads (virtual group): list all files recursively so the user can
    // see exactly what will be deleted before checking the category.
    else if (key == "cleanup.b_downloads") {
        // Recursive listing with a cap.
        const int cap = 500;
        int shown = 0;
        int total = 0;
        std::stack<QString> pending;
        pending.push(path);
        while (!pending.empty() && shown < cap) {
            const QString cur = pending.top();
            pending.pop();
            QDir dir(cur);
            const auto entries = dir.entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const auto& e : entries) {
                if (e.isDir()) {
                    pending.push(e.absoluteFilePath());
                } else {
                    ++total;
                    if (shown >= cap)
                        continue;
                    auto* fi = new QTreeWidgetItem;
                    // Show relative path for clarity.
                    QString rel = QDir(path).relativeFilePath(e.absoluteFilePath());
                    fi->setText(0, rel);
                    fi->setText(1, humanSize(e.size()));
                    fi->setTextAlignment(1, Qt::AlignRight);
                    fi->setText(2, I18n::tr(typeKey(e.fileName()).toLatin1().constData()));
                    fileList->addTopLevelItem(fi);
                    ++shown;
                }
            }
        }
        if (total > cap) {
            auto* moreItem = new QTreeWidgetItem;
            moreItem->setText(0, I18n::tr("cleanup.details_more",
                QMap<QString, QString>{{"n", QString::number(total - cap)}}));
            moreItem->setForeground(0, QColor(QString::fromLatin1(C::TEXT_MUTED())));
            QFont itf = moreItem->font(0);
            itf.setItalic(true);
            moreItem->setFont(0, itf);
            fileList->addTopLevelItem(moreItem);
        }
    }
    // Regular directories: list direct children (capped at 500).
    else if (pathInfo.isDir()) {
        QDir dir(path);
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                QDir::Name);
        const int cap = 500;
        int shown = 0;
        for (const auto& e : entries) {
            if (shown >= cap)
                break;
            auto* fi = new QTreeWidgetItem;
            fi->setText(0, e.fileName());
            fi->setText(1, e.isDir() ? QStringLiteral("—") : humanSize(e.size()));
            fi->setTextAlignment(1, Qt::AlignRight);
            fi->setText(2, e.isDir() ? I18n::tr("type.folder")
                                     : I18n::tr(typeKey(e.fileName()).toLatin1().constData()));
            fileList->addTopLevelItem(fi);
            ++shown;
        }
        if (entries.size() > cap) {
            auto* moreItem = new QTreeWidgetItem;
            moreItem->setText(0, I18n::tr("cleanup.details_more",
                QMap<QString, QString>{{"n", QString::number(entries.size() - cap)}}));
            moreItem->setForeground(0, QColor(QString::fromLatin1(C::TEXT_MUTED())));
            QFont itf = moreItem->font(0);
            itf.setItalic(true);
            moreItem->setFont(0, itf);
            fileList->addTopLevelItem(moreItem);
        }
    } else if (pathInfo.isFile()) {
        auto* fi = new QTreeWidgetItem;
        fi->setText(0, pathInfo.fileName());
        fi->setText(1, humanSize(pathInfo.size()));
        fi->setTextAlignment(1, Qt::AlignRight);
        fi->setText(2, I18n::tr(typeKey(pathInfo.fileName()).toLatin1().constData()));
        fileList->addTopLevelItem(fi);
    }

    lay->addWidget(fileList, 1);

    // Close button.
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
// Retranslation
// --------------------------------------------------------------------------- //
void CleanupPanel::refreshTheme()
{
    // Re-apply the inline styles that bake a palette color into the label.
    // (Per-item tree foregrounds are mid-tone and stay legible on both themes;
    // the global QSS handles the rest.)
    m_selectedLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(QString::fromLatin1(C::TEXT_SEC())));
    m_totalLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(QString::fromLatin1(C::TEXT_MUTED())));
}

void CleanupPanel::retranslate()
{
    m_catTree->setHeaderLabels({
        I18n::tr("cleanup.col_level"),
        I18n::tr("cleanup.col_category"),
        I18n::tr("cleanup.col_size"),
        I18n::tr("cleanup.col_items"),
        I18n::tr("cleanup.col_remark"),
    });
    m_lfTree->setHeaderLabels({
        I18n::tr("cleanup.col_level"),
        I18n::tr("cleanup.col_name"),
        I18n::tr("cleanup.col_size"),
        I18n::tr("cleanup.col_path"),
        I18n::tr("cleanup.col_warning"),
        I18n::tr("cleanup.col_type"),
    });
    m_dupTree->setHeaderLabels({
        I18n::tr("cleanup.col_level"),
        I18n::tr("cleanup.col_dup_group"),
        I18n::tr("cleanup.col_size"),
        I18n::tr("cleanup.col_dup_wasted"),
        I18n::tr("cleanup.col_paths"),
        I18n::tr("cleanup.col_type"),
    });

    m_tabs->setTabText(0, I18n::tr("cleanup.tab_categories"));
    m_tabs->setTabText(1, I18n::tr("cleanup.tab_large_files"));
    m_tabs->setTabText(2, I18n::tr("cleanup.tab_duplicates"));

    m_cleanBtn->setText(I18n::tr("cleanup.clean_selected"));
    m_rescanBtn->setText(I18n::tr("cleanup.rescan"));

    // Re-translate select-all buttons (preserve checked state).
    m_catSelBtn->setText(m_catSelBtn->isChecked()
                             ? I18n::tr("cleanup.deselect_all")
                             : I18n::tr("cleanup.select_all"));
    m_lfSelBtn->setText(m_lfSelBtn->isChecked()
                             ? I18n::tr("cleanup.deselect_all")
                             : I18n::tr("cleanup.select_all"));
    m_dupSelBtn->setText(m_dupSelBtn->isChecked()
                             ? I18n::tr("cleanup.deselect_all")
                             : I18n::tr("cleanup.select_all"));
    // Re-translate type-filter labels.
    m_lfTypeLabel->setText(I18n::tr("cleanup.filter_type"));
    m_dupTypeLabel->setText(I18n::tr("cleanup.filter_type"));

    // Re-translate each category item.
    for (int i = 0; i < m_catTree->topLevelItemCount(); ++i) {
        auto* item = m_catTree->topLevelItem(i);
        QVariantList data = item->data(0, Qt::UserRole).toList();
        if (data.size() < 3)
            continue;
        QString key = data[0].toString();
        QString path = data[1].toString();
        for (const auto& target : m_targets) {
            if (target.key == key && target.path == path) {
                applyTargetTranslation(item, target);
                break;
            }
        }
    }
    // Re-translate each large-file item's warning + type label.
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        auto* sw = dynamic_cast<SortableTreeWidgetItem*>(item);
        QString path = item->data(0, Qt::UserRole).toString();
        for (const auto& lf : m_largeFiles) {
            if (lf.path == path) {
                applyLargeFileWarning(item, dangerLevelToString(lf.danger));
                break;
            }
        }
        // Refresh col-5 type text from the stored language-independent key.
        if (sw) {
            const QString tKey = sw->sortData(5).toString();
            if (!tKey.isEmpty())
                item->setText(5, I18n::tr(tKey.toLatin1().constData()));
        }
    }
    // Re-translate each duplicate group's label and per-file type label.
    for (int gi = 0; gi < m_dupTree->topLevelItemCount(); ++gi) {
        auto* groupItem = m_dupTree->topLevelItem(gi);
        int groupIdx = groupItem->data(0, Qt::UserRole).toInt();
        if (groupIdx >= 0 && groupIdx < static_cast<int>(m_duplicateGroups.size())) {
            const auto& dg = m_duplicateGroups[groupIdx];
            groupItem->setText(1, I18n::tr("cleanup.dup_group_label",
                QMap<QString, QString>{{"count", QString::number(dg.files.size())}}));
        }
        for (int ci = 0; ci < groupItem->childCount(); ++ci) {
            auto* child = groupItem->child(ci);
            auto* sw = dynamic_cast<SortableTreeWidgetItem*>(child);
            if (sw) {
                const QString tKey = sw->sortData(5).toString();
                if (!tKey.isEmpty())
                    child->setText(5, I18n::tr(tKey.toLatin1().constData()));
            }
        }
    }
    // Rebuild filter combos so their visible labels use the new language
    // (the underlying type-key data is preserved).
    repopulateTypeFilters();

    updateSummary(m_freeBytes, m_totalBytes);
    updateSelectedLabel();
}
