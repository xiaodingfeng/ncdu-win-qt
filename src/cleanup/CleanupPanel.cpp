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

#include <algorithm>

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

    // ── Header: title + summary ──────────────────────────────────
    auto* header = new QFrame;
    auto* hlay = new QHBoxLayout(header);
    hlay->setContentsMargins(8, 4, 8, 4);
    m_titleLabel = new QLabel(I18n::tr("cleanup.title"));
    m_titleLabel->setStyleSheet(
        QStringLiteral("font-size: 14px; font-weight: 700; color: %1;")
            .arg(QString::fromLatin1(C::TEXT)));
    hlay->addWidget(m_titleLabel);
    hlay->addStretch(1);
    m_summaryLabel = new QLabel(QString());
    m_summaryLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;")
            .arg(QString::fromLatin1(C::TEXT_MUTED)));
    hlay->addWidget(m_summaryLabel);
    lay->addWidget(header);

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
        catLay->setContentsMargins(0, 4, 0, 0);
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

        auto* catHdr = new QHBoxLayout;
        catHdr->setContentsMargins(0, 0, 0, 0);
        catHdr->addStretch(1);
        catHdr->addWidget(m_catSelBtn);
        catLay->addLayout(catHdr);
        catLay->addWidget(m_catTree, 1);

        m_tabs->addTab(catTab, I18n::tr("cleanup.tab_categories"));
    }

    // Tab 2: Large files
    {
        auto* lfTab = new QWidget;
        auto* lfLay = new QVBoxLayout(lfTab);
        lfLay->setContentsMargins(0, 4, 0, 0);
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
        });

        auto* lfHdr = new QHBoxLayout;
        lfHdr->setContentsMargins(0, 0, 0, 0);
        lfHdr->addStretch(1);
        lfHdr->addWidget(m_lfSelBtn);
        lfLay->addLayout(lfHdr);
        lfLay->addWidget(m_lfTree, 1);

        m_tabs->addTab(lfTab, I18n::tr("cleanup.tab_large_files"));
    }

    // Connect item-change signals so the selected-size label and select-all
    // button state stay in sync when the user toggles checkboxes.
    connect(m_catTree, &QTreeWidget::itemChanged, this, &CleanupPanel::onCatItemChanged);
    connect(m_lfTree, &QTreeWidget::itemChanged, this, &CleanupPanel::onLfItemChanged);
    connect(m_lfTree, &QTreeWidget::itemClicked, this, &CleanupPanel::onLfItemClicked);

    lay->addWidget(m_tabs, 1);

    // ── Bottom: selected label + rescan + clean ──────────────────
    auto* bottom = new QFrame;
    auto* blay = new QHBoxLayout(bottom);
    blay->setContentsMargins(8, 4, 8, 4);
    m_selectedLabel = new QLabel(I18n::tr("cleanup.selected",
        QMap<QString, QString>{{"size", QStringLiteral("0 B")}, {"count", QStringLiteral("0")}}));
    m_selectedLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(QString::fromLatin1(C::TEXT_SEC)));
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
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

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
    m_summaryLabel->setText(I18n::tr("cleanup.scanning"));
    m_catTree->setSortingEnabled(false);
    m_lfTree->setSortingEnabled(false);
    m_catTree->clear();
    m_lfTree->clear();
    m_targets.clear();
    m_largeFiles.clear();
    m_cleanBtn->setEnabled(false);
}

void CleanupPanel::stopScanProgress()
{
    m_scanProgress->setVisible(false);
    m_summaryLabel->setText(QString());
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
    item->setToolTip(1, catName);

    // Col 2: Size
    QString sizeText = target.size > 0 ? humanSize(target.size) : QStringLiteral("\u2014");
    item->setText(2, sizeText);
    item->setToolTip(2, sizeText);
    item->setTextAlignment(2, Qt::AlignRight);
    item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC)));

    // Col 3: Item count
    QString countText = target.fileCount > 0 ? humanCount(target.fileCount)
                                              : QStringLiteral("\u2014");
    item->setText(3, countText);
    item->setToolTip(3, countText);
    item->setTextAlignment(3, Qt::AlignRight);
    item->setForeground(3, QColor(QString::fromLatin1(C::TEXT_MUTED)));

    // Col 4: Remark
    QString remark = target.remark.isEmpty() ? QString() : I18n::tr(target.remark);
    item->setText(4, remark);
    item->setToolTip(4, remark);
    item->setForeground(4, QColor(QString::fromLatin1(C::TEXT_MUTED)));

    // Checkbox: enabled targets are user-checkable; disabled (C level) are
    // shown with an unchecked, non-toggleable checkbox and a grayed name.
    if (target.enabled) {
        item->setCheckState(0, target.checked ? Qt::Checked : Qt::Unchecked);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    } else {
        item->setCheckState(0, Qt::Unchecked);
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED)));
    }

    if (target.size == 0)
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED)));

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
    item->setForeground(2, QColor(QString::fromLatin1(C::TEXT_SEC)));

    // Col 3: Path (clickable, reveals in Explorer)
    item->setText(3, lf.path);
    item->setForeground(3, QColor(QString::fromLatin1(C::PRIMARY)));
    item->setToolTip(3, lf.path);

    // Col 4: Warning text + color by level
    if (lvl == QLatin1String("C") || lvl == QLatin1String("D")) {
        applyLargeFileWarning(item, lvl);
        item->setForeground(4, QColor(QStringLiteral("#ef4444")));
    } else if (lvl == QLatin1String("A")) {
        applyLargeFileWarning(item, lvl);
        item->setForeground(4, QColor(QString::fromLatin1(C::TEXT_MUTED)));
    } else if (lvl == QLatin1String("B")) {
        applyLargeFileWarning(item, lvl);
        item->setForeground(4, QColor(QStringLiteral("#eab308")));
    }

    // C/D level: checkbox disabled (not user-checkable); others checkable.
    if (lvl == QLatin1String("C") || lvl == QLatin1String("D")) {
        item->setCheckState(0, Qt::Unchecked);
        item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_MUTED)));
    } else {
        item->setCheckState(0, Qt::Unchecked);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    }

    m_lfTree->addTopLevelItem(item);
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
        if (lf.danger == DangerLevel::D)
            continue;
        addLargeFile(lf);
    }

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
    item->setToolTip(1, catName);
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
    m_freeBytes = freeBytes;
    m_totalBytes = totalBytes;
    if (totalBytes > 0) {
        double pct = static_cast<double>(freeBytes) / totalBytes * 100.0;
        m_summaryLabel->setText(
            I18n::tr("cleanup.free_space", QMap<QString, QString>{
                {"free", humanSize(freeBytes)},
                {"total", humanSize(totalBytes)},
                {"pct", QString::number(pct, 'f', 0)},
            }));
    }
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

    m_selectedLabel->setText(
        I18n::tr("cleanup.selected", QMap<QString, QString>{
            {"size", humanSize(totalSize)},
            {"count", humanCount(totalCount)},
        }));
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
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        if (item->flags() & Qt::ItemIsUserCheckable)
            item->setCheckState(0, state);
    }
    m_lfTree->blockSignals(false);
    m_lfSelBtn->setText(checked ? I18n::tr("cleanup.deselect_all")
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

    if (!items.empty())
        emit cleanRequested(items);
}

// --------------------------------------------------------------------------- //
// Retranslation
// --------------------------------------------------------------------------- //
void CleanupPanel::retranslate()
{
    m_titleLabel->setText(I18n::tr("cleanup.title"));

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
    });

    m_tabs->setTabText(0, I18n::tr("cleanup.tab_categories"));
    m_tabs->setTabText(1, I18n::tr("cleanup.tab_large_files"));

    m_cleanBtn->setText(I18n::tr("cleanup.clean_selected"));
    m_rescanBtn->setText(I18n::tr("cleanup.rescan"));

    // Re-translate select-all buttons (preserve checked state).
    m_catSelBtn->setText(m_catSelBtn->isChecked()
                             ? I18n::tr("cleanup.deselect_all")
                             : I18n::tr("cleanup.select_all"));
    m_lfSelBtn->setText(m_lfSelBtn->isChecked()
                             ? I18n::tr("cleanup.deselect_all")
                             : I18n::tr("cleanup.select_all"));

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
    // Re-translate each large-file item's warning.
    for (int i = 0; i < m_lfTree->topLevelItemCount(); ++i) {
        auto* item = m_lfTree->topLevelItem(i);
        QString path = item->data(0, Qt::UserRole).toString();
        for (const auto& lf : m_largeFiles) {
            if (lf.path == path) {
                applyLargeFileWarning(item, dangerLevelToString(lf.danger));
                break;
            }
        }
    }

    updateSummary(m_freeBytes, m_totalBytes);
    updateSelectedLabel();
}
