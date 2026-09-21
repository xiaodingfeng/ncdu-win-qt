#include "AppDataMovePanel.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollBar>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QMap>
#include <QSet>
#include <QComboBox>
#include <QIcon>
#include <QPixmap>
#include <QProgressBar>
#include <QSettings>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QSignalBlocker>
#include <QDeadlineTimer>
#include <QRegularExpression>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "Style.h"
#include "I18n.h"
#include "DialogI18n.h"
#include "FilterHeaderView.h"
#include "FormatHelpers.h"
#include "InstalledApps.h"
#include "LockerDialog.h"
#include "OpGuard.h"
#include "WinApi.h"
#include "MoveDstResolver.h"
#include "MoveSelect.h"
#include "ScanRoots.h"
#include "Logger.h"

// The row states and the "may this row start a move?" rule. Both live in
// MoveSelect.h so the probe lab can check the rule without a panel.
using namespace MoveSelect;

namespace {

// Height of the "scan folder" row (combo + add/remove buttons). 24px used to
// squash the glyphs of the labels and buttons together, so the row now matches
// the height the rest of the panel's controls get from the global stylesheet.
constexpr int kScopeRowHeight = 32;

// Step counter values of the move job (m_movePhase). The order is the safety
// contract, so it is spelled out in full:
//
//   precheck -> copy -> verify -> rename the original aside -> create the
//        junction -> only now delete the original
//
// Nothing is removed from the original location until a complete copy has been
// verified elsewhere, and the original is not deleted at all until the junction
// that replaces it already exists.
//
// The precheck is deliberately FIRST and deliberately cheap: finding out who is
// holding the folder open is the one moment where closing a program costs the
// user nothing, because no data of theirs has been touched yet. Learning it
// after a failed copy instead means a half-copied tree has to be discarded
// before anything can be retried.
constexpr int kMvPrecheck = 6;     // who holds this folder open, before copying
constexpr int kMvCopy = 0;         // copy the folder to the other drive
constexpr int kMvCopied = 1;       // copy finished: start verifying it
constexpr int kMvVerified = 2;     // copy verified: rename the original aside
constexpr int kMvRenamed = 3;      // original moved aside: leave a junction behind
constexpr int kMvLinked = 4;       // junction created: drop the stale original
constexpr int kMvOldGone = 5;      // stale original removed: this folder is done
constexpr int kMvBadCopyGone = 10; // the unverified copy has been discarded

// Phases of the progress bar while one folder is being moved. The bar carries a
// fixed 0..1000 scale per folder and is split between these phases, because the
// steps after the copy have no percentage of their own: verifying a huge tree
// and then dropping the original copy are both slow, and a bar parked at 100%
// for the whole of them reads as "stuck". With the split, the bar keeps moving
// until the folder is really finished.
constexpr int kJobCopy = 0;     // bytes landing on the other drive
constexpr int kJobVerify = 1;   // measuring the original, then the copy
constexpr int kJobFinish = 2;   // renaming the original aside, leaving a junction
constexpr int kJobCleanup = 3;  // dropping the now redundant original

constexpr int kCopyBase = 0;
constexpr int kCopySpan = 700;
constexpr int kVerifyBase = kCopyBase + kCopySpan;
constexpr int kVerifySpan = 160;  // half for each tree
constexpr int kFinishBase = kVerifyBase + kVerifySpan;
constexpr int kFinishSpan = 60;
constexpr int kCleanupBase = kFinishBase + kFinishSpan;
constexpr int kCleanupSpan = 80;  // 0..1000 in total

// Step counter values of the restore job. Kept well clear of the move phases so
// the two jobs can never be confused for one another.
constexpr int kRsPrepare = 20;      // decide what currently occupies the old location
constexpr int kRsUnlinkDone = 21;   // the junction has been removed
constexpr int kRsCleared = 22;      // a leftover directory has been discarded
constexpr int kRsCopied = 24;       // copy back finished: verify it
constexpr int kRsVerify = 23;       // copy back verified off the UI thread
constexpr int kRsPartialGone = 25;  // a failed copy back has been discarded
constexpr int kRsLinkedBack = 26;   // junction restored after a failed copy back
constexpr int kRsCopyDropped = 27;  // the copy on the other drive is gone
// A move that failed at the hand-over left the original sitting under its own
// name again, so the copy on the other drive is the redundant one. Deciding
// which of the two to trust needs both trees measured, which happens here.
constexpr int kRsAssess = 28;
// The restore finished and the data is home, but the copy on the other drive
// could not be deleted — something inside it is still open. Not a failure of
// the restore, and not something to forget either: it is a folder this app put
// there and can still remove.
constexpr int kRsResidue = 29;
// The restore's own precheck. The same question the move asks before its copy,
// asked before the junction is taken away: who is holding the original folder?
// Nothing may be touched until it is answered, because emptying a folder that a
// program is still writing in is how a restore ends up with two half-copies and
// a folder nobody can delete.
constexpr int kRsProbe = 30;      // the "who holds it" query is in flight
constexpr int kRsProbed = 31;     // answer in hand: offer to close, or refuse
// The folder at the original path has to go before the copy comes back. That
// delete is checked: a folder that could not be emptied is a stop, never a
// detour — the copy that follows would be made into a tree that still holds
// open files, and its measurements could never agree with the source.
constexpr int kRsClearHome = 32;      // emptying the original path
constexpr int kRsHomeProbe = 33;      // it would not empty: find out who holds it
constexpr int kRsHomeProbed = 34;     // ...and say so, leaving the data where it is

// How far a relocation got, as stored in the journal. Recovery reads this to
// decide what to finish or undo after a crash or a power cut.
constexpr int kPhaseNone = 0;
constexpr int kPhaseCopying = 1;  // copy started; the original is untouched
constexpr int kPhaseCopied = 2;   // copy verified; the original is being replaced
constexpr int kPhaseLinked = 3;   // junction in place; only cleanup may remain
// Data is back at its original path, and all that is left of the whole
// operation is a copy on the other drive that could not be deleted. Recorded
// because the app has to be able to tell its own leftover — which it may
// remove — apart from a folder that was always the user's.
constexpr int kPhaseResidue = 4;

// Step counter values of the leftover-cleanup job. Its own range again, so a
// stale value can never be mistaken for a step of the move or the restore.
constexpr int kClProbe = 40;    // starting the "who is holding it" query
constexpr int kClProbed = 41;   // query answered: ask, close, then delete
constexpr int kClDelete = 42;   // delete running: report what is left

// The original is renamed to "<path>.ncduwin-old" instead of being deleted, so
// that the only copy is never on a single disk during the hand-over.
const QString kOldSuffix = QStringLiteral(".ncduwin-old");

// Watching the copy means enumerating the destination, which is the very thing
// that is slow on a folder with huge numbers of subdirectories. So the sampling
// runs on a worker thread, never overlaps itself, and its interval adapts to what
// the previous walk cost: a few hundred files are walked in milliseconds and
// deserve a bar that moves, while a tree of a million entries is only visited
// occasionally so the walk does not fight robocopy for the disk. The interval is
// at least double the walk's own duration, which keeps the sampler's share of
// the disk below roughly half even in the worst case.
constexpr int kCopyProbeMinMs = 700;
constexpr int kCopyProbeMaxMs = 15000;

// Width of the "action" column. It holds one short button, so it is kept to
// just what that button needs — the room is better spent on the path column.
// This is the floor; the three labels the button can carry set the real width
// (see actionColumnWidth).
constexpr int kActionColWidth = 56;
// The action column is never allowed to be this close to the button it holds:
// a button flush against its column reads as clipped even when it is not.
constexpr int kActionColMargin = 10;
// Room kept for the header's sort arrow, which the style draws at the right
// edge of the sorted section — the same edge the filter funnel uses.
constexpr int kSortArrowRoom = 16;

// A junction is a reparse point, not a symlink, so QFileInfo::isSymLink() misses
// it — which is why a folder that was already moved kept showing up as movable.
bool isReparsePoint(const QString& path)
{
    return WinApi::isReparsePointAt(path);
}

// Where a junction (or symlink) really leads. GetFinalPathNameByHandle resolves
// the reparse point for us, so no reparse buffer has to be parsed by hand.
QString linkTargetOf(const QString& path)
{
#ifdef _WIN32
    const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return QString();
    wchar_t buf[1024] = {};
    const DWORD len = GetFinalPathNameByHandleW(h, buf, 1024,
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (len == 0 || len >= 1024)
        return QString();
    QString resolved = QString::fromWCharArray(buf, static_cast<int>(len));
    if (resolved.startsWith(QStringLiteral("\\\\?\\")))
        resolved = resolved.mid(4);
    return QDir::toNativeSeparators(QDir::cleanPath(resolved));
#else
    return QFileInfo(path).symLinkTarget();
#endif
}

// Folders under AppData / LocalAppData that are Windows' own plumbing or that
// must not be relocated. Everything else is fair game, which is what makes this
// work for software nobody wrote a rule for.
bool isSkippedFolder(const QString& name)
{
    static const QStringList kSkip = {
        QStringLiteral("microsoft"), QStringLiteral("packages"), QStringLiteral("temp"),
        QStringLiteral("windows"), QStringLiteral("comms"),
        QStringLiteral("connecteddevicesplatform"), QStringLiteral("d3dscache"),
        QStringLiteral("elevateddiagnostics"), QStringLiteral("grouppolicy"),
        QStringLiteral("internet explorer"), QStringLiteral("nvidia"),
        QStringLiteral("nvidia corporation"), QStringLiteral("intel"), QStringLiteral("amd"),
        QStringLiteral("programs"), QStringLiteral("crashdumps"),
        QStringLiteral("placeholdertilelogofolder"), QStringLiteral("publishercache"),
        QStringLiteral("virtualstore"), QStringLiteral("microsoftedge"),
    };
    return kSkip.contains(name.toLower());
}

bool isOnSystemDrive(const QString& path)
{
    const QString sys = qEnvironmentVariable("SystemDrive");
    if (sys.isEmpty())
        return true;
    return path.startsWith(sys, Qt::CaseInsensitive);
}

QString normalizeName(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (const QChar& c : value) {
        if (c.isLetterOrNumber())
            out += c.toLower();
    }
    return out;
}

// Comparable form of a path: forward slashes, lowercase, no trailing slash.
QString pathKey(const QString& raw)
{
    if (raw.isEmpty())
        return QString();
    QString p = QDir::cleanPath(QDir::fromNativeSeparators(raw)).toLower();
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

// Copying a folder onto itself would trivially "verify" and then delete the only
// copy of the data, so any overlap between source and target is refused.
bool pathsOverlap(const QString& a, const QString& b)
{
    const QString x = pathKey(a);
    const QString y = pathKey(b);
    if (x.isEmpty() || y.isEmpty())
        return false;
    return x == y || x.startsWith(y + QLatin1Char('/'))
           || y.startsWith(x + QLatin1Char('/'));
}

// Names the folder after the installed program it looks like, so the list reads
// "WeChat" instead of "WeChat Files". No match simply keeps the folder name.
QString matchInstalledProgram(const QString& folderName, const QStringList& installed)
{
    const QString folder = normalizeName(folderName);
    if (folder.size() < 3)
        return QString();
    for (const QString& raw : installed) {
        const QString app = normalizeName(raw);
        if (app.size() < 3)
            continue;
        if (app.contains(folder) || folder.contains(app))
            return raw;
    }
    return QString();
}

// Size of the files a copy of this folder would move: reparse points are links,
// not storage, so they are skipped (robocopy is told the same with /XJ).
qint64 directorySize(const QString& path)
{
    return WinApi::dirSizeNoReparse(path);
}

// An empty folder is legitimate, but "0 bytes" can also mean "could not be
// read". Before such a folder counts as successfully copied, make sure it really
// holds nothing — the original is deleted right after.
bool holdsNothing(const QString& path)
{
    QDir dir(path);
    return dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                         | QDir::Hidden | QDir::System).isEmpty();
}

// "Who is holding this folder, and may we close them" lives in OpGuard now,
// next to the same question the folder tree's delete and move paths ask — one
// definition of "only system programs have it open", so the two halves of the
// app cannot disagree about what is worth interrupting the user for.

QString robocopyPath()
{
    return qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/robocopy.exe");
}

QString cmdPath()
{
    return qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/cmd.exe");
}

// Arguments for "mklink /J <link> <target>". Both paths are forced to native
// separators because mklink reads a forward slash as the start of an option:
// "mklink /J C:/x C:/y" fails with "syntax of the command is incorrect".
QStringList mkLinkArgs(const QString& link, const QString& target)
{
    return {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
            QDir::toNativeSeparators(QDir::cleanPath(link)),
            QDir::toNativeSeparators(QDir::cleanPath(target))};
}

// Arguments for removing a junction itself. "rmdir" without /S unlinks the
// reparse point and never touches what it points at. A trailing separator makes
// the command fail, so the path is normalised first.
QStringList unlinkArgs(const QString& link)
{
    return {QStringLiteral("/c"), QStringLiteral("rmdir"),
            QDir::toNativeSeparators(QDir::cleanPath(link))};
}

// The original, renamed aside next to itself. Same directory, so the rename is
// an instant metadata operation that can never run out of disk space.
QString oldPathOf(const QString& path)
{
    return path + kOldSuffix;
}

// Arguments for "copy this folder over there". Two of these are what make a
// folder with tens of thousands of subdirectories bearable:
//   /MT:16  copy with 16 threads instead of walking the tree on one
//   /COPY:DT  copy data and timestamps only, skipping the per-file
//             SetFileAttributes round-trip that attributes cost
// Everything else exists to make the run quiet, bounded and junction-safe.
QStringList copyArgs(const QString& src, const QString& dst)
{
    return {src, dst,
            QStringLiteral("/E"),        // include subdirectories, even empty ones
            QStringLiteral("/COPY:DT"),  // data + timestamps
            QStringLiteral("/XJ"),       // never follow junctions
            QStringLiteral("/MT:16"),    // parallel copy
            QStringLiteral("/R:1"), QStringLiteral("/W:1"),
            QStringLiteral("/NFL"), QStringLiteral("/NDL"), QStringLiteral("/NC"),
            QStringLiteral("/NJH"), QStringLiteral("/NJS"), QStringLiteral("/NP")};
}

// Robocopy packs several outcomes into one exit code, bit by bit. Bits 2, 3 and
// 4 are the ones that mean "not everything made it across" (the tests below use
// the same mask); the low bits are informational — files copied, extras seen.
// Spelled out because a bare "exit code 8" tells the user nothing actionable.
QString describeRobocopyExit(int code)
{
    QStringList parts;
    if (code & 0x01)
        parts << QStringLiteral("files-copied");
    if (code & 0x02)
        parts << QStringLiteral("extra-files");
    if (code & 0x04)
        parts << QStringLiteral("mismatched");
    if (code & 0x08)
        parts << QStringLiteral("copy-errors");
    if (code & 0x10)
        parts << QStringLiteral("fatal-error");
    if (parts.isEmpty())
        parts << QStringLiteral("nothing-to-do");
    return QStringLiteral("exit=%1 (%2)").arg(code).arg(parts.join(QLatin1Char('+')));
}

// Collapses a helper's output into something a log line can carry: the last few
// non-empty lines, which is where robocopy puts the reason it failed.
QString tailOf(const QString& output, int maxLines = 6)
{
    QStringList lines;
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            lines << trimmed;
    }
    while (lines.size() > maxLines)
        lines.removeFirst();
    return lines.join(QStringLiteral(" | "));
}

// Turns the Win32 error a failed rename or delete came back with into something
// the user can act on. Error 32 (sharing violation) is by far the most common
// here: the folder is still open in another program, and the only fix is to
// close it and try again — which the message has to say, because the folder
// looks perfectly fine in Explorer.
QString winErrorReason(quint32 err)
{
    switch (err) {
    case 32:
    case 33:
        return I18n::tr("app_move.winerr_in_use");
    case 5:
        return I18n::tr("app_move.winerr_denied");
    case 183:
        return I18n::tr("app_move.winerr_exists");
    case 2:
    case 3:
        return I18n::tr("app_move.winerr_missing");
    default:
        return I18n::tr("app_move.winerr_other",
                        QMap<QString, QString>{{"code", QString::number(err)}});
    }
}

// What a helper process said, split into the one line worth showing and the rest
// worth logging.
struct Diag {
    QString reason;  // short, for the progress row; empty when nothing was found
    QString detail;  // the tail of the output, for the log
};

// Pulls the one line a user can act on out of a helper's output.
//
// Two things make this trickier than reading the text: robocopy and cmd print
// their messages in the system's own language, and they write them in the
// console's 8-bit code page rather than UTF-8 (so the caller decodes with
// fromLocal8Bit). The parsing therefore cannot match on words — it matches the
// shape "错误 <n> (0x........) 正在复制文件 <path>" / "ERROR <n> (0x........)
// Copying File <path>" and takes the line after it as the explanation, which is
// where robocopy puts "另一个程序正在使用此文件" / "the file is in use".
Diag diagnoseOutput(const QString& output)
{
    Diag d;
    d.detail = tailOf(output, 30);

    static const QRegularExpression errRe(
        QStringLiteral(R"((?:错误|ERROR)\s+(\d+)\s*\(0x[0-9A-Fa-f]{8}\)\s*(.*))"));

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const QRegularExpressionMatch m = errRe.match(line);
        if (!m.hasMatch())
            continue;

        // "正在复制文件 C:\...\x.dat" — the path is the last whitespace token.
        QString path = m.captured(2).trimmed();
        const int sp = path.lastIndexOf(QLatin1Char(' '));
        if (sp > 0)
            path = path.mid(sp + 1);
        const QString file = QFileInfo(path).fileName();

        // The explanation is the first non-empty line after the error line.
        QString why;
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString next = lines.at(j).trimmed();
            if (!next.isEmpty()) {
                why = next;
                break;
            }
        }

        d.reason = I18n::tr("app_move.fail_errno", QMap<QString, QString>{
            {"code", m.captured(1)},
            {"file", file.isEmpty() ? I18n::tr("app_move.fail_no_file")
                                    : file},
            {"why", why.isEmpty() ? I18n::tr("app_move.fail_no_reason") : why}});
        break;
    }

    if (d.reason.isEmpty())
        d.reason = tailOf(output, 1);
    return d;
}

// Both trees are walked after the copy finished — never the size measured during
// the scan, because the point is to describe the state right now, at the moment
// the original is about to be replaced. Runs on a worker thread: walking two
// large trees on the UI thread is what made the dialog freeze. *progress* is
// filled in while the walk proceeds so the bar keeps moving through it.
AppDataMovePanel::VerifyResult verifyTrees(
    const QString& src, const QString& dst,
    std::shared_ptr<AppDataMovePanel::WalkProgress> progress)
{
    AppDataMovePanel::VerifyResult r;
    if (progress)
        progress->stage.store(0, std::memory_order_relaxed);
    r.src = WinApi::dirSizeNoReparse(src, progress ? &progress->srcBytes : nullptr);
    if (progress)
        progress->stage.store(1, std::memory_order_relaxed);
    r.dst = WinApi::dirSizeNoReparse(dst, progress ? &progress->dstBytes : nullptr);
    return r;
}

// Wait — but only for a moment — for a worker to come back. Only used while the
// panel is being destroyed. Everything these workers touch is either their own
// shared counter or disk state the journal already describes, so one that is
// still busy may finish on its own; the wait is there so teardown is
// deterministic in the common case, not because the panel owns their memory.
void waitForWorker(QFutureWatcherBase* watcher, int ms)
{
    if (!watcher || watcher->isFinished())
        return;
    QDeadlineTimer deadline(ms);
    while (!watcher->isFinished() && !deadline.hasExpired())
        QThread::msleep(20);
}

// One file listing where every relocated folder went. It is written the moment
// the copy has been verified, so an interrupted move can still be undone.
QString journalPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return QString();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/moved_dirs.json");
}

// The scan scope is remembered in the registry next to the app's other
// settings. Stored as: the user's extra directories, plus which entry was
// selected last.
const QString kRegRoot = QStringLiteral("HKEY_CURRENT_USER\\Software\\NcduWin");
const QString kRegCustomDirs = QStringLiteral("MoveScanCustomDirs");
const QString kRegScopeIndex = QStringLiteral("MoveScanScopeIndex");
const QString kRegLastTarget = QStringLiteral("MoveTargetRoot");

} // namespace

AppDataMovePanel::AppDataMovePanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
    loadScopeSettings();
    rebuildScopeCombo();
}

AppDataMovePanel::~AppDataMovePanel()
{
    // The move tab outlives its dialog, so this can run in the middle of a copy.
    // Nothing may be left in flight: the helper process (robocopy / mklink) is
    // still holding files open, and its QProcess owns the very handles that are
    // about to disappear.
    if (m_jobTimer)
        m_jobTimer->stop();
    if (m_copyTimer)
        m_copyTimer->stop();
    if (m_progressHideTimer)
        m_progressHideTimer->stop();

    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_procFailedToStart = true;  // no "finished" signal is coming
        m_proc->disconnect(this);
        // Killing a half-finished copy loses nothing: the journal already says
        // the copy was in flight, so the partial folder on the other drive is
        // discarded at the next start while the original is still untouched.
        m_proc->kill();
        m_proc->waitForFinished(5000);
    }

    // A worker still deleting or measuring may finish on its own — it writes into
    // counters it shares (shared_ptr) and to disk state the journal covers — but
    // giving it a moment keeps a quick one from being cut off mid-way.
    waitForWorker(m_deleteWatcher, 5000);
    waitForWorker(m_verifyWatcher, 2000);
    waitForWorker(m_sizeWatcher, 2000);
    waitForWorker(m_copyProbeWatcher, 2000);
    // The lock probe only enumerates and calls Restart Manager: it holds nothing
    // of ours and writes nothing, so it needs no more than a moment.
    waitForWorker(m_lockWatcher, 2000);
}

void AppDataMovePanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // A move that finished while the tab was closed left a notice behind; this is
    // where it finally reaches the user.
    if (!m_pendingNotice.isEmpty()) {
        const QString title = m_pendingNoticeTitle.isEmpty() ? I18n::tr("app_move.title")
                                                            : m_pendingNoticeTitle;
        const QString text = m_pendingNotice;
        m_pendingNotice.clear();
        m_pendingNoticeTitle.clear();
        // Queued: the dialog has not finished coming up yet, and a box parented to
        // a widget that is still being shown can end up off screen.
        QTimer::singleShot(0, this, [this, title, text]() { Dialogs::info(this, title, text); });
    }

    if (m_scanned) {
        // The list survived the dialog being closed, but the counts next to the
        // buttons are recomputed from the ticks that are still set.
        refreshSummary();
        return;
    }
    m_scanned = true;
    onScan();
}

void AppDataMovePanel::buildUI()
{
    auto* lay = new QVBoxLayout(this);
    // Same side margins as the sibling tabs (system optimisation, cleanup), so
    // every row here — tip, scan-scope, buttons, progress, list — lines up
    // with the rest of the app instead of touching the tab border.
    lay->setContentsMargins(16, 14, 16, 12);
    lay->setSpacing(10);

    m_tipLabel = new QLabel(I18n::tr("app_move.tip"));
    m_tipLabel->setWordWrap(true);
    lay->addWidget(m_tipLabel);

    // Scope warning: a whole user profile moved as one directory takes the
    // running programs' own data with it and leaves them pointing at a
    // junction they were never designed to follow. Say so before the scan,
    // not after something breaks.
    //
    // The same block carries the "do not delete" marker: a finished move writes
    // exactly this icon into the destination, so showing it here — in the one
    // place the user is already reading before moving anything — is what makes
    // the file on the other drive recognisable instead of mysterious.
    m_warnBar = new QWidget;
    m_warnBar->setObjectName(QStringLiteral("warnBar"));
    auto* warnRow = new QHBoxLayout(m_warnBar);
    warnRow->setContentsMargins(10, 8, 10, 8);
    warnRow->setSpacing(8);

    m_warnIcon = new QLabel(m_warnBar);
    m_warnIcon->setObjectName(QStringLiteral("warnIcon"));
    // Rendered at the screen's pixel ratio so a HiDPI display gets a sharp icon
    // (the .ico carries 16/32/48 px frames and only the matching one is asked
    // for), and at the same logical size everywhere else.
    const QIcon markerIconFile(QStringLiteral(":/moveguard/resources/dont_delete_folder.ico"));
    const qreal warnDpr = m_warnBar->devicePixelRatioF() > 0 ? m_warnBar->devicePixelRatioF() : 1.0;
    QPixmap markerIcon = markerIconFile.pixmap(
        QSize(qRound(18 * warnDpr), qRound(18 * warnDpr)));
    if (!markerIcon.isNull())
        markerIcon.setDevicePixelRatio(warnDpr);
    if (markerIcon.isNull()) {
        // Resource missing (a build without the qrc): the text still says what
        // matters, so drop the empty slot instead of leaving a gap.
        m_warnIcon->hide();
    } else {
        m_warnIcon->setPixmap(markerIcon);
        m_warnIcon->setFixedSize(markerIcon.size());
    }
    warnRow->addWidget(m_warnIcon, 0, Qt::AlignTop);

    m_warnLabel = new QLabel(I18n::tr("app_move.warn"));
    m_warnLabel->setWordWrap(true);
    warnRow->addWidget(m_warnLabel, 1);
    lay->addWidget(m_warnBar);

    // Scan scope: which folders the scan walks. Defaults to this user's data
    // folders, but it can be pointed at other directories (another profile, a
    // second drive, ...).
    auto* scopeBar = new QHBoxLayout;
    scopeBar->setSpacing(8);
    scopeBar->setContentsMargins(0, 4, 0, 4);
    m_scopeLabel = new QLabel(I18n::tr("app_move.scope"));
    scopeBar->addWidget(m_scopeLabel);
    m_scopeCombo = new QComboBox;
    m_scopeCombo->setObjectName(QStringLiteral("typeFilter"));
    m_scopeCombo->setMinimumWidth(260);
    m_scopeCombo->setFixedHeight(kScopeRowHeight);
    connect(m_scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppDataMovePanel::onScopeChanged);
    scopeBar->addWidget(m_scopeCombo);
    m_scopeAddBtn = new QPushButton(I18n::tr("app_move.scope_add"));
    m_scopeAddBtn->setObjectName(QStringLiteral("ghost"));
    m_scopeAddBtn->setCursor(Qt::PointingHandCursor);
    m_scopeAddBtn->setFixedHeight(kScopeRowHeight);
    connect(m_scopeAddBtn, &QPushButton::clicked, this, &AppDataMovePanel::onAddScope);
    scopeBar->addWidget(m_scopeAddBtn);
    m_scopeRemoveBtn = new QPushButton(I18n::tr("app_move.scope_remove"));
    m_scopeRemoveBtn->setObjectName(QStringLiteral("ghost"));
    m_scopeRemoveBtn->setCursor(Qt::PointingHandCursor);
    m_scopeRemoveBtn->setFixedHeight(kScopeRowHeight);
    connect(m_scopeRemoveBtn, &QPushButton::clicked, this, &AppDataMovePanel::onRemoveScope);
    scopeBar->addWidget(m_scopeRemoveBtn);
    scopeBar->addStretch(1);
    lay->addLayout(scopeBar);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(8);
    m_scanBtn = new QPushButton(I18n::tr("app_move.scan"));
    m_scanBtn->setObjectName(QStringLiteral("ghost"));
    m_scanBtn->setCursor(Qt::PointingHandCursor);
    connect(m_scanBtn, &QPushButton::clicked, this, &AppDataMovePanel::onScan);
    bar->addWidget(m_scanBtn);

    m_moveBtn = new QPushButton(I18n::tr("app_move.move"));
    m_moveBtn->setObjectName(QStringLiteral("primary"));
    m_moveBtn->setCursor(Qt::PointingHandCursor);
    connect(m_moveBtn, &QPushButton::clicked, this, &AppDataMovePanel::onMoveSelected);
    bar->addWidget(m_moveBtn);

    m_summaryLabel = new QLabel;
    bar->addWidget(m_summaryLabel);
    bar->addStretch(1);
    lay->addLayout(bar);

    // Scan progress: only visible while a scan is running, so the user can tell
    // at a glance whether it has finished. The bar uses the app's slim
    // text-less style, so the wording sits in a label beside it.
    m_progressRow = new QWidget;
    auto* progRow = new QHBoxLayout(m_progressRow);
    progRow->setContentsMargins(0, 0, 0, 0);
    progRow->setSpacing(8);
    m_scanStatus = new QLabel;
    progRow->addWidget(m_scanStatus);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    progRow->addWidget(m_progress, 1);
    m_progressRow->setVisible(false);
    lay->addWidget(m_progressRow);

    m_progressHideTimer = new QTimer(this);
    m_progressHideTimer->setSingleShot(true);
    connect(m_progressHideTimer, &QTimer::timeout, this, &AppDataMovePanel::hideProgressWhenIdle);

    m_tree = new QTreeWidget;
    // Named so a harness can tell this tree from the relocation tab's one
    // ("syncTree") without guessing by child order.
    m_tree->setObjectName(QStringLiteral("moveTree"));
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setHeaderLabels({
        I18n::tr("app_move.col_name"),
        I18n::tr("app_move.col_path"),
        I18n::tr("app_move.col_size"),
        I18n::tr("app_move.col_state"),
        I18n::tr("app_move.col_action"),
    });
    // The header is the filter-aware one (see FilterHeaderView.h); everything
    // below configures it exactly as a plain QHeaderView.
    m_header = new FilterHeaderView(Qt::Horizontal, m_tree);
    m_tree->setHeader(m_header);
    auto* hdr = m_header;
    hdr->setSectionResizeMode(0, QHeaderView::Interactive);
    hdr->resizeSection(0, 200);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionResizeMode(2, QHeaderView::Fixed);
    hdr->resizeSection(2, 100);
    // Both of the remaining columns are sized from the text that goes in them —
    // see applyColumnWidths, which is also what a language switch re-runs.
    hdr->setSectionResizeMode(3, QHeaderView::Fixed);
    hdr->setSectionResizeMode(4, QHeaderView::Fixed);
    applyColumnWidths();
    // Every data column is sortable; the list opens on "occupies most first".
    hdr->setSectionsClickable(true);
    hdr->setSortIndicatorShown(true);
    hdr->setSortIndicator(m_sortColumn, m_sortOrder);
    connect(hdr, &QHeaderView::sectionClicked, this, &AppDataMovePanel::onHeaderClicked);
    // The state column is the one worth narrowing down, and it is the only one
    // that gets a funnel. A click on it still sorts; only the funnel's own strip
    // at the right edge opens the filter.
    hdr->setFilterSection(3);
    hdr->setToolTip(I18n::tr(QStringLiteral("app_move.filter_tip")));
    connect(hdr, &FilterHeaderView::filterRequested, this,
            [this](int, const QPoint& globalPos) { onStateFilterRequested(globalPos); });

    connect(m_tree, &QTreeWidget::itemChanged, this, &AppDataMovePanel::onItemChanged);
    // Clicking the path cell opens it in Explorer.
    connect(m_tree, &QTreeWidget::itemClicked, this, &AppDataMovePanel::onPathClicked);
    lay->addWidget(m_tree, 1);

    m_sizeWatcher = new QFutureWatcher<qint64>(this);
    connect(m_sizeWatcher, &QFutureWatcher<qint64>::finished,
            this, &AppDataMovePanel::onSizeReady);

    m_deleteWatcher = new QFutureWatcher<bool>(this);
    connect(m_deleteWatcher, &QFutureWatcher<bool>::finished,
            this, &AppDataMovePanel::onDeleteFinished);

    // Verifying a finished copy walks two whole trees, so it runs beside the UI
    // thread instead of on it.
    m_verifyWatcher = new QFutureWatcher<VerifyResult>(this);
    connect(m_verifyWatcher, &QFutureWatcher<VerifyResult>::finished,
            this, &AppDataMovePanel::onVerifyReady);

    // Copy progress samples the destination size at a low rate: often enough to
    // show the copy moving, rarely enough that it does not compete with robocopy
    // for the same disk.
    m_copyProbeWatcher = new QFutureWatcher<qint64>(this);
    connect(m_copyProbeWatcher, &QFutureWatcher<qint64>::finished,
            this, &AppDataMovePanel::onCopyProbeReady);

    // "Which programs are using this folder?" walks the tree to sample it, which
    // is exactly the IO the UI thread must not do — a locked folder can hold
    // hundreds of thousands of files.
    m_lockWatcher = new QFutureWatcher<QVector<WinApi::LockingProcess>>(this);
    connect(m_lockWatcher, &QFutureWatcher<QVector<WinApi::LockingProcess>>::finished,
            this, &AppDataMovePanel::onLockProbeReady);
    m_copyTimer = new QTimer(this);
    m_copyTimer->setInterval(2500);
    connect(m_copyTimer, &QTimer::timeout, this, &AppDataMovePanel::onCopyTick);

    // The wording of the progress row is refreshed on its own, faster clock: it
    // only reads the counters the workers publish, so it costs no I/O at all and
    // the elapsed time keeps ticking during the phases that measure nothing.
    m_jobTimer = new QTimer(this);
    m_jobTimer->setInterval(1000);
    connect(m_jobTimer, &QTimer::timeout, this, &AppDataMovePanel::onJobTick);

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                // Every helper process hands control to the step that follows the
                // one it was started for. For all of them but the copy, that next
                // step is recorded the moment the process is launched. The copy is
                // the exception: its launch has to stay recognisable as "the copy"
                // so that a FailedToStart can be reported as "the copy never ran".
                // So it is advanced here, once the process has really finished —
                // without this the state machine falls straight back into the copy
                // phase and copies the same folder over and over, which is exactly
                // the "stuck at 正在复制, progress restarts, files already there"
                // behaviour this fixes.
                if (m_movePhase == kMvCopy)
                    m_movePhase = kMvCopied;
                runMoveStep();
            });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        // The helper program never ran, so no "finished" signal is coming.
        // Record it and drive the state machine forward by hand.
        m_procFailedToStart = true;

        if (m_repairActive) {
            repairNext();
            return;
        }
        if (m_restoreActive) {
            // Whatever the failed step was, the folder is still a junction and
            // its data is still on the other drive.
            m_restoreFailed = true;
            if (m_restoreRow >= 0 && m_restoreRow < static_cast<int>(m_dirs.size())) {
                m_dirs[m_restoreRow].state = kStateMoved;
                const int row = rowOfDir(m_restoreRow);
                if (row >= 0)
                    refreshRow(row);
            }
            finishRestore();
            return;
        }
        // A move step could not be launched. Only the copy phase is fatal on its
        // own: the original has not been touched yet, so the folder is simply
        // reported as unfinished. Every later step is owned by the phase that
        // started it, which is asked to deal with the flag.
        if (m_movePhase == kMvCopy) {
            if (m_movePos < static_cast<int>(m_moveQueue.size())) {
                DataDir& d = m_dirs[m_moveQueue[m_movePos]];
                d.state = kStateFailed;
                d.journalPhase = kPhaseNone;
                removeJournalEntry(d.path);
                const int row = rowOfDir(m_moveQueue[m_movePos]);
                if (row >= 0)
                    refreshRow(row);
                ++m_moveFailed;
                ++m_movePos;
            }
        }
        runMoveStep();
    });

    refreshTheme();
    retranslate();
}

void AppDataMovePanel::retranslate()
{
    m_tipLabel->setText(I18n::tr("app_move.tip"));
    m_warnLabel->setText(I18n::tr("app_move.warn"));
    m_scopeLabel->setText(I18n::tr("app_move.scope"));
    m_scopeAddBtn->setText(I18n::tr("app_move.scope_add"));
    m_scopeRemoveBtn->setText(I18n::tr("app_move.scope_remove"));
    // Combo entries hold localized text, so they are rebuilt on a language
    // change (rebuildScopeCombo keeps the selection and does not rescan).
    rebuildScopeCombo();
    m_scanBtn->setText(I18n::tr("app_move.scan"));
    m_moveBtn->setText(I18n::tr("app_move.move"));
    m_tree->setHeaderLabels({
        I18n::tr("app_move.col_name"),
        I18n::tr("app_move.col_path"),
        I18n::tr("app_move.col_size"),
        I18n::tr("app_move.col_state"),
        I18n::tr("app_move.col_action"),
    });
    // The funnel's tooltip names the states it filters on, in the language they
    // are now written in: the header outlives a language change, so it has to be
    // told again.
    if (m_header) {
        m_header->setFilterActive(stateFilterActive());
        m_header->setToolTip(stateFilterTooltip());
    }
    // Both are measured from the labels, so both change with the language: an
    // English "Remove leftover" does not fit a column sized for "清理残留".
    applyColumnWidths();
    // The state column is part of the ordering, so the rows themselves change.
    rebuildTree();
}

void AppDataMovePanel::refreshTheme()
{
    m_tipLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.5;")
        .arg(QString::fromLatin1(C::TEXT_MUTED())));
    // The amber block is the bar, not the label: the marker icon sits inside
    // the same frame, so both share one background and one border.
    m_warnBar->setStyleSheet(QStringLiteral(
        "#warnBar { background: %1; border: 1px solid %2; border-radius: 6px; }")
        .arg(QString::fromLatin1(C::SURFACE()), QString::fromLatin1(C::WARNING())));
    m_warnLabel->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: %1; background: transparent; border: none; line-height: 1.5;")
        .arg(QString::fromLatin1(C::WARNING())));
    m_scopeLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));
    m_scanStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));
    m_summaryLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 500; color: %1;")
        .arg(QString::fromLatin1(C::PRIMARY())));
}

void AppDataMovePanel::onScan()
{
    // Rebuilding the list invalidates every index a running job is holding, so a
    // scan is simply refused while one is in flight.
    if (m_busy || m_repairActive)
        return;

    m_scanned = true;
    collectCandidates();
    applySort();
    rebuildTree();

    m_sizeJobRow = -1;
    beginScanProgress();
    startNextSizeJob();
    refreshSummary();
}

void AppDataMovePanel::collectCandidates()
{
    m_dirs.clear();
    m_orphanOlds.clear();
    m_installedNames = installedProgramNames();

    // Whatever the scope combo currently selects (this user's data folders by
    // default, or a directory the user picked).
    QStringList roots = m_scanRoots.isEmpty() ? defaultScanRoots() : m_scanRoots;

    // The folder this app keeps its own settings in must never be listed: the
    // journal that tracks relocations lives in there.
    const QString ownName = QCoreApplication::applicationName().toLower();

    for (const QString& root : roots) {
        if (root.isEmpty())
            continue;
        QDir dir(root);
        const QFileInfoList entries =
            dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : entries) {
            const QString name = fi.fileName();
            if (name.toLower().endsWith(kOldSuffix) && name.size() > kOldSuffix.size()) {
                // A copy this app renamed aside and never got to delete. It is
                // not a data folder and must never be listed as a movable one;
                // the recovery pass decides whether it can go.
                m_orphanOlds << QDir::toNativeSeparators(fi.absoluteFilePath());
                continue;
            }
            if (name.startsWith(QLatin1Char('.')) || isSkippedFolder(name))
                continue;
            if (!ownName.isEmpty() && name.toLower() == ownName)
                continue;

            const QString full = fi.absoluteFilePath();
            const bool linked = isReparsePoint(full);
            // A plain folder that already lives on another drive is none of our
            // business; a linked one is, because it can be restored.
            if (!linked && !isOnSystemDrive(full))
                continue;

            DataDir d;
            d.path = QDir::toNativeSeparators(full);
            d.name = name;
            d.matchedApp = matchInstalledProgram(name, m_installedNames);
            if (linked) {
                d.linkTarget = linkTargetOf(full);
                d.state = kStateMoved;
            }
            m_dirs.push_back(d);
        }
    }

    loadJournal();
    // Anything the journal says was interrupted gets reconciled now, before the
    // list is shown, so the user never sees a state they would have to fix by
    // hand.
    startRepair();
}

// --------------------------------------------------------------------------- //
// Scan scope (which folders the scan walks)
// --------------------------------------------------------------------------- //
QStringList AppDataMovePanel::defaultScanRoots() const
{
    // Roaming, Local AND LocalLow (plus Documents). The list lives in
    // ScanRoots.h so the set of roots is one tested place rather than a few
    // lines here — LocalLow was silently missing while it was spelled out
    // inline, and it is ordinary user data: browsers' sandboxed helpers,
    // Unity/Unreal titles and installers all write there.
    return userDataScanRoots();
}

void AppDataMovePanel::loadScopeSettings()
{
    QSettings reg(kRegRoot, QSettings::NativeFormat);
    m_customRoots = reg.value(kRegCustomDirs).toStringList();
    m_scopeIndex = reg.value(kRegScopeIndex, 0).toInt();
    if (m_scopeIndex < 0 || m_scopeIndex > m_customRoots.size())
        m_scopeIndex = 0;
    m_lastTargetRoot = reg.value(kRegLastTarget).toString();
}

void AppDataMovePanel::saveScopeSettings()
{
    QSettings reg(kRegRoot, QSettings::NativeFormat);
    reg.setValue(kRegCustomDirs, m_customRoots);
    reg.setValue(kRegScopeIndex, m_scopeIndex);
    reg.setValue(kRegLastTarget, m_lastTargetRoot);
}

QString AppDataMovePanel::pickerStartDir() const
{
    // Only an existing folder is a useful starting point: a drive that was
    // unplugged (or a folder that was deleted) must not make the picker open
    // somewhere that no longer exists.
    if (!m_lastTargetRoot.isEmpty() && QDir(m_lastTargetRoot).exists())
        return m_lastTargetRoot;
    // Nothing remembered yet, but an interrupted move knows where its copy went.
    // Offering that drive is what lets picking the folder back up land on the
    // same destination as before — which is exactly the case where a new root
    // would mean copying gigabytes a second time. The destination is
    // root\<source parent>\<name>, so the root is two levels up from it.
    for (const DataDir& d : m_dirs) {
        if (d.moveTarget.isEmpty())
            continue;
        const QString root = QFileInfo(QFileInfo(d.moveTarget).path()).path();
        if (!root.isEmpty() && root != QStringLiteral("/") && QDir(root).exists())
            return root;
    }
    return QDir::homePath();
}

void AppDataMovePanel::rememberTargetRoot(const QString& root)
{
    if (root.isEmpty())
        return;
    const QString native = QDir::toNativeSeparators(QDir::cleanPath(root));
    if (native == m_lastTargetRoot)
        return;
    m_lastTargetRoot = native;
    QSettings reg(kRegRoot, QSettings::NativeFormat);
    reg.setValue(kRegLastTarget, m_lastTargetRoot);
}

void AppDataMovePanel::rebuildScopeCombo()
{
    // Rebuilding must not look like a user action: block the change signal so
    // no rescan is triggered (the caller decides whether to scan).
    const QSignalBlocker blocker(m_scopeCombo);
    m_scopeCombo->clear();
    m_scopeCombo->addItem(I18n::tr("app_move.scope_default"), QString());
    for (const QString& dir : m_customRoots)
        m_scopeCombo->addItem(QDir::toNativeSeparators(dir), dir);

    if (m_scopeIndex < 0 || m_scopeIndex >= m_scopeCombo->count())
        m_scopeIndex = 0;
    m_scopeCombo->setCurrentIndex(m_scopeIndex);

    // Apply the selection: the default entry expands to this user's data
    // folders, any other entry is exactly one directory.
    m_scanRoots = (m_scopeIndex <= 0)
                      ? defaultScanRoots()
                      : QStringList{ m_scopeCombo->itemData(m_scopeIndex).toString() };
    m_scopeRemoveBtn->setEnabled(m_scopeIndex > 0);
}

void AppDataMovePanel::onScopeChanged(int index)
{
    if (index < 0 || index >= m_scopeCombo->count())
        return;
    m_scopeIndex = index;
    m_scanRoots = (index <= 0)
                      ? defaultScanRoots()
                      : QStringList{ m_scopeCombo->itemData(index).toString() };
    m_scopeRemoveBtn->setEnabled(index > 0);
    saveScopeSettings();
    // A new scope is only useful once it has been scanned.
    onScan();
}

void AppDataMovePanel::onAddScope()
{
    const QString start = m_scanRoots.isEmpty() ? QDir::homePath() : m_scanRoots.first();
    const QString dir = QFileDialog::getExistingDirectory(
        this, I18n::tr("app_move.scope_add_title"), start);
    if (dir.isEmpty())
        return;
    const QString native = QDir::toNativeSeparators(QDir::cleanPath(dir));

    // A directory that is already known is not added twice — it is just
    // selected. The built-in default is recognised too, so it cannot be
    // shadowed by an identical custom entry.
    int target = -1;
    if (defaultScanRoots().contains(native)) {
        target = 0;
    } else {
        for (int i = 0; i < m_customRoots.size(); ++i) {
            if (pathKey(m_customRoots[i]) == pathKey(native)) {
                target = i + 1;
                break;
            }
        }
        if (target < 0) {
            m_customRoots << native;
            target = m_customRoots.size();  // select the newly added entry
        }
    }

    const bool scopeChanged = (target != m_scopeIndex);
    m_scopeIndex = target;
    rebuildScopeCombo();
    saveScopeSettings();
    if (scopeChanged)
        onScan();
}

void AppDataMovePanel::onRemoveScope()
{
    if (m_scopeIndex <= 0 || m_scopeIndex > m_customRoots.size())
        return;
    m_customRoots.removeAt(m_scopeIndex - 1);
    m_scopeIndex = 0;
    rebuildScopeCombo();
    saveScopeSettings();
    onScan();
}

// --------------------------------------------------------------------------- //
// Scan progress
// --------------------------------------------------------------------------- //
void AppDataMovePanel::beginScanProgress()
{
    if (m_progressHideTimer)
        m_progressHideTimer->stop();

    m_sizeDone = 0;
    m_sizeTotal = 0;
    for (const DataDir& d : m_dirs) {
        // Exactly the folders startNextSizeJob() will measure.
        if (d.size < 0 && d.state == kStateIdle)
            ++m_sizeTotal;
    }

    m_scanning = m_sizeTotal > 0;
    m_progressRow->setVisible(m_scanning);
    if (!m_scanning)
        return;
    m_progress->setRange(0, m_sizeTotal);
    m_progress->setValue(0);
    m_scanStatus->setText(I18n::tr("app_move.scanning", QMap<QString, QString>{
        {"done", QStringLiteral("0")}, {"total", QString::number(m_sizeTotal)}}));
}

void AppDataMovePanel::updateScanProgress()
{
    if (!m_scanning)
        return;
    if (m_sizeDone > m_sizeTotal)
        m_sizeDone = m_sizeTotal;
    m_progress->setValue(m_sizeDone);
    m_scanStatus->setText(I18n::tr("app_move.scanning", QMap<QString, QString>{
        {"done", QString::number(m_sizeDone)}, {"total", QString::number(m_sizeTotal)}}));
    if (m_sizeDone >= m_sizeTotal)
        finishScanProgress();
}

void AppDataMovePanel::finishScanProgress()
{
    m_scanning = false;
    m_progress->setValue(m_sizeTotal);
    m_scanStatus->setText(I18n::tr("app_move.scan_done",
                                   QMap<QString, QString>{{"total", QString::number(m_sizeTotal)}}));
    // Leave the finished bar on screen briefly so "did it finish?" is answered
    // at a glance, then reclaim the space.
    if (m_progressHideTimer)
        m_progressHideTimer->start(1600);
}

void AppDataMovePanel::hideProgressWhenIdle()
{
    // A failed move is holding the reason on this row. It stays until the next
    // job takes the row over, because hiding it would take the explanation away
    // with it — leaving the user with a row that just says "失败".
    if (m_jobFailed)
        return;
    // The row is shared with the move job, so it may only be reclaimed when
    // neither job is using it.
    if (!m_scanning && !m_moveActive)
        m_progressRow->setVisible(false);
}

// --------------------------------------------------------------------------- //
// Job progress — one bar for the folder the move is working on
// --------------------------------------------------------------------------- //
// The move of a single folder runs through four phases, and only the first one
// has a number of its own. The bar therefore carries a fixed 0..1000 scale per
// folder, split by weight across the phases (see kCopyBase and friends), and
// every phase fills its own slice:
//
//   copy        0..700  bytes landing on the other drive (sampled destination)
//   verify    700..860  both trees measured, half a slice each
//   hand over 860..920  rename the original aside + leave the junction
//   cleanup   920..1000 bytes of the original actually removed
//
// Only ever forwards: the value is clamped to the highest one already drawn, so
// a phase that cannot measure itself (unknown folder size, for instance) parks
// the bar at its own start instead of knocking it back to zero. That reset —
// arriving at 100% and then starting over — was what made a long move look like
// it had restarted.
void AppDataMovePanel::beginJobProgress(const DataDir& d)
{
    m_progressHideTimer->stop();
    // This folder is taking the row over, so the previous folder's failure
    // reason is no longer what the row is about.
    m_jobFailed = false;
    m_jobActive = true;
    m_jobStep = kJobCopy;
    m_jobTotal = d.size;
    m_jobDone = 0;
    m_jobPerMille = 0;
    m_walk.reset();
    m_freed.reset();
    m_jobClock.start();
    m_progressRow->setVisible(true);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    updateJobProgress();
    m_jobTimer->start();
    // The destination is watched from the start, at the cheapest interval until a
    // sample says how expensive watching this particular tree actually is.
    m_copyTimer->setInterval(kCopyProbeMinMs);
    m_copyTimer->start();
}

void AppDataMovePanel::setJobStep(int step)
{
    if (!m_jobActive || m_jobStep == step)
        return;
    m_jobStep = step;
    updateJobProgress();
}

void AppDataMovePanel::endJobProgress()
{
    m_jobActive = false;
    m_jobTimer->stop();
    m_copyTimer->stop();
}

// Relays a helper process's own words into the log. Without this, a failure that
// only robocopy knows about left no trace anywhere: the process was run with its
// output suppressed and nothing ever read it, so the reason was thrown away.
// Reads the 8-bit code page because that is what these tools write.
QString AppDataMovePanel::logProcResult(const char* what)
{
    if (!m_proc)
        return QString();

    if (m_procFailedToStart) {
        Logger::error(QStringLiteral("[move] %1: could not start (program=%2)")
                          .arg(QLatin1String(what), m_proc->program()));
        return I18n::tr(QStringLiteral("app_move.fail_no_cmd"));
    }

    // Read the helper's output exactly once — it is consumed here — and hand the
    // reason back so the caller can put it on the row without reading it again
    // (a second read returns nothing).
    const QString output =
        QString::fromLocal8Bit(m_proc->readAllStandardOutput()).trimmed();
    const int code = m_proc->exitCode();
    const Diag diag = diagnoseOutput(output);

    Logger::info(QStringLiteral("[move] %1: %2, reason=\"%3\", output=\"%4\"")
                     .arg(QLatin1String(what), describeRobocopyExit(code),
                          diag.reason.isEmpty() ? QStringLiteral("-") : diag.reason,
                          diag.detail.isEmpty() ? QStringLiteral("-") : diag.detail));

    // An exit code with no readable line still has to say something the user can
    // act on, so the code itself becomes the reason.
    if (!diag.reason.isEmpty())
        return diag.reason;
    return I18n::tr(QStringLiteral("app_move.fail_errno"),
                    QMap<QString, QString>{{"code", QString::number(code)},
                                           {"file", QStringLiteral("-")},
                                           {"why", describeRobocopyExit(code)}});
}

// A failed folder must not be a dead end. The reason is kept on the row, put on
// the progress row where the user was already looking, and written to the log;
// and because the row stays on screen, hiding it with the idle timer is stopped
// until the next job takes the row over.
void AppDataMovePanel::failMove(int dirIndex, const QString& key,
                                const QMap<QString, QString>& args)
{
    const QString text = I18n::tr(key, args);
    const QString path = (dirIndex >= 0 && dirIndex < static_cast<int>(m_dirs.size()))
                             ? m_dirs[dirIndex].path
                             : QStringLiteral("?");
    // journal phase as its raw number: the phase names live further down this
    // file and are not visible from here.
    Logger::error(QStringLiteral("[move] FAILED %1: %2 (state=%3, journalPhase=%4)")
                      .arg(path, text)
                      .arg(dirIndex >= 0 && dirIndex < static_cast<int>(m_dirs.size())
                               ? m_dirs[dirIndex].state
                               : -1)
                      .arg(dirIndex >= 0 && dirIndex < static_cast<int>(m_dirs.size())
                               ? m_dirs[dirIndex].journalPhase
                               : -1));

    if (dirIndex >= 0 && dirIndex < static_cast<int>(m_dirs.size())) {
        DataDir& d = m_dirs[dirIndex];
        d.failKey = key;
        d.failArgs = args;
        const int row = rowOfDir(dirIndex);
        if (row >= 0)
            refreshRow(row);
    }

    // Hold the row: the next tick must not paint over the reason and the idle
    // timer must not reclaim it.
    m_jobFailed = true;
    m_jobTimer->stop();
    m_copyTimer->stop();
    m_progressHideTimer->stop();
    m_progressRow->setVisible(true);
    m_progress->setRange(0, 1000);
    m_scanStatus->setText(text);
}

// The folder is finished: draw the bar at the end of the scale. Used where the
// last step of a folder succeeds, because the counter it was reading can have
// passed its final value between two ticks.
void AppDataMovePanel::markJobComplete()
{
    if (!m_jobActive)
        return;
    m_jobPerMille = 1000;
    m_progress->setRange(0, 1000);
    m_progress->setValue(1000);
}

void AppDataMovePanel::updateJobProgress()
{    if (!m_jobActive)
        return;

    const int perMille = jobPerMille();
    if (perMille > m_jobPerMille)
        m_jobPerMille = perMille;  // never drawn backwards within one folder
    m_progress->setValue(m_jobPerMille);

    const qint64 elapsedSec = qMax<qint64>(0, m_jobClock.elapsed() / 1000);
    const QString time = QStringLiteral("%1:%2")
                             .arg(elapsedSec / 60)
                             .arg(elapsedSec % 60, 2, 10, QLatin1Char('0'));

    QString text;
    switch (m_jobStep) {
    case kJobCopy:
        text = m_jobTotal > 0
                   ? I18n::tr("app_move.copying", QMap<QString, QString>{
                         {"done", humanSize(qMin(m_jobDone, m_jobTotal))},
                         {"total", humanSize(m_jobTotal)},
                         {"time", time}})
                   : I18n::tr("app_move.copying_unknown",
                              QMap<QString, QString>{{"time", time}});
        break;
    case kJobVerify: {
        // Which of the two trees is being measured right now; the copy itself is
        // already complete and sitting on the other drive at this point.
        const bool onCopy = m_walk && m_walk->stage.load(std::memory_order_relaxed) != 0;
        text = I18n::tr(onCopy ? "app_move.verifying_copy" : "app_move.verifying",
                        QMap<QString, QString>{{"time", time}});
        break;
    }
    case kJobFinish:
        text = I18n::tr("app_move.finishing", QMap<QString, QString>{{"time", time}});
        break;
    default:  // kJobCleanup
        text = m_jobTotal > 0
                   ? I18n::tr("app_move.cleaning", QMap<QString, QString>{
                         {"done", humanSize(qMin(m_jobDone, m_jobTotal))},
                         {"total", humanSize(m_jobTotal)},
                         {"time", time}})
                   : I18n::tr("app_move.cleaning_unknown",
                              QMap<QString, QString>{{"time", time}});
        break;
    }

    // Several folders in one go: say which one this is, so the bar starting over
    // on the next folder reads as "next item" instead of "it restarted".
    if (m_moveQueue.size() > 1 && m_movePos < static_cast<int>(m_moveQueue.size())) {
        text += QLatin1Char(' ') + I18n::tr("app_move.batch_of", QMap<QString, QString>{
            {"index", QString::number(m_movePos + 1)},
            {"count", QString::number(static_cast<int>(m_moveQueue.size()))}});
    }
    m_scanStatus->setText(text);
}

// Where on the 0..1000 scale the current phase stands. The copy and the cleanup
// are byte-driven (both counters only ever grow); the verify phase is driven by
// the walker's running total, split between the original and the copy. A phase
// with nothing to measure reports its own start.
int AppDataMovePanel::jobPerMille() const
{
    const auto fraction = [](qint64 value, qint64 total) {
        if (total <= 0)
            return 0.0;
        const double f = static_cast<double>(value) / static_cast<double>(total);
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    };

    switch (m_jobStep) {
    case kJobCopy:
        if (m_jobTotal <= 0)
            return kCopyBase;
        return kCopyBase + static_cast<int>(kCopySpan * fraction(m_jobDone, m_jobTotal));

    case kJobVerify: {
        if (!m_walk)
            return kVerifyBase;
        const qint64 src = m_walk->srcBytes.load(std::memory_order_relaxed);
        const qint64 dst = m_walk->dstBytes.load(std::memory_order_relaxed);
        // The scan's figure is the denominator while it is known; otherwise the
        // larger of the two running totals (the walk reports the copy last).
        const qint64 denom = m_jobTotal > 0 ? m_jobTotal : qMax(src, dst);
        const int half = kVerifySpan / 2;
        if (m_walk->stage.load(std::memory_order_relaxed) == 0)
            return kVerifyBase + static_cast<int>(half * fraction(src, denom));
        return kVerifyBase + half + static_cast<int>(half * fraction(dst, denom));
    }

    case kJobFinish:
        // Both steps are instant metadata operations; the wording carries them.
        return kFinishBase + kFinishSpan / 2;

    default: {  // kJobCleanup
        const qint64 freed = m_freed ? m_freed->load(std::memory_order_relaxed) : 0;
        if (m_jobTotal <= 0)
            return kCleanupBase;
        return kCleanupBase + static_cast<int>(kCleanupSpan * fraction(freed, m_jobTotal));
    }
    }
}

// The wording is refreshed every second; the numbers behind it come from the
// workers, so this reads counters and repaints, and never measures anything
// itself.
void AppDataMovePanel::onJobTick()
{
    if (!m_jobActive)
        return;
    // The cleanup phase publishes through a counter instead of a sample: every
    // file the worker removes adds its size, so there is nothing to poll but it.
    if (m_jobStep == kJobCleanup && m_freed)
        m_jobDone = m_freed->load(std::memory_order_relaxed);
    updateJobProgress();
}

void AppDataMovePanel::onCopyTick()
{
    if (!m_jobActive || m_jobStep != kJobCopy)
        return;
    if (m_copyProbeWatcher->isRunning())
        return;
    if (m_movePos >= static_cast<int>(m_moveQueue.size()))
        return;
    const DataDir& d = m_dirs[m_moveQueue[m_movePos]];
    if (d.moveTarget.isEmpty())
        return;
    m_copyProbeStartMs = QDateTime::currentMSecsSinceEpoch();
    m_copyProbeWatcher->setFuture(QtConcurrent::run(directorySize, d.moveTarget));
}

void AppDataMovePanel::onCopyProbeReady()
{
    if (!m_jobActive || m_jobStep != kJobCopy)
        return;
    // A folder can only grow while the copy runs, so a sample never goes
    // backwards unless the target was replaced — keep the largest one.
    m_jobDone = qMax(m_jobDone, m_copyProbeWatcher->result());
    updateJobProgress();

    // How much did watching it cost? The next sample is scheduled at twice that,
    // bounded at both ends, so a cheap tree is watched closely and an enormous one
    // is left alone most of the time.
    const qint64 cost = QDateTime::currentMSecsSinceEpoch() - m_copyProbeStartMs;
    qint64 next = cost * 2;
    if (next < kCopyProbeMinMs)
        next = kCopyProbeMinMs;
    if (next > kCopyProbeMaxMs)
        next = kCopyProbeMaxMs;
    if (m_copyTimer->interval() != static_cast<int>(next))
        m_copyTimer->setInterval(static_cast<int>(next));
}

void AppDataMovePanel::startNextSizeJob()
{
    for (int i = 0; i < static_cast<int>(m_dirs.size()); ++i) {
        if (m_dirs[i].size >= 0)
            continue;
        // A moved folder occupies nothing on this drive any more; measuring the
        // junction would just report the link itself.
        if (m_dirs[i].state != kStateIdle)
            continue;
        m_sizeJobRow = i;
        const QString path = m_dirs[i].path;
        // Size pass runs off the UI thread; these folders can hold GBs.
        m_sizeWatcher->setFuture(QtConcurrent::run(directorySize, path));
        return;
    }
}

void AppDataMovePanel::onSizeReady()
{
    if (m_sizeJobRow >= 0 && m_sizeJobRow < static_cast<int>(m_dirs.size())) {
        m_dirs[m_sizeJobRow].size = m_sizeWatcher->result();
        if (m_sortColumn == 2) {
            // The list is ordered by size, so a folder that was just measured
            // has to take its place right away.
            applySort();
            rebuildTree();
        } else {
            const int row = rowOfDir(m_sizeJobRow);
            if (row >= 0)
                refreshRow(row);
        }
    }
    m_sizeJobRow = -1;
    if (m_scanning) {
        ++m_sizeDone;
        updateScanProgress();
    }
    startNextSizeJob();
    refreshSummary();
}

bool AppDataMovePanel::relocated(const DataDir& d) const
{
    return (d.state == kStateMoved || d.state == kStateDone) && !d.linkTarget.isEmpty();
}

QString AppDataMovePanel::stateKeyOf(int state)
{
    switch (state) {
    case kStateCopying: return QStringLiteral("app_move.state_copying");
    case kStateChecking: return QStringLiteral("app_move.state_checking");
    case kStateCleaning: return QStringLiteral("app_move.state_cleaning");
    case kStateDeleting: return QStringLiteral("app_move.state_deleting");
    case kStateLinking: return QStringLiteral("app_move.state_linking");
    case kStateVerifying: return QStringLiteral("app_move.state_verifying");
    case kStateRestoring: return QStringLiteral("app_move.state_restoring");
    case kStateDone: return QStringLiteral("app_move.state_done");
    case kStateMoved: return QStringLiteral("app_move.state_moved");
    case kStateQueued: return QStringLiteral("app_move.state_queued");
    case kStateFailed: return QStringLiteral("app_move.state_failed");
    default: return QStringLiteral("app_move.state_idle");
    }
}

int AppDataMovePanel::rowOfDir(int dirIndex) const
{
    for (int i = 0; i < static_cast<int>(m_order.size()); ++i) {
        if (m_order[i] == dirIndex)
            return i;
    }
    return -1;
}

// The two columns whose content is wording rather than data are sized from the
// wording itself. A fixed number is clipped in one language and wasteful in the
// other, and both quantities are known exactly: the longest state name, and the
// widest label the action button can carry.
int AppDataMovePanel::stateColumnWidth() const
{
    // The header's own font, taken from the QHeaderView::section rule in
    // Style.h rather than from the tree: that rule is what the header paints
    // with, and it is not the widget font.
    QFont f = m_tree->font();
    f.setPixelSize(12);
    f.setBold(true);
    const QFontMetrics fm(f);
    int text = 0;
    for (int s = 0; s < MoveSelect::kStateCount; ++s)
        text = qMax(text, fm.horizontalAdvance(I18n::tr(stateKeyOf(s))));
    // 10px of padding either side, the strip the filter funnel is drawn in, and
    // room for the sort arrow — which the header puts at the right edge of
    // whichever column the list happens to be ordered by, i.e. exactly where the
    // funnel also lives.
    return text + 20 + FilterHeaderView::kFilterSlot + kSortArrowRoom;
}

int AppDataMovePanel::actionColumnWidth() const
{
    // Sized from a button styled exactly like the one the column holds, so the
    // padding, the frame and the label are all counted instead of guessed at.
    QPushButton probe;
    probe.setObjectName(QStringLiteral("ghost"));
    probe.setStyleSheet(QStringLiteral("font-size: 11px; padding: 1px 8px;"));
    int need = kActionColWidth;
    for (const char* key : {"app_move.restore", "app_move.abandon", "app_move.clean_residue"}) {
        probe.setText(I18n::tr(key));
        need = qMax(need, probe.sizeHint().width());
    }
    return need + kActionColMargin;
}

void AppDataMovePanel::applyColumnWidths()
{
    QHeaderView* hdr = m_tree->header();
    if (!hdr)
        return;
    hdr->resizeSection(3, stateColumnWidth());
    hdr->resizeSection(4, actionColumnWidth());
}

void AppDataMovePanel::refreshRow(int row)
{
    if (row < 0 || row >= static_cast<int>(m_order.size()))
        return;
    auto* item = m_tree->topLevelItem(row);
    if (!item)
        return;

    const DataDir& d = m_dirs[m_order[row]];
    item->setText(0, d.name);
    item->setToolTip(0, d.matchedApp.isEmpty()
                            ? I18n::tr("app_move.name_unknown")
                            : I18n::tr("app_move.name_matched",
                                       QMap<QString, QString>{{"name", d.matchedApp}}));
    item->setToolTip(1, d.linkTarget.isEmpty()
                            ? I18n::tr("app_move.open_tip",
                                       QMap<QString, QString>{{"path", d.path}})
                            : I18n::tr("app_move.moved_to",
                                       QMap<QString, QString>{{"path", d.linkTarget}}));
    // A folder that lives on the other drive occupies nothing here.
    item->setText(2, relocated(d)
                        ? QStringLiteral("—")
                        : (d.size < 0 ? I18n::tr("app_move.size_pending") : humanSize(d.size)));

    // Waiting behind the folder that is being copied right now.
    const int state = effectiveState(m_order[row]);
    if (state == kStateQueued && !d.moveTarget.isEmpty()) {
        item->setToolTip(1, I18n::tr("app_move.moved_to",
                                     QMap<QString, QString>{{"path", d.moveTarget}}));
    }

    QString stateKey;
    const char* color = C::TEXT_MUTED();
    switch (state) {
    case kStateCopying: stateKey = QStringLiteral("app_move.state_copying"); color = C::PRIMARY(); break;
    case kStateDeleting: stateKey = QStringLiteral("app_move.state_deleting"); color = C::PRIMARY(); break;
    case kStateLinking: stateKey = QStringLiteral("app_move.state_linking"); color = C::PRIMARY(); break;
    case kStateChecking: stateKey = QStringLiteral("app_move.state_checking"); color = C::PRIMARY(); break;
    case kStateCleaning: stateKey = QStringLiteral("app_move.state_cleaning"); color = C::PRIMARY(); break;
    case kStateRestoring: stateKey = QStringLiteral("app_move.state_restoring"); color = C::PRIMARY(); break;
    case kStateQueued: stateKey = QStringLiteral("app_move.state_queued"); color = C::TEXT_MUTED(); break;
    case kStateDone: stateKey = QStringLiteral("app_move.state_done"); color = C::SUCCESS(); break;
    case kStateMoved: stateKey = QStringLiteral("app_move.state_moved"); color = C::PRIMARY(); break;
    case kStateFailed: stateKey = QStringLiteral("app_move.state_failed"); color = C::DANGER(); break;
    default: stateKey = QStringLiteral("app_move.state_idle"); break;
    }
    item->setText(3, I18n::tr(stateKey));
    item->setForeground(3, QColor(QString::fromLatin1(color)));
    // A failed row carries the reason with it: the progress row only shows it
    // until the next job takes that row over, and "失败" on its own leaves the
    // user with nothing to act on.
    const QString failText = d.failKey.isEmpty() ? QString() : I18n::tr(d.failKey, d.failArgs);
    if (!failText.isEmpty())
        item->setToolTip(0, failText);
    item->setToolTip(3, failText);

    // Last column: "restore" for anything whose data already sits on the other
    // drive — including a move that failed after the copy, where restoring is
    // the way to bring the data back. Restoring removes the folder on the other
    // drive, so it is only offered for relocations this app recorded itself; a
    // link somebody else created is left strictly alone.
    //
    // An interrupted move shares the button but not the meaning, and must not
    // read the same. There the data is still untouched in its original place
    // and the only thing that would go away is the copy on the other drive —
    // that is abandoning the move, not restoring anything, and calling it
    // "restore" is what made it look like a required first step before moving
    // the folder again. Finishing it is the move button's job now.
    const bool pendingMove = d.state == kStateFailed && d.knownMove
                             && !d.linkTarget.isEmpty();
    const bool canRestore = d.knownMove && !d.linkTarget.isEmpty()
                            && (relocated(d) || d.state == kStateFailed);
    // A copy this app left behind on the other drive. There is no relocation to
    // undo — the data is home — so the only thing the button can offer is to
    // delete what is still there, and that is what it says.
    const bool hasResidue = !d.residueTarget.isEmpty() && QDir(d.residueTarget).exists();
    if (!canRestore && !d.linkTarget.isEmpty() && relocated(d)) {
        item->setToolTip(0, I18n::tr("app_move.restore_unknown",
                                     QMap<QString, QString>{{"path", d.linkTarget}}));
    }
    auto* restoreBtn = qobject_cast<QPushButton*>(m_tree->itemWidget(item, 4));
    if (canRestore || hasResidue) {
        if (!restoreBtn) {
            restoreBtn = new QPushButton;
            restoreBtn->setObjectName(QStringLiteral("ghost"));
            restoreBtn->setCursor(Qt::PointingHandCursor);
            restoreBtn->setFixedHeight(22);
            restoreBtn->setStyleSheet(QStringLiteral("font-size: 11px; padding: 1px 8px;"));
            // Queued on purpose. Restoring measures the folder again when it
            // finishes, and a measurement that lands in the sort-by-size path
            // rebuilds every row — which deletes this very button. Delivered
            // from the event loop rather than from inside
            // QAbstractButton::click(), the handler can never leave that click
            // finishing up on a widget that is already gone.
            //
            // The folder index is what is carried, not its row: by the time the
            // event is delivered a re-sort may have moved it, and acting on the
            // wrong folder is the one mistake this panel must never make.
            //
            // Which of the two actions it means is read at that same moment, not
            // baked in here: one row can hold a relocation's leftover and then
            // stop holding it, and the button outlives the change.
            const int stableDir = m_order[row];
            connect(restoreBtn, &QPushButton::clicked, this, [this, stableDir]() {
                const int r = rowOfDir(stableDir);
                if (r < 0)
                    return;
                if (!m_dirs[stableDir].residueTarget.isEmpty())
                    onCleanResidue(r);
                else
                    onRestore(r);
            }, Qt::QueuedConnection);
            m_tree->setItemWidget(item, 4, restoreBtn);
        }
        restoreBtn->setText(I18n::tr(
            hasResidue ? QStringLiteral("app_move.clean_residue")
                       : (pendingMove ? QStringLiteral("app_move.abandon")
                                      : QStringLiteral("app_move.restore"))));
        restoreBtn->setToolTip(
            hasResidue
                ? I18n::tr(QStringLiteral("app_move.clean_residue_tip"),
                           QMap<QString, QString>{{"path", d.residueTarget}})
                : I18n::tr(pendingMove ? QStringLiteral("app_move.abandon_tip")
                                       : QStringLiteral("app_move.restore_tip")));
    }
    if (restoreBtn) {
        // Hidden rather than deleted: refreshRow can run from inside the button's
        // own click handler, where deleting the sender would be unsafe.
        restoreBtn->setVisible(canRestore || hasResidue);
        restoreBtn->setEnabled((canRestore || hasResidue) && !m_busy);
        // The width above is an estimate made before any row existed; this is
        // the button itself. Growing the column is the only correction it can
        // ever need — a label that does not fit is a button that reads as
        // broken, and the column is fixed-size, so it can only be too narrow.
        if (QHeaderView* hdr = m_tree->header()) {
            const int need = restoreBtn->sizeHint().width() + kActionColMargin;
            if (hdr->sectionSize(4) < need)
                hdr->resizeSection(4, need);
        }
    }
}

void AppDataMovePanel::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (!item || column != 0)
        return;
    const int row = m_tree->indexOfTopLevelItem(item);
    if (row < 0 || row >= static_cast<int>(m_order.size()))
        return;
    // The tick is kept next to the folder, because a re-sort throws the items
    // away and rebuilds them.
    m_dirs[m_order[row]].checked = (item->checkState(0) == Qt::Checked);
    refreshSummary();
}

void AppDataMovePanel::refreshSummary()
{
    int count = 0;
    qint64 total = 0;
    int visible = 0;
    for (int i = 0; i < static_cast<int>(m_order.size()); ++i) {
        const int dirIndex = m_order[i];
        auto* item = m_tree->topLevelItem(i);
        // A hidden row counts for nothing: it is not on screen, so a total that
        // includes it would describe a selection the user cannot see or unmake.
        if (!item || item->isHidden())
            continue;
        ++visible;
        if (item->checkState(0) != Qt::Checked)
            continue;
        ++count;
        if (m_dirs[dirIndex].size > 0)
            total += m_dirs[dirIndex].size;
    }
    // A filter that hides everything is worth saying out loud: an empty list
    // otherwise reads as a scan that found nothing at all.
    if (visible == 0 && stateFilterActive()) {
        m_summaryLabel->setText(I18n::tr(QStringLiteral("app_move.filter_empty")));
    } else {
        m_summaryLabel->setText(I18n::tr("app_move.summary", QMap<QString, QString>{
            {"count", QString::number(count)},
            {"size", humanSize(total)}}));
    }
    // Driven by what can actually be started, not by what is ticked: during a
    // move the folders already on their way stay ticked but must not count, and
    // a scan, a restore or a recovery owns the state machine outright.
    const bool canStart = !m_scanning && !m_restoreActive && !m_repairActive
                          && !collectMoveSelection().empty();
    m_moveBtn->setEnabled(canStart);
}

// --------------------------------------------------------------------------- //
// State filter
// --------------------------------------------------------------------------- //
int AppDataMovePanel::effectiveState(int dirIndex) const
{
    if (dirIndex < 0 || dirIndex >= static_cast<int>(m_dirs.size()))
        return kStateIdle;
    const DataDir& d = m_dirs[dirIndex];
    if (d.state == kStateIdle && queueIndexOf(dirIndex) >= 0)
        return kStateQueued;
    return d.state;
}

bool AppDataMovePanel::passesStateFilter(int dirIndex) const
{
    return MoveSelect::passesFilter(effectiveState(dirIndex), m_stateFilter.data());
}

bool AppDataMovePanel::stateFilterActive() const
{
    for (int i = 0; i < MoveSelect::kFilterOrderCount; ++i) {
        if (m_stateFilter[MoveSelect::kFilterOrder[i]])
            return true;
    }
    return false;
}

QSet<int> AppDataMovePanel::stateFilterSelection() const
{
    QSet<int> out;
    for (int i = 0; i < MoveSelect::kFilterOrderCount; ++i) {
        const int s = MoveSelect::kFilterOrder[i];
        if (m_stateFilter[s])
            out.insert(s);
    }
    return out;
}

QVector<StateFilterEntry> AppDataMovePanel::stateFilterEntries() const
{
    // Counted over the whole list rather than over what is visible: the panel is
    // the one place that has to be able to say "nothing is in that state any
    // more", which a count of visible rows could never do.
    QMap<int, int> counts;
    for (int i = 0; i < static_cast<int>(m_dirs.size()); ++i)
        ++counts[effectiveState(i)];

    QVector<StateFilterEntry> out;
    for (int i = 0; i < MoveSelect::kFilterOrderCount; ++i) {
        const int s = MoveSelect::kFilterOrder[i];
        if (s < 0 || s >= MoveSelect::kStateCount)
            continue;
        const int n = counts.value(s, 0);
        // A state the list does not hold is not offered — unless it is one the
        // filter is already on. That one has to stay listed, or a state that
        // emptied out (the last failed folder was just cleaned up) would take
        // its own tick with it and the filter could never be undone from here.
        if (n == 0 && !m_stateFilter[s])
            continue;
        StateFilterEntry e;
        e.state = s;
        e.count = n;
        e.label = I18n::tr(QStringLiteral("app_move.filter_item"),
                           QMap<QString, QString>{{"state", I18n::tr(stateKeyOf(s))},
                                                  {"count", QString::number(n)}});
        out << e;
    }
    return out;
}

QString AppDataMovePanel::stateFilterTooltip() const
{
    QStringList names;
    for (int i = 0; i < MoveSelect::kFilterOrderCount; ++i) {
        const int s = MoveSelect::kFilterOrder[i];
        if (m_stateFilter[s])
            names << I18n::tr(stateKeyOf(s));
    }
    if (names.isEmpty())
        return I18n::tr(QStringLiteral("app_move.filter_tip"));
    return I18n::tr(QStringLiteral("app_move.filter_active"),
                    QMap<QString, QString>{{"states",
                                            names.join(I18n::tr(QStringLiteral("app_move.filter_join")))}});
}

void AppDataMovePanel::onStateFilterRequested(const QPoint& globalPos)
{
    // A panel per opening, torn down when it closes. Its entries and its ticks
    // both come from the list as it stands right now, so a panel kept alive
    // between openings would only carry the last one's contents — and it would
    // be reopened while still on screen, which is the one moment a widget's
    // layout is least willing to recompute itself.
    auto* popup = new StateFilterPopup(this);
    connect(popup, &StateFilterPopup::filterChanged, this,
            [this](const QSet<int>& states) {
                std::array<bool, MoveSelect::kStateCount> next{};
                for (int s : states) {
                    if (s >= 0 && s < MoveSelect::kStateCount)
                        next[s] = true;
                }
                if (next == m_stateFilter)
                    return;
                m_stateFilter = next;
                applyStateFilter();
            });
    popup->showFor(stateFilterEntries(), stateFilterSelection(), globalPos);
}

void AppDataMovePanel::applyStateFilter()
{
    if (m_header) {
        m_header->setFilterActive(stateFilterActive());
        m_header->setToolTip(stateFilterTooltip());
    }
    rebuildTree();
}

// --------------------------------------------------------------------------- //
// Sorting
// --------------------------------------------------------------------------- //
void AppDataMovePanel::applySort()
{
    m_order.resize(m_dirs.size());
    for (int i = 0; i < static_cast<int>(m_dirs.size()); ++i)
        m_order[i] = i;
    std::stable_sort(m_order.begin(), m_order.end(), [this](int a, int b) {
        return rowLessThan(m_dirs[a], m_dirs[b]);
    });
}

bool AppDataMovePanel::rowLessThan(const DataDir& a, const DataDir& b) const
{
    int cmp = 0;
    switch (m_sortColumn) {
    case 0:
        cmp = a.name.compare(b.name, Qt::CaseInsensitive);
        break;
    case 1:
        cmp = a.path.compare(b.path, Qt::CaseInsensitive);
        break;
    case 3:
        cmp = QString::compare(I18n::tr(stateKeyOf(a.state)), I18n::tr(stateKeyOf(b.state)));
        break;
    case 2:
    default:
        cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
        break;
    }
    if (cmp == 0)
        cmp = a.name.compare(b.name, Qt::CaseInsensitive);
    return m_sortOrder == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
}

void AppDataMovePanel::onHeaderClicked(int column)
{
    // The last column only holds the restore button, so there is nothing to
    // order by.
    if (column < 0 || column > 3)
        return;
    if (column == m_sortColumn) {
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder
                                                          : Qt::AscendingOrder;
    } else {
        m_sortColumn = column;
        m_sortOrder = Qt::AscendingOrder;
    }
    setSort(m_sortColumn, m_sortOrder);
}

void AppDataMovePanel::setSort(int column, Qt::SortOrder order)
{
    m_sortColumn = column;
    m_sortOrder = order;
    m_tree->header()->setSortIndicator(column, order);
    m_tree->header()->setSortIndicatorShown(true);
    applySort();
    rebuildTree();
}

void AppDataMovePanel::rebuildTree()
{
    QScrollBar* bar = m_tree->verticalScrollBar();
    const int scrollPos = bar ? bar->value() : 0;

    {
        // Rebuilding the rows must not look like the user ticking boxes.
        const QSignalBlocker blocker(m_tree);
        m_tree->clear();
        for (int i = 0; i < static_cast<int>(m_order.size()); ++i) {
            const DataDir& d = m_dirs[m_order[i]];
            auto* item = new QTreeWidgetItem(m_tree);
            // A folder that already sits behind a junction cannot be moved
            // again, so it gets no checkbox and only offers "restore".
            if (d.state != kStateMoved) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, d.checked ? Qt::Checked : Qt::Unchecked);
            }
            item->setText(1, d.path);
            item->setForeground(1, QColor(QString::fromLatin1(C::TEXT_SEC())));
            // The filter narrows the VIEW, not the list: a row that does not
            // match is hidden and stays where it is. Dropping it from m_order
            // instead would invalidate every row index a running job holds.
            item->setHidden(!passesStateFilter(m_order[i]));
        }
    }

    for (int i = 0; i < static_cast<int>(m_order.size()); ++i)
        refreshRow(i);

    if (bar)
        bar->setValue(scrollPos);
    refreshSummary();
}

// --------------------------------------------------------------------------- //
// Move
// --------------------------------------------------------------------------- //
void AppDataMovePanel::setBusy(bool busy)
{
    m_busy = busy;
    // A scan rebuilds the whole list, so it must not run while folders are being
    // moved around underneath it.
    m_scanBtn->setEnabled(!busy);
    // The move button is NOT switched off here: while a move is running more
    // folders may be queued behind it. refreshSummary() owns the final decision
    // (it also reacts to the selection changing).
    for (int i = 0; i < static_cast<int>(m_order.size()); ++i)
        refreshRow(i);
    refreshSummary();
}

// Where this folder would land under *root*. The name is appended because two
// different parents can easily contain folders of the same name.
QString AppDataMovePanel::dstOf(const QString& root, const QString& name) const
{
    return QDir::toNativeSeparators(QDir::cleanPath(root + QLatin1Char('\\') + name));
}

int AppDataMovePanel::queueIndexOf(int dirIndex) const
{
    for (int i = 0; i < static_cast<int>(m_moveQueue.size()); ++i) {
        if (m_moveQueue[i] == dirIndex)
            return i;
    }
    return -1;
}

std::vector<int> AppDataMovePanel::collectMoveSelection() const
{
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(m_order.size()); ++i) {
        auto* item = m_tree->topLevelItem(i);
        // Hidden by the state filter, so the user cannot see it: moving it would
        // be the panel acting on a selection nobody made.
        if (!item || item->isHidden() || item->checkState(0) != Qt::Checked)
            continue;
        const int dirIndex = m_order[i];
        const DataDir& d = m_dirs[dirIndex];
        // The rule itself lives in MoveSelect.h, where the probe lab can check
        // it: already-relocated rows and somebody else's junctions are out, and
        // this app's own interrupted move — failed state, link target present —
        // is deliberately still in.
        if (!mayStartMove(d.state, d.knownMove, !d.linkTarget.isEmpty()))
            continue;
        // Already waiting in the queue, or being copied right now.
        if (queueIndexOf(dirIndex) >= 0)
            continue;
        out.push_back(dirIndex);
    }
    return out;
}

void AppDataMovePanel::onMoveSelected()
{
    if (m_restoreActive || m_repairActive || m_cleanActive)
        return;

    const std::vector<int> picked = collectMoveSelection();
    if (picked.empty()) {
        // "You ticked nothing" and "everything you ticked is already on its way"
        // are different situations, and only the first one is a mistake.
        bool anyChecked = false;
        for (int i = 0; i < static_cast<int>(m_order.size()); ++i) {
            auto* item = m_tree->topLevelItem(i);
            if (item && item->checkState(0) == Qt::Checked) {
                anyChecked = true;
                break;
            }
        }
        Dialogs::warn(this, I18n::tr("app_move.title"),
                      I18n::tr(anyChecked ? "app_move.already_queued"
                                          : "app_move.none_selected"));
        return;
    }

    // A second batch may well be aimed at a different drive than the one that is
    // still copying — that is why the destination is stored per folder.
    const bool appending = m_moveActive;

    // Opens where the user pointed the last move to, so a machine with a
    // dedicated data drive does not have to be navigated to every time.
    const QString targetRoot = QFileDialog::getExistingDirectory(
        this, I18n::tr("app_move.select_target"), pickerStartDir());
    if (targetRoot.isEmpty())
        return;

    const QString nativeRoot = QDir::toNativeSeparators(QDir::cleanPath(targetRoot));
    rememberTargetRoot(nativeRoot);

    // Where every picked folder lands: MIRRORED by its source parent
    // ("root\Roaming\Name", "root\Local\Name") — always, not just on a name
    // clash — so the destination shows where each folder came from and
    // same-named folders from Roaming and Local can go in one batch without
    // ever colliding. Queued folders for this same root count as taken too.
    // The rules live in MoveDstResolver.h (pure, unit-tested); nothing is
    // ever merged silently — an unresolvable clash and an existing
    // destination with content both stop the batch below.
    QMap<int, QString> dstByDir;
    {
        QStringList queuedRel;
        for (int dirIndex : m_moveQueue) {
            const QString mt = QDir::toNativeSeparators(
                QDir::cleanPath(m_dirs[dirIndex].moveTarget));
            if (pathKey(QFileInfo(mt).path()) != pathKey(nativeRoot))
                continue;   // aimed at a different destination: no clash here
            queuedRel << mt.mid(nativeRoot.length() + 1);
        }
        QStringList names, paths;
        for (int dirIndex : picked) {
            names << m_dirs[dirIndex].name;
            paths << m_dirs[dirIndex].path;
        }
        bool resolved = false;
        // resolveBatchDst keys its result by POSITION in the picked list
        // (0..n-1); remap to the panel's directory indices before any use —
        // a raw .value(dirIndex) misses whenever the selection does not start
        // at 0, and the empty string it returns then made every later check
        // test the current working directory instead of the real destination
        // ("target exists" on an empty drive).
        const QMap<int, QString> byPos =
            resolveBatchDst(nativeRoot, names, paths, queuedRel, &resolved);
        if (!resolved) {
            Dialogs::warn(this, I18n::tr("app_move.title"), I18n::tr("app_move.name_conflict"));
            return;
        }
        for (int k = 0; k < picked.size(); ++k)
            dstByDir.insert(picked.at(k), byPos.value(k));
    }

    // Defensive: every picked folder must have a concrete destination now.
    for (int dirIndex : picked) {
        if (dstByDir.value(dirIndex).isEmpty()) {
            Dialogs::warn(this, I18n::tr("app_move.title"), I18n::tr("app_move.target_invalid"));
            return;
        }
    }

    // Never let source and target overlap: the copy would verify against itself
    // and the original would be deleted afterwards.
    for (int dirIndex : picked) {
        const QString dst = dstByDir.value(dirIndex);
        if (pathsOverlap(m_dirs[dirIndex].path, dst)
            || pathsOverlap(m_dirs[dirIndex].path, nativeRoot)) {
            Dialogs::warn(this, I18n::tr("app_move.title"), I18n::tr("app_move.target_invalid"));
            return;
        }
    }

    // Anything already sitting at the destination is somebody else's data: the
    // copy would merge into it and a failed check would delete the lot.
    // Nothing here is ever overwritten — a destination with content in it
    // stops the batch and the user decides (move elsewhere or clear it).
    //
    // Two destinations are not "somebody else's": one this same job created for
    // an earlier folder of the batch, and the copy an interrupted move of this
    // very folder left on the other drive last time. Reusing the latter is the
    // whole point of being able to pick an unfinished folder back up.
    for (int dirIndex : picked) {
        const QString dst = dstByDir.value(dirIndex);
        const DataDir& d = m_dirs[dirIndex];
        const bool ownCopy = d.knownMove && !d.moveTarget.isEmpty()
                             && pathKey(d.moveTarget) == pathKey(dst);
        if (QDir(dst).exists() && !holdsNothing(dst) && !ownCopy) {
            // Our own leftover from an operation that could not delete it. This
            // used to be a dead end: the user was told a folder with that name
            // already existed, with no way to tell — and no way to fix — that the
            // folder was one this program had put there itself. It is offered as
            // a cleanup instead, and only for a leftover this app recorded; a
            // folder that was always the user's still stops the batch outright.
            if (!d.residueTarget.isEmpty() && pathKey(d.residueTarget) == pathKey(dst)) {
                if (Dialogs::confirm(this, I18n::tr("app_move.title"),
                                     I18n::tr(QStringLiteral("app_move.residue_before_move"),
                                              QMap<QString, QString>{
                                                  {"name", d.name}, {"path", dst}}))) {
                    // Deliberately NOT started behind the cleanup. Two jobs
                    // sharing one progress row and one interactive prompt is a
                    // state machine nobody can reason about; the cleanup runs
                    // now, and the folder is ready to move the moment it is done.
                    startResidueCleanup(dirIndex);
                }
                return;
            }
            Dialogs::warn(this, I18n::tr("app_move.title"),
                          I18n::tr("app_move.target_exists",
                                   QMap<QString, QString>{{"path", dst}}));
            return;
        }
    }

    const QMap<QString, QString> args{
        {"count", QString::number(picked.size())},
        {"path", nativeRoot}};
    if (!Dialogs::confirm(this, I18n::tr("app_move.title"),
                          I18n::tr(appending ? "app_move.confirm_queue"
                                             : "app_move.confirm", args))) {
        return;
    }

    m_batchTargetRoot = nativeRoot;   // a successful move drops the warning icon here
    for (int dirIndex : picked) {
        m_dirs[dirIndex].moveTarget = dstByDir.value(dirIndex);
        m_dirs[dirIndex].state = kStateIdle;
        // A fresh start for this folder, in this batch: the prompt about the
        // programs holding it is offered once per batch, not once per session.
        m_dirs[dirIndex].closeAttempted = false;
    }

    if (appending) {
        // The state machine walks m_moveQueue by index, so appending is safe
        // while it is running — the new folders simply follow the current one.
        for (int dirIndex : picked)
            m_moveQueue.push_back(dirIndex);
        for (int dirIndex : picked) {
            const int row = rowOfDir(dirIndex);
            if (row >= 0)
                refreshRow(row);
        }
        refreshSummary();
        Dialogs::info(this, I18n::tr("app_move.title"),
                      I18n::tr("app_move.queued", QMap<QString, QString>{
                          {"count", QString::number(picked.size())}}));
        return;
    }

    m_moveQueue = picked;
    m_movePos = 0;
    m_moveOk = 0;
    m_moveFailed = 0;
    m_moveSkipped = 0;
    // Every folder starts by finding out who is holding it, not by copying it.
    m_movePhase = kMvPrecheck;
    m_moveActive = true;
    setBusy(true);
    runMoveStep();
}

void AppDataMovePanel::startProc(const QString& program, const QStringList& args)
{
    m_procFailedToStart = false;
    m_proc->setProgram(program);
    m_proc->setArguments(args);
    m_proc->start();
}

void AppDataMovePanel::deleteTreeAsync(const QStringList& paths, int nextPhase,
                                       const std::shared_ptr<std::atomic<qint64>>& freed)
{
    m_movePhase = nextPhase;
    // Deliberately not "cmd /c rmdir /S /Q": a recursive rmdir walks through
    // directory junctions and would take the relocated data with it. This
    // deleter unlinks reparse points instead of following them. The counter is
    // taken by value, so the worker keeps it alive even if the panel is gone by
    // the time the last file disappears.
    m_deleteWatcher->setFuture(QtConcurrent::run([paths, freed]() {
        return WinApi::deletePermanent(paths, freed ? freed.get() : nullptr);
    }));
}

void AppDataMovePanel::onDeleteFinished()
{
    m_deleteOk = m_deleteWatcher->result();
    if (m_repairActive) {
        repairNext();
        return;
    }
    runMoveStep();
}

// The copy finished. Start measuring both trees on a worker thread instead of
// blocking the window on two full walks. The walk publishes its running totals
// into the shared counters, which is what keeps the bar moving here instead of
// standing still at 100% for as long as the measurement takes.
void AppDataMovePanel::startVerify()
{
    const int dirIndex = m_moveQueue[m_movePos];
    DataDir& d = m_dirs[dirIndex];
    // The copy is over: the folder on the other drive is complete, and what is
    // left is checking it. Saying so is the difference between "copying" and
    // "checking" for a user who is watching a folder that already has its data.
    m_walk = std::make_shared<WalkProgress>();
    d.state = kStateVerifying;
    const int row = rowOfDir(dirIndex);
    if (row >= 0)
        refreshRow(row);
    setJobStep(kJobVerify);
    m_verifyNext = kMvVerified;
    m_verifyWatcher->setFuture(
        QtConcurrent::run(verifyTrees, d.path, d.moveTarget, m_walk));
}

void AppDataMovePanel::onVerifyReady()
{
    // The same worker serves the move and both restore steps, so it has to hand
    // control back to the one that actually launched it.
    m_movePhase = m_verifyNext;
    runMoveStep();
}

// --------------------------------------------------------------------------- //
// Who is holding the folder open
// --------------------------------------------------------------------------- //
// A folder can be perfectly copied and still refuse to be replaced: the rename
// that frees the original path fails with ERROR_ACCESS_DENIED for as long as any
// process holds one of its files open, and no amount of elevation changes that —
// the handle belongs to somebody else. Learning WHO at that point is useful but
// late: the copy is already on the other drive and has to be thrown away if the
// user cannot close them. So the same question is asked BEFORE the copy, where
// the answer is free — nothing has been touched yet, so closing the programs or
// skipping the folder both cost the user exactly nothing.

void AppDataMovePanel::startLockProbe(const QString& dir)
{
    m_lockProcs.clear();
    m_lockWatcher->setFuture(QtConcurrent::run([dir]() {
        QVector<WinApi::LockingProcess> found;
        WinApi::processesLockingDir(dir, &found);
        return found;
    }));
}

void AppDataMovePanel::onLockProbeReady()
{
    m_lockProcs = m_lockWatcher->result();

    // The restore's own tail end uses the same query: it has the data home and
    // needs to know who is keeping the copy on the other drive alive.
    if (m_restoreActive && m_movePhase == kRsResidue) {
        settleResidue();
        return;
    }
    // ...as does its precheck, which is the same question asked before anything
    // has been touched at all.
    if (m_restoreActive && (m_movePhase == kRsProbe || m_movePhase == kRsHomeProbe)) {
        m_movePhase = (m_movePhase == kRsProbe) ? kRsProbed : kRsHomeProbed;
        runRestoreStep();
        return;
    }
    // ...and so does the leftover cleanup, which is the same question asked on
    // its own.
    if (m_cleanActive && m_movePhase == kClProbe) {
        m_movePhase = kClProbed;
        runMoveStep();
        return;
    }

    // The job may have been cancelled, or a restore may have taken over, while
    // the worker was still walking the tree.
    if (!m_moveActive || m_movePhase != kMvPrecheck
        || m_movePos >= static_cast<int>(m_moveQueue.size()))
        return;

    const int dirIndex = m_moveQueue[m_movePos];

    // Restart Manager reports every process with a handle on a file in the tree.
    // A handle that merely READS — an indexer walking the folder, a virus
    // scanner between two passes, Explorer painting a thumbnail — does not hold
    // the hand-over back, and those are exactly the processes this program is
    // never allowed to touch. Interrupting the user over nothing we can act on
    // would make the prompt meaningless, so when those are the ONLY holders the
    // copy starts and their names go to the log; if the hand-over then really
    // fails, the failure path names them properly. A holder that is not one of
    // those — a program actually running out of the folder, say — is worth
    // showing before a single byte moves, because that is the one moment where
    // closing it costs the user nothing.
    if (m_lockProcs.isEmpty() || OpGuard::onlyBackgroundHolders(m_lockProcs)) {
        if (!m_lockProcs.isEmpty()) {
            QStringList names;
            for (const WinApi::LockingProcess& p : m_lockProcs)
                names << p.name;
            Logger::info(QStringLiteral("[move] %1 is open in programs we never "
                                        "close, copying anyway: %2")
                             .arg(m_dirs[dirIndex].path, names.join(QStringLiteral(", "))));
        }
        m_movePhase = kMvCopy;
        runMoveStep();
        return;
    }

    switch (askAboutLockers(dirIndex, QStringLiteral("app_move.locked_intro"))) {
    case LockerProceed:
        m_movePhase = kMvCopy;
        runMoveStep();
        return;
    case LockerSkipFolder:
        // This folder stays exactly as it is; the ones behind it still get their
        // turn, each with its own check.
        ++m_movePos;
        m_movePhase = kMvPrecheck;
        runMoveStep();
        return;
    case LockerAbortBatch:
    default:
        // "Not now" — the folders that were not reached keep their tick, so the
        // button that starts a batch is also the button that resumes one.
        finishMove();
        return;
    }
}

// Which programs this panel actually got rid of, by image path — the thing that
// survives a restart, and therefore the only way to tell "a program is holding
// the folder" apart from "a program restarts itself".
void AppDataMovePanel::rememberClosedImages(DataDir& d,
                                            const QVector<WinApi::CloseOutcome>& outcomes)
{
    for (const WinApi::CloseOutcome& o : outcomes) {
        if (o.result == WinApi::CloseOutcome::Survived)
            continue;
        for (const WinApi::LockingProcess& p : m_lockProcs) {
            if (p.pid != o.pid)
                continue;
            const QString key = p.exePath.isEmpty() ? p.name : p.exePath;
            if (!key.isEmpty() && !d.closedImages.contains(key))
                d.closedImages << key;
            break;
        }
    }
}

AppDataMovePanel::LockerAnswer AppDataMovePanel::askAboutLockers(int dirIndex,
                                                                const QString& introKey)
{
    DataDir& d = m_dirs[dirIndex];

    const LockerDialog::Answer answer = LockerDialog::ask(
        this, I18n::tr(QStringLiteral("app_move.title")),
        I18n::tr(introKey, QMap<QString, QString>{{"name", d.name}}),
        m_lockProcs,
        I18n::tr(QStringLiteral("app_move.btn_close_continue")),
        I18n::tr(QStringLiteral("app_move.btn_skip_dir")),
        /*allowSkip=*/true);

    // Anything the dialog did not report — Escape, the title bar's close button —
    // arrives as Cancel, and cancelling never acts on a process.
    if (answer == LockerDialog::Cancel)
        return LockerAbortBatch;

    if (answer == LockerDialog::Skip) {
        // Left exactly as it is. The row carries the reason and its tick is
        // cleared, so the user finds the folder where they left it instead of a
        // silent nothing.
        failMove(dirIndex, QStringLiteral("app_move.fail_locked_skip"));
        d.state = kStateFailed;
        d.checked = false;
        const int row = rowOfDir(dirIndex);
        if (row >= 0) {
            if (auto* item = m_tree->topLevelItem(row)) {
                const QSignalBlocker blocker(m_tree);   // not a user tick
                item->setCheckState(0, Qt::Unchecked);
            }
            refreshRow(row);
        }
        ++m_moveSkipped;
        return LockerSkipFolder;
    }

    // Close them. Closing is bounded (see WinApi::closeProcesses) but it is not
    // instant, so the pointer says what the window is waiting for.
    d.closeAttempted = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QVector<WinApi::CloseOutcome> outcomes = WinApi::closeProcesses(m_lockProcs);
    QApplication::restoreOverrideCursor();

    // Remember what actually went away. A later round compares this against
    // whoever is holding the folder then: the same image means the program
    // restarted itself, which is a dead end rather than a reason to ask again.
    rememberClosedImages(d, outcomes);

    // The detail goes to the log and to the row on screen: which program shut
    // down politely, which had to be forced, and which was left alone is exactly
    // what the user needs if the move still does not go through.
    const QString report = LockerDialog::outcomeText(outcomes);
    if (!report.isEmpty())
        Logger::info(QStringLiteral("[move] programs holding %1:\n%2").arg(d.path, report));
    m_scanStatus->setText(I18n::tr(LockerDialog::anyClosed(outcomes)
                                       ? QStringLiteral("app_move.closed_retry")
                                       : QStringLiteral("app_move.closed_none")));
    m_progressRow->setVisible(true);
    return LockerProceed;
}

void AppDataMovePanel::runMoveStep()
{
    if (m_repairActive) {
        repairNext();
        return;
    }
    if (m_restoreActive) {
        runRestoreStep();
        return;
    }
    if (m_cleanActive) {
        runCleanStep();
        return;
    }

    if (m_movePos >= static_cast<int>(m_moveQueue.size())) {
        finishMove();
        return;
    }

    const int dirIndex = m_moveQueue[m_movePos];
    DataDir& d = m_dirs[dirIndex];
    const int row = rowOfDir(dirIndex);
    const QString dst = QDir::toNativeSeparators(d.moveTarget);

    switch (m_movePhase) {
    case kMvPrecheck: {
        // Peeking costs nothing and can save the whole copy. Whatever is holding
        // this folder open is found HERE — before a byte has moved — which is
        // the only moment where closing a program also costs the user nothing.
        // Leaving it until the copy fails means a half-copied tree has to be
        // discarded before anything can be retried.
        d.state = kStateChecking;
        if (row >= 0)
            refreshRow(row);
        // The previous folder finished its job steps a moment ago, and its
        // progress timer would otherwise keep rewriting the row underneath the
        // occupancy check — relabelling it "cleaning up" while the bar is busy.
        endJobProgress();
        m_jobFailed = false;
        m_progressHideTimer->stop();
        m_progressRow->setVisible(true);
        m_scanStatus->setText(I18n::tr(QStringLiteral("app_move.state_checking")));
        // A busy bar, not a percentage: the query has no idea how long it will
        // take, and a bar parked at 0% reads as "stuck".
        m_progress->setRange(0, 0);
        startLockProbe(d.path);
        return;
    }

    case kMvCopy: {
        // A queued folder may have been queued while an earlier one was still
        // copying, so it is marked here, the moment its turn comes.
        QDir().mkpath(dst);
        d.state = kStateCopying;
        if (row >= 0)
            refreshRow(row);
        // Recorded before a single byte is copied: if the machine dies now, the
        // journal says "an incomplete copy of this folder may exist at <target>"
        // and the original is provably still in place.
        d.journalPhase = kPhaseCopying;
        saveJournal();
        beginJobProgress(d);
        startProc(robocopyPath(), copyArgs(d.path, dst));
        return;
    }

    case kMvCopied: {
        startVerify();
        return;
    }

    case kMvVerified: {
        // robocopy exit codes 0x04 (mismatch), 0x08 (failed) and 0x10 (fatal)
        // all mean "not every file made it across".
        const bool copied = !m_procFailedToStart && (m_proc->exitCode() & 0x1C) == 0;
        const VerifyResult v = m_verifyWatcher->result();
        // Compare the two trees as they are right now, at the moment the
        // original is about to be replaced.
        const bool same = v.src == v.dst && v.src >= 0;
        // Zero bytes is a legitimate empty folder, but it is also what an
        // unreadable one reports — so an empty result has to be confirmed.
        const bool emptyOk = !(v.src == 0 && !holdsNothing(d.path));
        if (!copied || !same || !emptyOk) {
            // Read the helper's output once: logProcResult consumes it, and the
            // reason it carries is exactly what the row has to show.
            const QString diagReason = logProcResult("copy");
            // When this batch already closed the programs holding the folder and
            // the copy still failed, find out whether the same ones are back.
            // That changes the explanation completely: it is not "some program
            // is using it" (which closing fixes) but "this program starts itself
            // again" (which closing never will).
            QStringList respawned;
            if (d.closeAttempted) {
                QVector<WinApi::LockingProcess> now;
                WinApi::processesLockingDir(d.path, &now);
                if (!now.isEmpty())
                    m_lockProcs = now;
                respawned = WinApi::respawnedAmong(now, d.closedImages);
            }
            if (!respawned.isEmpty()) {
                Logger::info(QStringLiteral("[move] %1: %2 restarted after being "
                                            "closed; closing again cannot help")
                                 .arg(d.path, respawned.join(QStringLiteral(", "))));
                failMove(dirIndex, QStringLiteral("app_move.fail_respawn"),
                         QMap<QString, QString>{
                             {"name", d.name},
                             {"procs", respawned.join(QStringLiteral("、"))}});
            } else if (m_procFailedToStart) {
                failMove(dirIndex, QStringLiteral("app_move.fail_no_robocopy"));
            } else if (!copied) {
                failMove(dirIndex, QStringLiteral("app_move.fail_copy"),
                         QMap<QString, QString>{
                             {"code", QString::number(m_proc->exitCode())},
                             {"reason", diagReason}});
            } else if (!same) {
                // Half-copied is the one case where the numbers themselves are
                // the explanation.
                failMove(dirIndex, QStringLiteral("app_move.fail_verify"),
                         QMap<QString, QString>{{"src", humanSize(v.src)},
                                                {"dst", humanSize(v.dst)}});
            } else {
                failMove(dirIndex, QStringLiteral("app_move.fail_unreadable"));
            }
            d.state = kStateFailed;
            if (row >= 0)
                refreshRow(row);
            // The original is provably untouched at this point, so the partial
            // copy on the other drive is ours to discard.
            deleteTreeAsync({dst}, kMvBadCopyGone);
            return;
        }

        // The copy is complete and verified. Remember where it went before the
        // original location is touched at all, so the row can always offer
        // "restore" from here on.
        d.linkTarget = dst;
        d.knownMove = true;
        d.oldPath = oldPathOf(d.path);
        d.journalPhase = kPhaseCopied;
        saveJournal();
        // Verifying is done; the hand-over is all that is left, and it gets the
        // bar's own slice rather than a frozen 100%.
        setJobStep(kJobFinish);

        // Hand the original over by RENAMING it, which is an instant metadata
        // operation on the same volume. This is what makes the hand-over safe:
        // from here on there are two complete copies, and the original path is
        // either free or occupied by its own renamed original — never empty with
        // the data only on one disk.
        if (QDir(d.oldPath).exists() || QFile::exists(d.oldPath)) {
            // A leftover from an earlier attempt. Recovery owns those, so this
            // folder is left for it rather than deleting something unknown — but
            // the user still has to be told which name is in the way.
            failMove(dirIndex, QStringLiteral("app_move.fail_leftover"),
                     QMap<QString, QString>{{"name", QFileInfo(d.oldPath).fileName()}});
            d.state = kStateFailed;
            if (row >= 0)
                refreshRow(row);
            ++m_moveFailed;
            ++m_movePos;
            // Every folder starts with its own occupancy check, never with the
            // copy: what is holding THIS one open is not what held the last.
            m_movePhase = kMvPrecheck;
            runMoveStep();
            return;
        }
        quint32 werr = 0;
        if (!WinApi::renamePath(d.path, d.oldPath, &werr)) {
            QString reason = winErrorReason(werr);
            // A rename of a folder whose tree holds an open handle surfaces as
            // ERROR_ACCESS_DENIED (5) or a sharing violation (32/33) — even
            // fully elevated, because no privilege closes ANOTHER process's
            // handle. So: only when this instance somehow runs WITHOUT admin
            // is a relaunch worth offering; otherwise name the processes that
            // hold the folder (Restart Manager) so the user knows what to
            // close (Tencent and DJI background processes live here).
            if (werr == 5 || werr == 32 || werr == 33) {
                if (!WinApi::isAdmin()) {
                    if (Dialogs::confirm(this, I18n::tr("app_move.title"),
                                         I18n::tr("app_move.elevate_ask"))) {
                        if (WinApi::relaunchAsAdmin()) {
                            Logger::info(QStringLiteral("[move] relaunching elevated; "
                                                       "folder stays in journal for retry"));
                            QCoreApplication::quit();
                            return;
                        }
                    }
                } else {
                    // The pre-copy check should have caught this, so what is left
                    // is a program that took a handle in between — a client that
                    // restarted itself, or one whose lock the sampling cap hid.
                    // Ask the same question, once, and with the same escape
                    // hatch: the copy is already complete and verified, so the
                    // only thing standing between the user and a finished move
                    // is that program.
                    QVector<WinApi::LockingProcess> procs;
                    WinApi::processesLockingDir(d.path, &procs);
                    m_lockProcs = procs;

                    // Unless it is a program this batch already closed and which
                    // is here again: then it restarts itself — a helper process,
                    // a tray agent, a service — and another round of closing
                    // would end exactly where this round did. That is the one
                    // piece of information that tells the user the move has to
                    // be done differently, so it is worth saying instead of
                    // offering a second round that cannot work.
                    const QStringList respawned = WinApi::respawnedAmong(procs, d.closedImages);
                    if (!respawned.isEmpty()) {
                        Logger::info(QStringLiteral("[move] %1: %2 restarted after being "
                                                    "closed; closing again cannot help")
                                         .arg(d.path, respawned.join(QStringLiteral(", "))));
                        failMove(dirIndex, QStringLiteral("app_move.fail_respawn"),
                                 QMap<QString, QString>{
                                     {"name", d.name},
                                     {"procs", respawned.join(QStringLiteral("、"))}});
                        d.state = kStateFailed;
                        if (row >= 0)
                            refreshRow(row);
                        ++m_moveFailed;
                        ++m_movePos;
                        m_movePhase = kMvPrecheck;
                        runMoveStep();
                        return;
                    }

                    if (!procs.isEmpty()) {
                        m_lockProcs = procs;
                        // The prompt is shown whenever we know WHO, including
                        // when none of them may be closed. Declining to ask
                        // because there is nothing to offer took the one piece
                        // of information the user needs — the names — away with
                        // it, and left a bare "the folder is in use" behind. The
                        // dialog itself leaves the close button out when closing
                        // is not on the table, so showing it never promises
                        // anything we cannot do.
                        if (!d.closeAttempted) {
                            const LockerAnswer answer = askAboutLockers(
                                dirIndex, QStringLiteral("app_move.locked_fail_intro"));
                            if (answer == LockerAbortBatch) {
                                finishMove();
                                return;
                            }
                            if (answer == LockerSkipFolder) {
                                // askAboutLockers has already recorded the reason
                                // and cleared the tick; all that is left is to
                                // move on to the next folder.
                                ++m_movePos;
                                m_movePhase = kMvPrecheck;
                                runMoveStep();
                                return;
                            }
                            // Close and continue: re-copy from the start on
                            // purpose. A program that has just been closed may
                            // have written on its way out, so the copy verified
                            // a moment ago is no longer provably current —
                            // robocopy brings across only what differs, and the
                            // result is verified again before anything is moved.
                            d.state = kStateCopying;
                            if (row >= 0)
                                refreshRow(row);
                            m_movePhase = kMvCopy;
                            runMoveStep();
                            return;
                        }
                        // Asked already, or the user declined: name them, and say
                        // what to do when none of them is ours to close — which
                        // is the case where "close it yourself and retry" is not
                        // advice but a riddle.
                        QStringList names;
                        for (const WinApi::LockingProcess& p : procs)
                            names << p.name;
                        reason += QLatin1Char('\n')
                                  + I18n::tr("app_move.locked_by",
                                             QMap<QString, QString>{
                                                 {"procs", names.join(QStringLiteral("、"))}});
                        if (LockerDialog::closableCount(procs) == 0)
                            reason += QLatin1Char('\n')
                                      + I18n::tr(QStringLiteral("app_move.locked_noclose"));
                    } else {
                        // Nobody named. That is not proof that nobody is there:
                        // a folder handle held by a program whose working
                        // directory is here is invisible to both detectors. It is
                        // also usually transient — an indexer or a virus scanner
                        // between two passes — so the advice is worth more than
                        // the empty answer.
                        Logger::info(QStringLiteral("[move] %1: rename refused (%2) but no "
                                                    "program could be identified")
                                         .arg(d.path).arg(werr));
                        reason += QLatin1Char('\n')
                                  + I18n::tr(QStringLiteral("app_move.locked_unknown"));
                    }
                }
            }
            failMove(dirIndex, QStringLiteral("app_move.fail_rename"),
                     QMap<QString, QString>{{"reason", reason}});
            d.state = kStateFailed;
            if (row >= 0)
                refreshRow(row);
            ++m_moveFailed;
            ++m_movePos;
            // Every folder starts with its own occupancy check, never with the
            // copy: what is holding THIS one open is not what held the last.
            m_movePhase = kMvPrecheck;
            runMoveStep();
            return;
        }
        d.state = kStateLinking;
        if (row >= 0)
            refreshRow(row);
        m_movePhase = kMvRenamed;
        runMoveStep();
        return;
    }

    case kMvRenamed: {
        // The old location is free, so the junction can take its place. There is
        // nothing to measure in this step, which is why the bar is only nudged
        // into its next slice and the wording carries the change.
        setJobStep(kJobFinish);
        startProc(cmdPath(), mkLinkArgs(d.path, dst));
        m_movePhase = kMvLinked;
        return;
    }

    case kMvLinked: {
        if (m_procFailedToStart || m_proc->exitCode() != 0) {
            // Put the original back under its own name. Nothing was lost: the
            // complete copy stays on the other drive and the row keeps its
            // "restore" action.
            const QString linkReason = logProcResult("link");
            failMove(dirIndex, QStringLiteral("app_move.fail_link"),
                     QMap<QString, QString>{{"reason", linkReason}});
            WinApi::renamePath(d.oldPath, d.path);
            d.oldPath.clear();
            d.journalPhase = kPhaseNone;
            removeJournalEntry(d.path);
            d.state = kStateFailed;
            if (row >= 0)
                refreshRow(row);
            ++m_moveFailed;
            ++m_movePos;
            // Every folder starts with its own occupancy check, never with the
            // copy: what is holding THIS one open is not what held the last.
            m_movePhase = kMvPrecheck;
            runMoveStep();
            return;
        }
        d.journalPhase = kPhaseLinked;
        saveJournal();
        d.state = kStateDeleting;
        if (row >= 0)
            refreshRow(row);
        // Only now, with the junction already in place, does the stale original
        // go. The move is effectively finished; this is the cleanup — and on a
        // multi-gigabyte folder it is the longest step left, so it gets a slice
        // of the bar and a byte counter of its own instead of an idle spinner.
        m_freed = std::make_shared<std::atomic<qint64>>(0);
        m_jobDone = 0;
        setJobStep(kJobCleanup);
        deleteTreeAsync({d.oldPath}, kMvOldGone, m_freed);
        return;
    }

    case kMvBadCopyGone: {
        d.state = kStateFailed;
        d.journalPhase = kPhaseNone;
        removeJournalEntry(d.path);
        if (row >= 0)
            refreshRow(row);
        ++m_moveFailed;
        ++m_movePos;
        m_movePhase = kMvPrecheck;
        runMoveStep();
        return;
    }

    default: {  // kMvOldGone
        // The junction is in place and working, so the folder counts as
        // relocated whether or not the stale copy could be removed. A leftover
        // only costs disk space.
        //
        // The journal keeps this folder as "linked" on purpose: that record is
        // what tells a later scan that this app — and not some other tool — put
        // the junction there, which is what makes "restore" available for it.
        d.oldPath.clear();
        d.state = kStateDone;
        d.size = 0;  // the data lives on the other drive now
        // The job is done for this folder: drop its tick so the next batch
        // starts from what the user still wants moved, not from stale state.
        if (d.checked) {
            d.checked = false;
            if (row >= 0) {
                auto* item = m_tree->topLevelItem(row);
                if (item) {
                    const QSignalBlocker blocker(m_tree);  // not a user tick
                    item->setCheckState(0, Qt::Unchecked);
                }
            }
        }
        ++m_moveOk;
        // A finished folder must not stop short of 100%: the last file can go
        // between two ticks, so the bar is put at its end here rather than
        // waiting for a tick that will never come.
        markJobComplete();
        if (row >= 0)
            refreshRow(row);
        ++m_movePos;
        m_movePhase = kMvPrecheck;
        runMoveStep();
        return;
    }
    }
}

void AppDataMovePanel::finishMove()
{
    endJobProgress();
    m_moveActive = false;
    setBusy(false);
    m_movePhase = kMvCopy;
    m_moveQueue.clear();
    saveJournal();
    // Something actually landed on the other drive: mark that home with the
    // warning icon so a later cleanup spree does not delete live data.
    if (m_moveOk > 0 && !m_batchTargetRoot.isEmpty())
        WinApi::writeDontDeleteMarker(m_batchTargetRoot);
    // The progress row was owned by the move job; hand it back and let it fade.
    m_progressHideTimer->start(1200);
    QString text = I18n::tr("app_move.done", QMap<QString, QString>{
                               {"ok", QString::number(m_moveOk)},
                               {"failed", QString::number(m_moveFailed)}});
    // Folders the user chose to leave alone, because a program was using them.
    // They are neither successes nor failures, and saying so is what tells the
    // user the folders are still ticked and still waiting.
    if (m_moveSkipped > 0)
        text += QLatin1Char('\n')
                + I18n::tr("app_move.done_skipped",
                           QMap<QString, QString>{{"skipped", QString::number(m_moveSkipped)}});
    notify(I18n::tr("app_move.title"), text);
}

// A closed tab must not lose the outcome of a move it started, and must not have
// a modal box thrown over whatever the user is doing either. So: shown right away
// while the panel is on screen, kept meanwhile, delivered by showEvent().
void AppDataMovePanel::notify(const QString& titleKey, const QString& text)
{
    if (isVisible()) {
        Dialogs::info(this, titleKey, text);
        return;
    }
    m_pendingNoticeTitle = titleKey;
    m_pendingNotice = text;
}

void AppDataMovePanel::onPathClicked(QTreeWidgetItem* item, int column)
{
    if (!item || column != 1)
        return;
    const int row = m_tree->indexOfTopLevelItem(item);
    if (row < 0 || row >= static_cast<int>(m_order.size()))
        return;
    const QString path = m_dirs[m_order[row]].path;
    if (!QDir(path).exists())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// --------------------------------------------------------------------------- //
// Restore
// --------------------------------------------------------------------------- //
void AppDataMovePanel::onRestore(int row)
{
    if (m_busy || row < 0 || row >= static_cast<int>(m_order.size()))
        return;

    const int dirIndex = m_order[row];
    const DataDir& d = m_dirs[dirIndex];
    if (d.linkTarget.isEmpty() || !QDir(d.linkTarget).exists()) {
        Dialogs::warn(this, I18n::tr("app_move.title"),
            I18n::tr("app_move.restore_missing",
                     QMap<QString, QString>{{"path", d.linkTarget}}));
        return;
    }

    if (!Dialogs::confirm(this, I18n::tr("app_move.title"),
                          I18n::tr(d.state == kStateFailed && d.knownMove
                                       ? QStringLiteral("app_move.abandon_confirm")
                                       : QStringLiteral("app_move.restore_confirm"),
                                   QMap<QString, QString>{{"name", d.name}}))) {
        return;
    }

    m_restoreRow = dirIndex;
    m_restoreFailed = false;
    m_restoreAskRound = 0;
    m_restoreActive = true;
    m_movePhase = kRsPrepare;
    setBusy(true);
    runMoveStep();
}

// The way is clear, so the hand-over starts: a junction is unlinked, a real
// folder sitting at the original path is measured against the copy, and an empty
// path is simply copied into. Split out of the precheck's own step because that
// step now goes on a detour first — the folder has to be known to be free before
// any of this may run.
void AppDataMovePanel::startRestoreHandover(int dirIndex)
{
    DataDir& d = m_dirs[dirIndex];
    const int row = rowOfDir(dirIndex);

    d.state = kStateRestoring;
    if (row >= 0)
        refreshRow(row);

    if (isReparsePoint(d.path)) {
        // "rmdir" without /S removes the junction itself, never its target.
        startProc(cmdPath(), unlinkArgs(d.path));
        m_movePhase = kRsUnlinkDone;
        return;
    }
    if (QDir(d.path).exists()) {
        // Something real sits at the original path. After a move that failed
        // at the hand-over this is the folder itself, back under its own
        // name — and then the copy on the other drive is the redundant one.
        // Telling those two apart means measuring both, which is asked of
        // the same worker the rest of the job already uses.
        m_verifyNext = kRsAssess;
        m_verifyWatcher->setFuture(QtConcurrent::run(
            verifyTrees, d.linkTarget, d.path,
            std::shared_ptr<AppDataMovePanel::WalkProgress>()));
        m_movePhase = kRsAssess;
        return;
    }
    m_movePhase = kRsCleared;  // nothing in the way, copy straight back
    runRestoreStep();
}

// Mirror image of the move: whatever occupies the old location goes away first,
// the data is copied back and verified, and only then is the copy on the other
// drive removed.
void AppDataMovePanel::runRestoreStep()
{
    const int dirIndex = m_restoreRow;
    if (dirIndex < 0 || dirIndex >= static_cast<int>(m_dirs.size())) {
        finishRestore();
        return;
    }
    DataDir& d = m_dirs[dirIndex];
    const int row = rowOfDir(dirIndex);
    // Every step of a restore is traced: this is the job that ends up touching
    // two drives at once, and when it misbehaves the log is the only account of
    // which step it got to.
    Logger::info(QStringLiteral("[restore] %1 phase=%2 state=%3 row=%4 target=%5")
                     .arg(d.path)
                     .arg(m_movePhase)
                     .arg(d.state)
                     .arg(row)
                     .arg(d.linkTarget));

    switch (m_movePhase) {
    case kRsPrepare: {
        // The source of truth has to exist before anything is touched. This
        // check used to run before *every* step, which meant it also ran after
        // the last step had legitimately deleted that copy — so a restore that
        // had fully succeeded still ended in a "restore failed" box. It belongs
        // here, at the one point where the job starts.
        if (!QDir(d.linkTarget).exists()) {
            failMove(dirIndex, QStringLiteral("app_move.fail_restore_no_source"),
                     QMap<QString, QString>{{"path", d.linkTarget}});
            m_restoreFailed = true;
            finishRestore();
            return;
        }

        // Before the junction goes, and before anything else is touched: who is
        // holding this folder? A restore cannot be half-done — it takes the link
        // away and rewrites the original path — and doing that while a program
        // still holds files in there is how the folder ends up emptied under the
        // running program, with a copy back that can never be verified and a
        // directory Explorer refuses to delete. So the question is asked here,
        // where the answer still costs the user nothing.
        d.state = kStateChecking;
        if (row >= 0)
            refreshRow(row);
        endJobProgress();
        m_jobFailed = false;
        m_progressHideTimer->stop();
        m_progressRow->setVisible(true);
        m_scanStatus->setText(I18n::tr(QStringLiteral("app_move.state_checking")));
        // A busy bar: the query has no idea how long it will take, and a bar
        // parked at 0% reads as "stuck".
        m_progress->setRange(0, 0);
        m_movePhase = kRsProbe;
        startLockProbe(d.linkTarget);
        return;
    }

    case kRsProbed: {
        // The answer decides everything: go, offer to close and ask again, or
        // refuse. Nothing has been touched at this point, which is what makes a
        // refusal cheap.
        const bool blocked = !m_lockProcs.isEmpty() && !OpGuard::onlyBackgroundHolders(m_lockProcs);
        switch (MoveSelect::restoreGate(blocked, LockerDialog::closableCount(m_lockProcs),
                                        m_restoreAskRound > 0)) {
        case MoveSelect::kRestoreGo:
            if (!m_lockProcs.isEmpty()) {
                // Only programs this app never closes, i.e. read handles. Worth a
                // line in the log, not worth a prompt.
                Logger::info(QStringLiteral("[restore] %1 is open in programs we never "
                                            "close, restoring anyway: %2")
                                 .arg(d.path, OpGuard::namesOf(m_lockProcs).join(QStringLiteral(", "))));
            }
            startRestoreHandover(dirIndex);
            return;

        case MoveSelect::kRestoreAsk: {
            const LockerDialog::Answer answer = LockerDialog::ask(
                this, I18n::tr(QStringLiteral("app_move.title")),
                I18n::tr(QStringLiteral("app_move.restore_locked_intro"),
                         QMap<QString, QString>{{"name", d.name}}),
                m_lockProcs,
                I18n::tr(QStringLiteral("app_move.btn_close_restore")),
                QString(), /*allowSkip=*/false);
            if (answer != LockerDialog::CloseAndContinue) {
                // Cancelled, so nothing was touched: the junction and the data are
                // exactly where they were. Say why the restore did not happen
                // rather than leaving the row looking as if it had never been
                // asked for.
                failMove(dirIndex, QStringLiteral("app_move.fail_restore_locked"),
                         QMap<QString, QString>{
                             {"name", d.name},
                             {"procs", OpGuard::namesOf(m_lockProcs).join(QStringLiteral("、"))}});
                d.state = kStateMoved;
                if (row >= 0)
                    refreshRow(row);
                m_restoreFailed = true;
                finishRestore();
                return;
            }
            d.closeAttempted = true;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const QVector<WinApi::CloseOutcome> outcomes = WinApi::closeProcesses(m_lockProcs);
            QApplication::restoreOverrideCursor();
            rememberClosedImages(d, outcomes);

            const QString report = LockerDialog::outcomeText(outcomes);
            if (!report.isEmpty())
                Logger::info(QStringLiteral("[restore] programs holding %1:\n%2")
                                 .arg(d.linkTarget, report));
            m_scanStatus->setText(I18n::tr(LockerDialog::anyClosed(outcomes)
                                               ? QStringLiteral("app_move.closed_retry_back")
                                               : QStringLiteral("app_move.closed_none")));
            m_progressRow->setVisible(true);
            // Ask again — closing a program is a request, not a guarantee, and
            // the whole point of asking before anything is touched is that the
            // answer is still allowed to be "no".
            m_restoreAskRound = 1;
            m_movePhase = kRsProbe;
            startLockProbe(d.linkTarget);
            return;
        }

        case MoveSelect::kRestoreRefuse:
        default: {
            // Held, and closing is either not ours to do or has already been
            // tried. The data stays on the other drive, the junction stays, and
            // the reason is named — a half-finished restore would be worse than
            // no restore at all.
            Logger::info(QStringLiteral("[restore] %1 refused: %2 holding %3")
                             .arg(d.path, OpGuard::namesOf(m_lockProcs).join(QStringLiteral(", ")),
                                  d.linkTarget));
            failRestoreLocked(dirIndex, m_lockProcs);
            d.state = kStateMoved;
            if (row >= 0)
                refreshRow(row);
            m_restoreFailed = true;
            finishRestore();
            return;
        }
        }
    }

    case kRsAssess: {
        const VerifyResult v = m_verifyWatcher->result();
        Logger::info(QStringLiteral("[restore] assess target=%1 home=%2 -> %3 / %4")
                         .arg(d.linkTarget, d.path,
                              QString::number(v.src), QString::number(v.dst)));
        if (v.src >= 0 && v.src == v.dst) {
            // Both trees hold the same thing, so the folder at its original path
            // is the real one and the copy on the other drive is dropped. Not a
            // byte is copied, which is what makes restoring a failed move
            // instant — and immune to the very lock that made the move fail.
            d.state = kStateDeleting;
            if (row >= 0)
                refreshRow(row);
            m_freed = std::make_shared<std::atomic<qint64>>(0);
            m_jobDone = 0;
            setJobStep(kJobCleanup);
            deleteTreeAsync({d.linkTarget}, kRsCopyDropped, m_freed);
            return;
        }
        // The folder at home is only a partial copy, so it goes and the verified
        // copy on the other drive is written back in its place. Whether it really
        // went is checked: the copy that follows would otherwise be made into a
        // tree that still holds open files, where the two totals can never be
        // made to agree — and the folder would be left half-deleted with the
        // data on the other drive.
        deleteTreeAsync({d.path}, kRsClearHome);
        return;
    }

    case kRsClearHome: {
        if (m_deleteOk || !QDir(d.path).exists()) {
            m_movePhase = kRsCleared;
            runRestoreStep();
            return;
        }
        // Still there. Find out who is holding it, so the report can name them
        // rather than repeat "could not restore" — the user is the only one who
        // can close them. The precheck asked the same question a moment ago, so
        // this is one probe deep, not an offer: whatever it finds, the answer
        // here is the same.
        Logger::info(QStringLiteral("[restore] %1 could not be emptied; asking who "
                                    "is holding it").arg(d.path));
        d.state = kStateChecking;
        if (row >= 0)
            refreshRow(row);
        m_movePhase = kRsHomeProbe;
        startLockProbe(d.path);
        return;
    }

    case kRsHomeProbed: {
        Logger::info(QStringLiteral("[restore] %1 refused: original path still held by %2")
                         .arg(d.path, OpGuard::namesOf(m_lockProcs).join(QStringLiteral(", "))));
        failRestoreLocked(dirIndex, m_lockProcs);
        d.state = kStateFailed;
        if (row >= 0)
            refreshRow(row);
        m_restoreFailed = true;
        finishRestore();
        return;
    }

    case kRsUnlinkDone: {
        if (m_procFailedToStart || m_proc->exitCode() != 0 || QDir(d.path).exists()) {
            // The junction is still in place, so nothing was touched and the
            // folder simply stays where it is. Report it as such instead of
            // leaving the user with an unexplained "restore failed".
            const QString reason =
                (!m_procFailedToStart && QDir(d.path).exists())
                    ? I18n::tr(QStringLiteral("app_move.restore_link_stuck"))
                    : logProcResult("restore-unlink");
            failMove(dirIndex, QStringLiteral("app_move.fail_restore_unlink"),
                     QMap<QString, QString>{{"reason", reason}});
            d.state = kStateMoved;
            m_restoreFailed = true;
            if (row >= 0)
                refreshRow(row);
            finishRestore();
            return;
        }
        // fall through: the way is clear, copy the data back
    }
    [[fallthrough]];
    case kRsCleared: {
        d.state = kStateCopying;
        if (row >= 0)
            refreshRow(row);
        QDir().mkpath(d.path);
        startProc(robocopyPath(), copyArgs(d.linkTarget, d.path));
        m_movePhase = kRsCopied;
        return;
    }

    case kRsCopied: {
        if (!m_procFailedToStart && (m_proc->exitCode() & 0x1C) == 0) {
            // Same reasoning as the move: measure both trees off the UI thread.
            m_verifyNext = kRsVerify;
            m_verifyWatcher->setFuture(QtConcurrent::run(
                verifyTrees, d.linkTarget, d.path,
                std::shared_ptr<AppDataMovePanel::WalkProgress>()));
            m_movePhase = kRsVerify;
            return;
        }
        const int code = m_procFailedToStart ? -1 : m_proc->exitCode();
        const QString reason = logProcResult("restore-copy");
        failMove(dirIndex, QStringLiteral("app_move.fail_restore_copy"),
                 QMap<QString, QString>{{"code", QString::number(code)},
                                        {"reason", reason}});
        m_restoreFailed = true;
        deleteTreeAsync({d.path}, kRsPartialGone);
        return;
    }

    case kRsVerify: {
        const VerifyResult v = m_verifyWatcher->result();
        if (v.src != v.dst || v.src < 0 || (v.src == 0 && !holdsNothing(d.linkTarget))) {
            // Discard the partial copy and put the junction back, so the
            // application keeps working; the data on the other drive is
            // untouched.
            failMove(dirIndex, QStringLiteral("app_move.fail_restore_verify"),
                     QMap<QString, QString>{{"src", humanSize(v.src)},
                                            {"dst", humanSize(v.dst)}});
            m_restoreFailed = true;
            deleteTreeAsync({d.path}, kRsPartialGone);
            return;
        }
        d.state = kStateDeleting;
        if (row >= 0)
            refreshRow(row);
        // Only now, with the data verified back on this drive, may the copy on
        // the other drive go.
        m_freed = std::make_shared<std::atomic<qint64>>(0);
        m_jobDone = 0;
        setJobStep(kJobCleanup);
        deleteTreeAsync({d.linkTarget}, kRsCopyDropped, m_freed);
        return;
    }

    case kRsPartialGone: {
        startProc(cmdPath(), mkLinkArgs(d.path, d.linkTarget));
        m_movePhase = kRsLinkedBack;
        return;
    }

    case kRsLinkedBack: {
        // mklink is a command, not a promise: it cannot create a link where a
        // folder of that name already exists, which is exactly what happens when
        // the partial copy could not be deleted because its files were open.
        // Reporting that as "moved" is how a user ends up with a folder that is
        // neither the data nor a link to it — the app reads an empty data folder,
        // and the next attempt answers "a folder with that name already exists".
        if (!isReparsePoint(d.path)) {
            Logger::info(QStringLiteral("[restore] %1: the link could not be put back")
                             .arg(d.path));
            failMove(dirIndex, QStringLiteral("app_move.fail_restore_nolink"),
                     QMap<QString, QString>{{"name", d.name},
                                            {"path", d.path},
                                            {"target", d.linkTarget}});
            d.state = kStateFailed;
            if (row >= 0)
                refreshRow(row);
            m_restoreFailed = true;
            finishRestore();
            return;
        }
        d.state = kStateMoved;
        if (row >= 0)
            refreshRow(row);
        finishRestore();
        return;
    }

    default: {  // kRsCopyDropped: the copy is back on this drive and verified
        // Everything that mattered has happened: the data is home and measured
        // against the copy it came from. What is left is dropping the copy on
        // the other drive — and a partial failure there must not be rounded up
        // to success. The leftover is a folder this app created, at a name this
        // app will want to use again, and inside it sit the very files a program
        // had open. Calling that "restored, all good" is exactly what left a
        // user with a folder Explorer refused to delete and a second move that
        // answered "a folder with that name already exists".
        if (!m_deleteOk && QDir(d.linkTarget).exists()) {
            Logger::info(QStringLiteral("[restore] %1: leftover at %2 could not be "
                                        "removed; asking who is holding it")
                             .arg(d.path, d.linkTarget));
            d.state = kStateCleaning;
            if (row >= 0)
                refreshRow(row);
            endJobProgress();
            m_progressHideTimer->stop();
            m_progressRow->setVisible(true);
            m_progress->setRange(0, 0);
            m_scanStatus->setText(I18n::tr(QStringLiteral("app_move.state_cleaning")));
            m_movePhase = kRsResidue;
            startLockProbe(d.linkTarget);
            return;
        }

        d.linkTarget.clear();
        d.oldPath.clear();
        d.moveTarget.clear();
        d.journalPhase = kPhaseNone;
        // The folder lives at its original path again, so it is no longer a
        // relocation the journal has to keep track of.
        removeJournalEntry(d.path);
        d.state = kStateIdle;
        d.size = -1;          // it lives here again, so measure it again
        markJobComplete();
        if (row >= 0)
            refreshRow(row);
        Logger::info(QStringLiteral("[restore] %1 done, re-measuring").arg(d.path));
        startNextSizeJob();
        finishRestore();
        Logger::info(QStringLiteral("[restore] %1 returned from finishRestore").arg(d.path));
        return;
    }
    }
}

// Why a restore stopped short of touching anything. The wording depends on what
// is known: a named program tells the user exactly what to close, and "a program
// restarted itself after being closed" tells them closing it again is useless,
// which is the difference between two very different next steps.
void AppDataMovePanel::failRestoreLocked(int dirIndex,
                                         const QVector<WinApi::LockingProcess>& procs)
{
    const DataDir& d = m_dirs[dirIndex];
    const QStringList respawned = WinApi::respawnedAmong(procs, d.closedImages);
    const QStringList names = respawned.isEmpty() ? OpGuard::namesOf(procs) : respawned;
    if (names.isEmpty()) {
        failMove(dirIndex, QStringLiteral("app_move.fail_restore_locked_unknown"),
                 QMap<QString, QString>{{"name", d.name}});
        return;
    }
    failMove(dirIndex,
             !respawned.isEmpty() ? QStringLiteral("app_move.fail_restore_respawn")
                                  : QStringLiteral("app_move.fail_restore_locked"),
             QMap<QString, QString>{{"name", d.name},
                                    {"procs", names.join(QStringLiteral("、"))}});
}

// The data is home and verified; the copy on the other drive is still there
// because something inside it is open. Same problem as a blocked move, so it
// gets the same treatment: name the holders, offer once to close the closable
// ones, try the delete again. The difference is what happens when that does not
// work — the folder is written down as this app's own leftover instead of being
// reported as success, which is what makes it cleanable later.
void AppDataMovePanel::settleResidue()
{
    const int dirIndex = m_restoreRow;
    if (dirIndex < 0 || dirIndex >= static_cast<int>(m_dirs.size())) {
        finishRestore();
        return;
    }
    DataDir& d = m_dirs[dirIndex];
    const int row = rowOfDir(dirIndex);
    const QString target = d.linkTarget;

    Logger::info(QStringLiteral("[restore] %1: %2 programs still hold %3")
                     .arg(d.path)
                     .arg(m_lockProcs.size())
                     .arg(target));

    if (!m_lockProcs.isEmpty() && !d.closeAttempted
        && LockerDialog::closableCount(m_lockProcs) > 0) {
        const LockerDialog::Answer answer = LockerDialog::ask(
            this, I18n::tr(QStringLiteral("app_move.title")),
            I18n::tr(QStringLiteral("app_move.residue_intro"),
                     QMap<QString, QString>{{"name", d.name}, {"path", target}}),
            m_lockProcs,
            I18n::tr(QStringLiteral("app_move.btn_close_clean")),
            QString(), /*allowSkip=*/false);

        if (answer == LockerDialog::CloseAndContinue) {
            d.closeAttempted = true;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const QVector<WinApi::CloseOutcome> outcomes = WinApi::closeProcesses(m_lockProcs);
            QApplication::restoreOverrideCursor();

            const QString report = LockerDialog::outcomeText(outcomes);
            if (!report.isEmpty())
                Logger::info(QStringLiteral("[restore] programs holding %1:\n%2")
                                 .arg(target, report));

            if (LockerDialog::anyClosed(outcomes)) {
                // Run the delete again. A program that has just been closed may
                // have written on its way out, so nothing about the folder is
                // assumed to be current — the same call simply tries again, and
                // re-enters this branch if anything is still in the way.
                d.state = kStateCleaning;
                if (row >= 0)
                    refreshRow(row);
                m_scanStatus->setText(I18n::tr(QStringLiteral("app_move.closed_retry")));
                m_progress->setRange(0, 0);
                deleteTreeAsync({target}, kRsCopyDropped);
                return;
            }
        }
    }

    // Still there. Say so, and keep it as ours.
    d.residueTarget = target;
    d.linkTarget.clear();
    d.oldPath.clear();
    d.moveTarget.clear();
    d.journalPhase = kPhaseResidue;
    d.state = kStateIdle;
    d.size = -1;  // it lives here again, so measure it again
    saveJournal();
    m_restoreResidue = target;
    if (row >= 0)
        refreshRow(row);
    markJobComplete();
    Logger::info(QStringLiteral("[restore] %1: leftover kept at %2 (recorded as residue)")
                     .arg(d.path, target));
    startNextSizeJob();
    finishRestore();
}

// --------------------------------------------------------------------------- //
// Leftover cleanup — deleting a copy this app left on another drive
// --------------------------------------------------------------------------- //

// Drops the copy a restore (or an interrupted move) could not delete. It is
// offered right on the row that records it, because the alternative the user is
// left with — finding the folder in Explorer and being told a program is using
// it — is exactly the dead end this exists to remove.
void AppDataMovePanel::onCleanResidue(int row)
{
    if (m_busy || row < 0 || row >= m_tree->topLevelItemCount())
        return;
    const int dirIndex = m_order[row];
    if (dirIndex < 0 || dirIndex >= static_cast<int>(m_dirs.size()))
        return;
    DataDir& d = m_dirs[dirIndex];

    if (d.residueTarget.isEmpty() || !QDir(d.residueTarget).exists()) {
        // Already gone: deleted by hand, or by a program that finally let go.
        // Forgetting the record is the whole of the work.
        d.residueTarget.clear();
        d.journalPhase = kPhaseNone;
        removeJournalEntry(d.path);
        refreshRow(row);
        notify(I18n::tr("app_move.title"),
               I18n::tr(QStringLiteral("app_move.clean_residue_gone")));
        return;
    }

    if (!Dialogs::confirm(this, I18n::tr("app_move.title"),
                          I18n::tr(QStringLiteral("app_move.clean_residue_confirm"),
                                   QMap<QString, QString>{{"name", d.name},
                                                          {"path", d.residueTarget}}))) {
        return;
    }
    startResidueCleanup(dirIndex);
}

// The cleanup itself, without a prompt of its own: every caller has already
// asked its own question, and asking twice for the same delete would make the
// panel look like it is not listening.
void AppDataMovePanel::startResidueCleanup(int dirIndex)
{
    if (dirIndex < 0 || dirIndex >= static_cast<int>(m_dirs.size()))
        return;
    DataDir& d = m_dirs[dirIndex];
    if (d.residueTarget.isEmpty())
        return;

    d.closeAttempted = false;
    d.failKey.clear();
    d.failArgs.clear();
    // Remembered so the row can be handed back afterwards: the cleanup shows
    // "dropping a leftover" while it runs, and a job that finishes without
    // putting the row back is a row that looks busy for ever.
    m_cleanPrevState = d.state;
    m_cleanRow = dirIndex;
    m_cleanActive = true;
    m_cleanFailKey.clear();
    m_cleanFailArgs.clear();
    m_jobFailed = false;
    m_movePhase = kClProbe;
    setBusy(true);
    runMoveStep();
}

void AppDataMovePanel::runCleanStep()
{
    const int dirIndex = m_cleanRow;
    if (dirIndex < 0 || dirIndex >= static_cast<int>(m_dirs.size())) {
        finishClean();
        return;
    }
    DataDir& d = m_dirs[dirIndex];
    const int row = rowOfDir(dirIndex);

    switch (m_movePhase) {
    case kClProbe: {
        d.state = kStateCleaning;
        if (row >= 0)
            refreshRow(row);
        endJobProgress();
        m_progressHideTimer->stop();
        m_progressRow->setVisible(true);
        // A busy bar: finding out who holds a folder takes as long as the walk
        // takes, and a bar parked at 0% reads as "stuck".
        m_progress->setRange(0, 0);
        m_scanStatus->setText(I18n::tr(QStringLiteral("app_move.state_cleaning")));
        startLockProbe(d.residueTarget);
        return;
    }

    case kClProbed: {
        // The same question as a blocked move, asked about the leftover: who is
        // holding it, and would the user like us to close them. Asked only when
        // there is a program we are allowed to close — an offer we cannot keep
        // is worse than no offer.
        if (!m_lockProcs.isEmpty() && !d.closeAttempted
            && LockerDialog::closableCount(m_lockProcs) > 0) {
            const LockerDialog::Answer answer = LockerDialog::ask(
                this, I18n::tr(QStringLiteral("app_move.title")),
                I18n::tr(QStringLiteral("app_move.residue_intro"),
                         QMap<QString, QString>{{"name", d.name},
                                               {"path", d.residueTarget}}),
                m_lockProcs,
                I18n::tr(QStringLiteral("app_move.btn_close_clean")),
                QString(), /*allowSkip=*/false);

            if (answer == LockerDialog::CloseAndContinue) {
                d.closeAttempted = true;
                QApplication::setOverrideCursor(Qt::WaitCursor);
                const QVector<WinApi::CloseOutcome> outcomes =
                    WinApi::closeProcesses(m_lockProcs);
                QApplication::restoreOverrideCursor();

                const QString report = LockerDialog::outcomeText(outcomes);
                if (!report.isEmpty())
                    Logger::info(QStringLiteral("[clean] programs holding %1:\n%2")
                                     .arg(d.residueTarget, report));
                m_scanStatus->setText(I18n::tr(QStringLiteral("app_move.closed_retry")));
            } else {
                // Cancelled, so nothing was touched. Saying so beats reporting a
                // cleanup that never ran.
                m_cleanFailKey = QStringLiteral("app_move.clean_residue_cancelled");
                m_cleanFailArgs = QMap<QString, QString>{{"path", d.residueTarget}};
                finishClean();
                return;
            }
        }
        m_progress->setRange(0, 0);
        deleteTreeAsync({d.residueTarget}, kClDelete);
        return;
    }

    default: {  // kClDelete — whatever is left is the answer
        if (m_deleteOk || !QDir(d.residueTarget).exists()) {
            const QString gone = d.residueTarget;
            d.residueTarget.clear();
            d.journalPhase = kPhaseNone;
            // Not re-measured: what goes away is a copy on ANOTHER drive, and
            // this folder's own figure has nothing to do with it. Blanking it
            // would put the row back on "measuring" with nothing measuring it —
            // which is exactly what it used to do.
            removeJournalEntry(d.path);
            Logger::info(QStringLiteral("[clean] %1: leftover %2 removed").arg(d.path, gone));
            if (row >= 0)
                refreshRow(row);
        } else {
            // Something inside is still open. The folder stays recorded as ours,
            // so the button is still there and the next attempt starts from the
            // same place instead of from a dead end.
            QStringList names;
            for (const WinApi::LockingProcess& p : m_lockProcs)
                names << p.name;
            m_cleanFailKey = names.isEmpty()
                                 ? QStringLiteral("app_move.clean_residue_failed_unknown")
                                 : QStringLiteral("app_move.clean_residue_failed");
            m_cleanFailArgs = QMap<QString, QString>{{"path", d.residueTarget}};
            if (!names.isEmpty())
                m_cleanFailArgs.insert(QStringLiteral("procs"),
                                       names.join(QStringLiteral("、")));
            Logger::warn(QStringLiteral("[clean] %1: leftover %2 still held")
                             .arg(d.path, d.residueTarget));
        }
        finishClean();
        return;
    }
    }
}

void AppDataMovePanel::finishClean()
{
    const int dirIndex = m_cleanRow;
    m_cleanRow = -1;
    m_cleanActive = false;
    m_movePhase = kClProbe;
    setBusy(false);
    saveJournal();

    const bool ok = m_cleanFailKey.isEmpty();
    if (dirIndex >= 0 && dirIndex < static_cast<int>(m_dirs.size())) {
        DataDir& d = m_dirs[dirIndex];
        // Hand the state back (MoveSelect::stateAfterCleanup): leaving the row on
        // "dropping a leftover" is what made a finished cleanup — and a failed
        // one — look like a job that never ended.
        d.state = stateAfterCleanup(m_cleanPrevState, d.state);
        if (ok) {
            d.failKey.clear();
            d.failArgs.clear();
        } else {
            d.failKey = m_cleanFailKey;
            d.failArgs = m_cleanFailArgs;
            m_scanStatus->setText(I18n::tr(d.failKey, d.failArgs));
            m_progressRow->setVisible(true);
        }
        // A folder that was never measured (it came out of the journal that
        // way) goes back into the measuring queue, so no row is left reading
        // "measuring" with nothing measuring it.
        if (d.size < 0)
            startNextSizeJob();
        const int row = rowOfDir(dirIndex);
        if (row >= 0)
            refreshRow(row);
        refreshSummary();
        notify(I18n::tr("app_move.title"),
               I18n::tr(ok ? QStringLiteral("app_move.clean_residue_done")
                           : m_cleanFailKey,
                        ok ? QMap<QString, QString>{{"name", d.name}} : m_cleanFailArgs));
    }
    m_cleanFailKey.clear();
    m_cleanFailArgs.clear();
    // A cleanup that failed keeps the row: the reason is written on it, and
    // hiding it would take the explanation away.
    m_jobFailed = !ok;
    if (ok)
        m_progressHideTimer->start(900);
}

void AppDataMovePanel::finishRestore()
{
    const int dirIndex = m_restoreRow;
    m_restoreRow = -1;
    m_restoreActive = false;
    m_movePhase = kRsPrepare;
    const QString name = (dirIndex >= 0 && dirIndex < static_cast<int>(m_dirs.size()))
                             ? m_dirs[dirIndex].name
                             : QString();
    const bool failed = m_restoreFailed;
    m_restoreFailed = false;
    // A leftover does not make the restore a failure — the data really is back —
    // so it gets its own closing message rather than being folded into either.
    const QString residue = m_restoreResidue;
    m_restoreResidue.clear();
    setBusy(false);
    saveJournal();
    Logger::info(QStringLiteral("[restore] finish \"%1\" failed=%2 residue=%3")
                     .arg(name).arg(failed).arg(residue));
    // The restore shares the progress row with the scan; make sure it is handed
    // back even though the restore itself does not draw progress on it. A failed
    // restore keeps it, because the reason is written there. A leftover does not:
    // that story is told by the dialog that just closed, and leaving a spinner
    // labelled "removing leftovers" behind would be a lie by the time it is read.
    if (!m_moveActive && !failed)
        m_progressHideTimer->start(600);
    const QString body =
        failed ? I18n::tr(QStringLiteral("app_move.restore_failed"),
                          QMap<QString, QString>{{"name", name}})
               : (residue.isEmpty()
                      ? I18n::tr(QStringLiteral("app_move.restore_done"),
                                 QMap<QString, QString>{{"name", name}})
                      : I18n::tr(QStringLiteral("app_move.restore_done_residue"),
                                 QMap<QString, QString>{{"name", name},
                                                        {"path", residue}}));
    Dialogs::info(this, I18n::tr("app_move.title"), body);
}

// --------------------------------------------------------------------------- //
// Journal — where every relocated folder went
// --------------------------------------------------------------------------- //
namespace {

QString phaseName(int phase)
{
    switch (phase) {
    case kPhaseCopying: return QStringLiteral("copying");
    case kPhaseCopied: return QStringLiteral("copied");
    case kPhaseLinked: return QStringLiteral("linked");
    case kPhaseResidue: return QStringLiteral("residue");
    default: return QStringLiteral("none");
    }
}

int phaseFromName(const QString& name)
{
    if (name == QLatin1String("copying"))
        return kPhaseCopying;
    if (name == QLatin1String("copied"))
        return kPhaseCopied;
    if (name == QLatin1String("linked"))
        return kPhaseLinked;
    if (name == QLatin1String("residue"))
        return kPhaseResidue;
    return kPhaseNone;
}

// Read the journal as a path-keyed map. Reading it before every write is what
// makes the file a durable record: scanning a different folder must not drop the
// entries of relocations that are not in the current list.
QMap<QString, QJsonObject> readJournalByPath()
{
    QMap<QString, QJsonObject> out;
    const QString file = journalPath();
    if (file.isEmpty())
        return out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray())
        return out;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString path = o.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            out[pathKey(path)] = o;
    }
    return out;
}

// Written through a temporary file (QSaveFile commits with a rename), so losing
// power halfway through a write cannot leave a truncated journal behind.
void writeJournalMap(const QMap<QString, QJsonObject>& entries)
{
    const QString file = journalPath();
    if (file.isEmpty())
        return;
    QJsonArray arr;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        arr.append(it.value());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    f.commit();
}

} // namespace

void AppDataMovePanel::saveJournal()
{
    QMap<QString, QJsonObject> entries = readJournalByPath();
    for (const DataDir& d : m_dirs) {
        if (d.journalPhase == kPhaseNone)
            continue;

        // A residue entry records the opposite of a move: the data is home and
        // the only path that matters is the leftover waiting to be deleted.
        // Writing it means the app can still recognise its own mess after a
        // restart, which is what keeps it from being mistaken for the user's.
        if (d.journalPhase == kPhaseResidue) {
            if (d.residueTarget.isEmpty())
                continue;
            QJsonObject o;
            o[QStringLiteral("path")] = d.path;
            o[QStringLiteral("name")] = d.name;
            o[QStringLiteral("target")] = d.residueTarget;
            o[QStringLiteral("residue")] = d.residueTarget;
            o[QStringLiteral("phase")] = phaseName(d.journalPhase);
            entries[pathKey(d.path)] = o;
            continue;
        }

        const QString target = d.moveTarget.isEmpty() ? d.linkTarget : d.moveTarget;
        QJsonObject o;
        o[QStringLiteral("path")] = d.path;
        o[QStringLiteral("name")] = d.name;
        o[QStringLiteral("target")] = target;
        if (!d.oldPath.isEmpty())
            o[QStringLiteral("old")] = d.oldPath;
        o[QStringLiteral("phase")] = phaseName(d.journalPhase);
        entries[pathKey(d.path)] = o;
    }
    writeJournalMap(entries);
}

void AppDataMovePanel::removeJournalEntry(const QString& path)
{
    QMap<QString, QJsonObject> entries = readJournalByPath();
    if (entries.remove(pathKey(path)) == 0)
        return;
    writeJournalMap(entries);
}

// A relocation that was interrupted (crash, power loss, a link that could not be
// created) leaves the folder and its data disagreeing. The journal says how far
// each one got, so the mismatch can be replayed into a consistent state instead
// of being left for the user to discover. The one rule that is never broken:
// nothing is deleted unless another complete copy is known to exist.
void AppDataMovePanel::loadJournal()
{
    const QMap<QString, QJsonObject> entries = readJournalByPath();
    if (entries.isEmpty())
        return;

    // Paths a journal entry already claims as its own renamed-aside copy, so the
    // orphan sweep at the bottom does not schedule the same delete twice.
    QSet<QString> claimedOld;
    // Residue entries whose folder is already gone, collected so the journal can
    // be rewritten once the iteration is over instead of during it.
    QStringList staleResidues;

    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QJsonObject o = it.value();
        const QString path = QDir::toNativeSeparators(
            QDir::cleanPath(o.value(QStringLiteral("path")).toString()));
        const QString target = QDir::toNativeSeparators(
            QDir::cleanPath(o.value(QStringLiteral("target")).toString()));
        if (path.isEmpty())
            continue;

        int phase = phaseFromName(o.value(QStringLiteral("phase")).toString());
        if (phase == kPhaseNone) {
            // Entries written by an older build carry only a target; they were
            // only ever written once the copy had been verified.
            phase = target.isEmpty() ? kPhaseNone : kPhaseLinked;
        }
        if (phase == kPhaseNone)
            continue;

        int dirIndex = -1;
        for (int i = 0; i < static_cast<int>(m_dirs.size()); ++i) {
            if (QString::compare(m_dirs[i].path, path, Qt::CaseInsensitive) == 0) {
                dirIndex = i;
                break;
            }
        }
        if (dirIndex < 0) {
            // Not part of the scanned scope. Recreate a row for it only when the
            // original path is empty — otherwise there is nothing to reconcile.
            if (QDir(path).exists())
                continue;
            DataDir d;
            d.path = path;
            const QString recorded = o.value(QStringLiteral("name")).toString();
            d.name = recorded.isEmpty() ? QFileInfo(path).fileName() : recorded;
            d.matchedApp = matchInstalledProgram(d.name, m_installedNames);
            m_dirs.push_back(d);
            dirIndex = static_cast<int>(m_dirs.size()) - 1;
        }

        DataDir& d = m_dirs[dirIndex];
        const bool srcIsLink = isReparsePoint(d.path);
        if (d.linkTarget.isEmpty())
            d.linkTarget = target;
        if (d.moveTarget.isEmpty())
            d.moveTarget = target;
        d.knownMove = true;

        // A leftover this app put on another drive and could not delete. It is
        // not a relocation: the data is already at its original path, so there
        // is nothing to replay and nothing to reconcile. What has to survive is
        // the fact that the folder is OURS — that is what lets the row offer to
        // delete it, and what lets a later move tell our own leftover apart from
        // a folder that was always the user's.
        if (phase == kPhaseResidue) {
            const QString residue = QDir::toNativeSeparators(QDir::cleanPath(
                o.value(QStringLiteral("residue")).toString(target)));
            d.linkTarget.clear();
            d.moveTarget.clear();
            d.knownMove = false;
            if (residue.isEmpty() || !QDir(residue).exists()) {
                // Already gone: deleted by hand, or by a cleanup that never got
                // to write it down. Nothing left to remember.
                d.residueTarget.clear();
                d.journalPhase = kPhaseNone;
                staleResidues << path;
            } else {
                d.residueTarget = residue;
                d.journalPhase = kPhaseResidue;
                d.state = kStateIdle;
            }
            continue;
        }

        const QString recordedOld = o.value(QStringLiteral("old")).toString();
        const QString old = recordedOld.isEmpty()
                                ? oldPathOf(d.path)
                                : QDir::toNativeSeparators(QDir::cleanPath(recordedOld));
        const bool oldExists = QDir(old).exists() || QFile::exists(old);
        if (oldExists)
            claimedOld.insert(pathKey(old));

        RepairItem item;
        item.dirIndex = dirIndex;

        if (phase == kPhaseCopying) {
            if (srcIsLink || !QDir(d.path).exists()) {
                // No original to compare against, so there may be only one copy
                // left. Touch nothing and let the row say "unfinished".
                d.journalPhase = kPhaseCopying;
                d.state = kStateFailed;
                continue;
            }
            if (holdsNothing(d.path)) {
                d.journalPhase = kPhaseCopying;
                d.state = kStateFailed;
                continue;
            }
            // The original is provably complete and untouched, so whatever sits
            // on the other drive is one of our own half-written copies.
            item.drop << target;
            item.forget = true;
            d.state = kStateFailed;
        } else {
            if (!srcIsLink && !target.isEmpty() && QDir(target).exists()) {
                // Verified copy on the other drive, no junction here yet: finish
                // the hand-over by putting the link in place.
                item.linkAt = d.path;
                item.linkTo = target;
            }
            if (oldExists)
                item.drop << old;
            d.journalPhase = phase;
            d.state = srcIsLink ? kStateMoved : kStateFailed;
        }

        if (!item.linkAt.isEmpty() || !item.drop.isEmpty())
            m_repairs.append(item);
    }

    // Written back only now: rewriting the file inside the loop would re-read it
    // under the iteration.
    for (const QString& gone : staleResidues)
        removeJournalEntry(gone);

    // Stale copies left behind by a move that finished but could not clean up
    // after itself. Droppable only when the name they belong to is a junction
    // now: that proves the relocation completed and the copy is redundant.
    for (const QString& orphan : m_orphanOlds) {
        if (claimedOld.contains(pathKey(orphan)))
            continue;
        const QString owner = orphan.left(orphan.size() - kOldSuffix.size());
        if (!isReparsePoint(owner))
            continue;
        RepairItem item;
        item.drop << orphan;
        m_repairs.append(item);
    }
}

void AppDataMovePanel::startRepair()
{
    if (m_repairs.isEmpty())
        return;
    m_repairPos = 0;
    m_repairStep = 0;
    m_repairLinked = 0;
    m_repairDropped = 0;
    m_repairActive = true;
    m_scanBtn->setEnabled(false);
    m_progressHideTimer->stop();
    m_progressRow->setVisible(true);
    m_progress->setRange(0, 0);
    m_scanStatus->setText(I18n::tr("app_move.repairing"));
    repairNext();
}

// Small state machine per repair: put the junction in place, then drop whatever
// is now redundant. Both the link and the deletes are asynchronous, so the pass
// comes back here after each one finishes.
void AppDataMovePanel::repairNext()
{
    if (!m_repairActive)
        return;
    if (m_repairPos >= m_repairs.size()) {
        finishRepair();
        return;
    }

    RepairItem& item = m_repairs[m_repairPos];

    if (m_repairStep == 0) {
        if (!item.linkAt.isEmpty() && !isReparsePoint(item.linkAt)) {
            startProc(cmdPath(), mkLinkArgs(item.linkAt, item.linkTo));
            m_repairStep = 1;
            return;
        }
        m_repairStep = 1;
    }

    if (m_repairStep == 1) {
        // Judged by the outcome rather than by an exit code: what matters is
        // whether the path is a junction now.
        if (!item.linkAt.isEmpty() && isReparsePoint(item.linkAt))
            ++m_repairLinked;
        if (!item.drop.isEmpty()) {
            m_repairStep = 2;
            deleteTreeAsync(item.drop, 0);
            return;
        }
        m_repairStep = 2;
    }

    // Step 2: this item is done. Put its row — and the journal — into the state
    // the repair actually produced.
    if (item.dirIndex >= 0 && item.dirIndex < static_cast<int>(m_dirs.size())) {
        DataDir& d = m_dirs[item.dirIndex];
        if (item.forget) {
            d.journalPhase = kPhaseNone;
            d.linkTarget.clear();
            d.moveTarget.clear();
            d.oldPath.clear();
            d.knownMove = false;
            d.state = kStateIdle;
            d.size = -1;
            removeJournalEntry(d.path);
            // While a scan is running its own loop will measure this folder
            // again; starting a second job on the single watcher would lose one.
            if (!m_scanning)
                startNextSizeJob();
        } else {
            d.journalPhase = kPhaseLinked;
            d.oldPath.clear();
            d.knownMove = true;
            d.state = isReparsePoint(d.path) ? kStateMoved : kStateFailed;
        }
        const int row = rowOfDir(item.dirIndex);
        if (row >= 0)
            refreshRow(row);
    }
    if (!item.drop.isEmpty() && m_deleteOk)
        ++m_repairDropped;

    ++m_repairPos;
    m_repairStep = 0;
    repairNext();
}

void AppDataMovePanel::finishRepair()
{
    m_repairActive = false;
    m_repairs.clear();
    m_scanBtn->setEnabled(!m_busy);
    saveJournal();
    refreshSummary();
    m_progressHideTimer->start(1200);
    if (m_repairLinked == 0 && m_repairDropped == 0)
        return;
    notify(I18n::tr("app_move.title"),
           I18n::tr("app_move.repair_done", QMap<QString, QString>{
               {"linked", QString::number(m_repairLinked)},
               {"dropped", QString::number(m_repairDropped)}}));
}
