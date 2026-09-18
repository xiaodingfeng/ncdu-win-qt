#include "SystemOptPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QProcess>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QDateTime>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <winsvc.h>
#  include <sddl.h>
#  include <aclapi.h>
#endif

#include "Style.h"
#include "I18n.h"
#include "DialogI18n.h"
#include "Logger.h"
#include "ToggleSwitch.h"

namespace {

// The auto-update guard re-runs its check whenever the card comes into view. It
// never fires twice in quick succession: something else on the machine may be
// setting the same values, and two programs undoing each other in a loop is
// worse than the block it is trying to remove.
constexpr int kGuardCooldownMs = 60000;

// Where the guard's on/off preference lives. Same key the rest of the app uses,
// so a user can see every NcduWin setting in one place.
const QString kRegRoot = QStringLiteral("HKEY_CURRENT_USER\\Software\\NcduWin");
const QString kRegUpdateGuard = QStringLiteral("AutoUpdateGuard");

// netsh's success line for a reset sub-section, in the languages this app
// ships: English prints "Ok!", Simplified Chinese prints "完成!". A denial
// ("Access is denied." / "拒绝访问。") on one sub-section is routine for these
// resets — the reset still takes effect after the reboot — so any success line
// is enough to treat the step as done.
const QStringList kNetshResetSuccessMarkers{
    QStringLiteral("Ok!"),
    QStringLiteral("完成"),
};

#ifdef _WIN32
QString systemExe(const QString& fileName)
{
    return qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/") + fileName;
}

QString powerShellExe()
{
    return qEnvironmentVariable("SystemRoot")
           + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
}

bool readRegDword(HKEY root, const wchar_t* subKey, const wchar_t* name, DWORD& value)
{
    HKEY hKey = NULL;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(hKey, name, NULL, &type,
                                     reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS
                    && type == REG_DWORD;
    RegCloseKey(hKey);
    return ok;
}

bool readRegString(HKEY root, const wchar_t* subKey, const wchar_t* name, QString& value)
{
    HKEY hKey = NULL;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD size = 0;
    DWORD type = 0;
    bool ok = false;
    if (RegQueryValueExW(hKey, name, NULL, &type, nullptr, &size) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(hKey, name, NULL, &type,
                             reinterpret_cast<LPBYTE>(buf.data()), &size) == ERROR_SUCCESS) {
            value = QString::fromWCharArray(buf.data());
            ok = true;
        }
    }
    RegCloseKey(hKey);
    return ok;
}

DWORD serviceStartType(const wchar_t* serviceName)
{
    DWORD startType = static_cast<DWORD>(-1);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_CONFIG);
        if (svc) {
            DWORD bytesNeeded = 0;
            QueryServiceConfigW(svc, nullptr, 0, &bytesNeeded);
            if (bytesNeeded > 0) {
                std::vector<BYTE> buf(bytesNeeded);
                auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
                if (QueryServiceConfigW(svc, cfg, bytesNeeded, &bytesNeeded))
                    startType = cfg->dwStartType;
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    return startType;
}

bool isServiceDisabled(const wchar_t* serviceName)
{
    return serviceStartType(serviceName) == SERVICE_DISABLED;
}

// Tools like "Windows Update Blocker" go one step further than setting the
// start type to disabled: they rewrite the service's security descriptor so
// that even an elevated administrator gets ERROR_ACCESS_DENIED from
// "sc config". Without noticing that, the enable path runs, "succeeds", and
// the very same reasons are still on screen afterwards.
bool serviceConfigLocked(const wchar_t* serviceName)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_CHANGE_CONFIG);
    const bool locked = (svc == nullptr) && GetLastError() == ERROR_ACCESS_DENIED;
    if (svc)
        CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return locked;
}

// The service's DACL as an SDDL string; empty when it cannot be read, which is
// exactly the situation a blocker creates — there the standard allow set is
// used as the starting point instead.
QString serviceSddl(const wchar_t* serviceName)
{
    QString sddl;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return sddl;
    SC_HANDLE svc = OpenServiceW(scm, serviceName, READ_CONTROL);
    if (svc) {
        DWORD bytesNeeded = 0;
        QueryServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, nullptr, 0, &bytesNeeded);
        if (bytesNeeded > 0) {
            std::vector<BYTE> buf(bytesNeeded);
            if (QueryServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, buf.data(),
                                           bytesNeeded, &bytesNeeded)) {
                LPWSTR str = nullptr;
                if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
                        buf.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &str, nullptr)
                    && str) {
                    sddl = QString::fromWCharArray(str);
                    LocalFree(str);
                }
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return sddl;
}

// Take ownership of a locked service object and write a fresh DACL, or as a
// last resort delete the service's persisted security descriptor so Windows
// rebuilds the default one.
//
// Why this exists: "sc sdset" repairs a blocker-rewritten descriptor only when
// the caller still holds WRITE_DAC on the service. A blocker that denies
// WRITE_DAC to Administrators (Windows Update Blocker does) therefore refuses
// the very command that is supposed to remove the denial — the unlock step
// fails with access denied and every later "sc config" fails with it.
//
// Two layers, tried in order:
//  1. SeTakeOwnershipPrivilege. Changing the OWNER of an object is allowed
//     while holding that privilege regardless of what the DACL denies, and the
//     owner may then always rewrite the DACL. SetNamedSecurityInfoW does both
//     in one call and updates the SCM's in-memory descriptor immediately.
//  2. HKLM\SYSTEM\CurrentControlSet\Services\<name>\Security — the binary blob
//     where the SCM persists a custom descriptor. Deleting it (the registry
//     key's own ACL still grants Administrators full control, so this does not
//     go through the service object's DACL) makes the SCM rebuild the default
//     descriptor at the next boot. *needsReboot* reports this case.
bool resetServiceSecurityNative(const wchar_t* serviceName, bool* needsReboot)
{
    *needsReboot = false;

    // Privileges first: without SeTakeOwnershipPrivilege step 1 is just
    // another access-denied call.
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        for (const wchar_t* priv : {SE_TAKE_OWNERSHIP_NAME, SE_RESTORE_NAME}) {
            LUID luid{};
            if (LookupPrivilegeValueW(nullptr, priv, &luid)) {
                TOKEN_PRIVILEGES tp{};
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            }
        }
        CloseHandle(token);
    }

    // The allow set repairServiceSddl() builds as SDDL text, as explicit
    // accesses: SYSTEM may control but not reconfigure, Administrators may do
    // everything, interactive and service logons may read and start.
    PSID sidSystem = nullptr, sidAdmins = nullptr, sidInteractive = nullptr, sidService = nullptr;
    ConvertStringSidToSidW(L"S-1-5-18", &sidSystem);
    ConvertStringSidToSidW(L"S-1-5-32-544", &sidAdmins);
    ConvertStringSidToSidW(L"S-1-5-4", &sidInteractive);
    ConvertStringSidToSidW(L"S-1-5-6", &sidService);

    EXPLICIT_ACCESSW ea[4]{};
    ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea[0].grfAccessMode = GRANT_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidSystem);
    ea[1].grfAccessPermissions = GENERIC_ALL;
    ea[1].grfAccessMode = GRANT_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidAdmins);
    for (int i = 2; i < 4; ++i) {
        ea[i].grfAccessPermissions = READ_CONTROL | SERVICE_QUERY_CONFIG
            | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS
            | SERVICE_START | SERVICE_PAUSE_CONTINUE;
        ea[i].grfAccessMode = GRANT_ACCESS;
        ea[i].grfInheritance = NO_INHERITANCE;
        ea[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    }
    ea[2].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidInteractive);
    ea[3].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidService);

    PACL newAcl = nullptr;
    if (SetEntriesInAclW(4, ea, nullptr, &newAcl) == ERROR_SUCCESS && newAcl) {
        const DWORD err = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(serviceName), SE_SERVICE,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            sidAdmins, nullptr, newAcl, nullptr);
        LocalFree(newAcl);
        if (err == ERROR_SUCCESS) {
            for (PSID sid : {sidSystem, sidAdmins, sidInteractive, sidService})
                if (sid) LocalFree(sid);
            return true;
        }
        Logger::warn(QStringLiteral("[sysopt] SetNamedSecurityInfoW on service failed, winError=%1").arg(err));
    } else {
        Logger::warn(QStringLiteral("[sysopt] SetEntriesInAclW for service DACL failed"));
    }

    // Fallback: drop the persisted descriptor, Windows restores the default at
    // the next boot.
    const QString subKey = QStringLiteral(
        "SYSTEM\\CurrentControlSet\\Services\\%1\\Security").arg(QString::fromWCharArray(serviceName));
    const LSTATUS lr = RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                                      reinterpret_cast<const wchar_t*>(subKey.utf16()));
    for (PSID sid : {sidSystem, sidAdmins, sidInteractive, sidService})
        if (sid) LocalFree(sid);
    if (lr == ERROR_SUCCESS) {
        *needsReboot = true;
        return true;
    }
    Logger::warn(QStringLiteral("[sysopt] RegDeleteTreeW on service Security key failed, status=%1").arg(lr));
    return false;
}

// The hosts file is the one place a "disable Windows Update" tool can hide that
// is not a registry value, and it is worth checking: an entry pointing the
// update hosts at 127.0.0.1 makes the whole update stack look broken while every
// setting reads as "enabled".
bool hostsBlocksWindowsUpdate()
{
    const QString path = qEnvironmentVariable("SystemRoot")
                         + QStringLiteral("/System32/drivers/etc/hosts");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    while (!f.atEnd()) {
        QString line = QString::fromLocal8Bit(f.readLine());
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line.truncate(hash);   // a commented-out entry changes nothing
        line = line.trimmed().toLower();
        if (line.isEmpty())
            continue;
        if (line.contains(QStringLiteral("windowsupdate"))
            || line.contains(QStringLiteral("update.microsoft"))
            || line.contains(QStringLiteral("delivery.mp.microsoft")))
            return true;
    }
    return false;
}

// Every switch a "disable Windows Update" tool flips, in one place. Detection
// reports what is really in the registry and the service database, not what this
// app once asked for — that is the whole point of the guard.
struct UpdateBlockState {
    bool serviceDisabled = false;   // wuauserv: the update service itself
    bool usoDisabled = false;       // UsoSvc: orchestrates the downloads
    bool bitsDisabled = false;      // BITS: moves the bytes
    bool medicDisabled = false;     // WaaSMedicSvc: the service that repairs the rest
    bool noAutoUpdate = false;      // AU\NoAutoUpdate = 1
    bool noInternetWU = false;      // DoNotConnectToWindowsUpdateInternetLocations = 1
    bool noAccess = false;          // DisableWindowsUpdateAccess = 1
    bool noUxAccess = false;        // SetDisableUXWUAccess = 1
    bool neverCheck = false;        // Auto Update\AUOptions = 1 ("never check")
    bool legacyNoAuto = false;      // CurrentVersion\...\Auto Update\NoAutoUpdate = 1
    bool wsusRedirected = false;    // UseWUServer = 1 with an address pointing nowhere
    bool hostsBlocked = false;      // update hosts redirected in the hosts file
    bool svcAclLocked = false;      // a blocker rewrote a service's security descriptor

    bool blocked() const
    {
        // WaaSMedicSvc is deliberately not part of this: it is the service that
        // repairs the update stack, so disabling it does not stop updates from
        // working. Counting it would have this app declare "updates are off" on
        // machines where they are perfectly fine. It is still listed among the
        // reasons once something else really is wrong, and turning updates back
        // on restores it either way. A locked security descriptor is the same
        // kind of passenger: it only hurts when something else is wrong too,
        // but then it is the reason the repair does not stick.
        return serviceDisabled || usoDisabled || bitsDisabled
               || noAutoUpdate || noInternetWU || noAccess || noUxAccess || neverCheck
               || legacyNoAuto || wsusRedirected || hostsBlocked;
    }

    // One i18n key per flag that is set. The panel localizes them; the log gets
    // the keys, which stay readable whoever reads the file.
    QStringList reasonKeys() const
    {
        QStringList keys;
        if (serviceDisabled)
            keys << QStringLiteral("sysopt.reason.svc_wu");
        if (usoDisabled)
            keys << QStringLiteral("sysopt.reason.svc_uso");
        if (bitsDisabled)
            keys << QStringLiteral("sysopt.reason.svc_bits");
        if (medicDisabled)
            keys << QStringLiteral("sysopt.reason.svc_medic");
        if (noAutoUpdate)
            keys << QStringLiteral("sysopt.reason.policy_noauto");
        if (noInternetWU)
            keys << QStringLiteral("sysopt.reason.policy_nonet");
        if (noAccess)
            keys << QStringLiteral("sysopt.reason.policy_noaccess");
        if (noUxAccess)
            keys << QStringLiteral("sysopt.reason.policy_noux");
        if (neverCheck)
            keys << QStringLiteral("sysopt.reason.auoptions");
        if (legacyNoAuto)
            keys << QStringLiteral("sysopt.reason.legacy");
        if (wsusRedirected)
            keys << QStringLiteral("sysopt.reason.wsus");
        if (hostsBlocked)
            keys << QStringLiteral("sysopt.reason.hosts");
        if (svcAclLocked)
            keys << QStringLiteral("sysopt.reason.svc_acl");
        return keys;
    }
};

UpdateBlockState queryUpdateBlockState()
{
    static const wchar_t* kAU = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
    static const wchar_t* kWU = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate";
    static const wchar_t* kLegacy =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update";

    UpdateBlockState st;
    st.serviceDisabled = isServiceDisabled(L"wuauserv");
    st.usoDisabled = isServiceDisabled(L"UsoSvc");
    st.bitsDisabled = isServiceDisabled(L"BITS");
    st.medicDisabled = isServiceDisabled(L"WaaSMedicSvc");

    DWORD v = 0;
    if (readRegDword(HKEY_LOCAL_MACHINE, kAU, L"NoAutoUpdate", v))
        st.noAutoUpdate = (v == 1);
    if (readRegDword(HKEY_LOCAL_MACHINE, kWU, L"DoNotConnectToWindowsUpdateInternetLocations", v))
        st.noInternetWU = (v == 1);
    if (readRegDword(HKEY_LOCAL_MACHINE, kWU, L"DisableWindowsUpdateAccess", v))
        st.noAccess = (v == 1);
    if (readRegDword(HKEY_LOCAL_MACHINE, kWU, L"SetDisableUXWUAccess", v))
        st.noUxAccess = (v == 1);
    if (readRegDword(HKEY_LOCAL_MACHINE, kLegacy, L"AUOptions", v))
        st.neverCheck = (v == 1);
    if (readRegDword(HKEY_LOCAL_MACHINE, kLegacy, L"NoAutoUpdate", v))
        st.legacyNoAuto = (v == 1);

    // A WSUS address is only treated as a block when it points back at this
    // machine: a real server on a corporate network is a legitimate setup and
    // must not be "repaired" away.
    if (readRegDword(HKEY_LOCAL_MACHINE, kAU, L"UseWUServer", v) && v == 1) {
        QString server;
        if (readRegString(HKEY_LOCAL_MACHINE, kWU, L"WUServer", server)) {
            const QString s = server.toLower();
            st.wsusRedirected = s.contains(QStringLiteral("127.0.0.1"))
                                || s.contains(QStringLiteral("localhost"))
                                || s.contains(QStringLiteral("0.0.0.0"));
        } else {
            st.wsusRedirected = true;   // switched on with no server at all
        }
    }

    st.hostsBlocked = hostsBlocksWindowsUpdate();
    st.svcAclLocked = serviceConfigLocked(L"wuauserv") || serviceConfigLocked(L"UsoSvc")
                      || serviceConfigLocked(L"BITS") || serviceConfigLocked(L"WaaSMedicSvc");
    return st;
}
#endif

} // namespace

QStringList netshResetSuccessMarkers()
{
    return kNetshResetSuccessMarkers;
}

bool stepOutputShowsSuccess(const QStringList& markers, const QString& output)
{
    for (const QString& marker : markers) {
        if (!marker.isEmpty() && output.contains(marker, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool stepOutputAsksForReboot(const QString& output)
{
    static const QStringList kMarkers{
        QStringLiteral("重新启动"),   // zh: 重新启动计算机来完成此操作
        QStringLiteral("重启"),       // zh: 某些工具写「重启」
        QStringLiteral("restart"),    // en: Restart the computer to complete
        QStringLiteral("reboot"),
    };
    return stepOutputShowsSuccess(kMarkers, output);
}

// The allow set every Windows service starts with: SYSTEM may control but not
// reconfigure, Administrators may do everything, interactive and service users
// may read status and start. Good enough for the four update services, and it
// is what the common blockers strip away when they deny access.
QString repairServiceSddl(const QString& currentSddl)
{
    static const QStringList kStandardAllow{
        QStringLiteral("(A;;CCLCSWRPWPDTLOCRRC;;;SY)"),
        QStringLiteral("(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"),
        QStringLiteral("(A;;CCLCSWLOCRRC;;;IU)"),
        QStringLiteral("(A;;CCLCSWLOCRRC;;;SU)"),
    };

    QString dacl = currentSddl;
    // A full descriptor ("O:...G:...D:...") may have been passed; only the D:
    // section is ours to rewrite.
    const int dPos = dacl.lastIndexOf(QLatin1String("D:"));
    if (dPos >= 0)
        dacl = dacl.mid(dPos);

    // Keep the allow ACEs as they are (a legitimately customised descriptor is
    // not ours to flatten), drop every deny ACE — on a service the only thing
    // that ever added one is a blocker.
    QStringList keep;
    qsizetype pos = 0;
    while ((pos = dacl.indexOf(QLatin1Char('('), pos)) >= 0) {
        const qsizetype end = dacl.indexOf(QLatin1Char(')'), pos);
        if (end < 0)
            break;
        const QString ace = dacl.mid(pos, end - pos + 1);   // "(...)"
        if (!ace.startsWith(QStringLiteral("(D;")))
            keep << ace;
        pos = end + 1;
    }

    // Make sure the standard allows are present exactly once, appended in a
    // deterministic order.
    QStringList all = keep;
    for (const QString& allow : kStandardAllow)
        if (!all.contains(allow))
            all << allow;

    return QStringLiteral("D:") + all.join(QString());
}

SystemOptPanel::SystemOptPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
    checkAutoUpdateStatus();
    // Deferred: this runs inside the main window's own construction, and the
    // guard may need to start processes. Letting the event loop come up first
    // keeps the window's first paint from waiting on a repair.
    QTimer::singleShot(0, this, [this]() { maybeGuardAutoUpdate(); });
}

void SystemOptPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // Queued for the same reason, plus one more: the user may have just switched
    // to this tab, and a job must not be started from inside that repaint.
    QTimer::singleShot(0, this, [this]() { maybeGuardAutoUpdate(); });
}

void SystemOptPanel::buildUI()
{
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(16, 16, 16, 16);
    mainLay->setSpacing(12);

    // Header section
    m_headerTitle = new QLabel(I18n::tr("sysopt.title"));
    m_headerTitle->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 700; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_headerDesc = new QLabel(I18n::tr("sysopt.desc"));
    m_headerDesc->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
        .arg(QString::fromLatin1(C::TEXT_MUTED())));

    mainLay->addWidget(m_headerTitle);
    mainLay->addWidget(m_headerDesc);
    mainLay->addSpacing(4);

    // Scrollable content area for cards
    m_scrollArea = new QScrollArea;
    QScrollArea* scrollArea = m_scrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; } QWidget#scrollContent { background: transparent; }"));

    auto* scrollContent = new QWidget;
    scrollContent->setObjectName(QStringLiteral("scrollContent"));
    auto* cardLay = new QVBoxLayout(scrollContent);
    cardLay->setContentsMargins(0, 0, 0, 0);
    cardLay->setSpacing(10);

    // `slot` always receives the layout the card's action widget goes into.
    QHBoxLayout* slot = nullptr;

    // Card 1: Move app save folders to another drive
    m_cardSync = createCard(QStringLiteral("📦"), m_titleSync, m_descSync, m_statusSync, slot,
                            &m_badgeSync);
    m_titleSync->setText(I18n::tr("sysopt.sync.title"));
    m_descSync->setText(I18n::tr("sysopt.sync.desc"));
    m_statusSync->setText(QString());
    if (m_badgeSync) {
        m_badgeSync->setText(I18n::tr("sysopt.sync.badge"));
        m_badgeSync->setVisible(true);
    }
    m_btnSync = makeCardButton(QStringLiteral("sysopt.sync.btn"));
    connect(m_btnSync, &QPushButton::clicked, this, &SystemOptPanel::onOpenAppSync);
    slot->addWidget(m_btnSync, 0, Qt::AlignVCenter);
    cardLay->addWidget(m_cardSync);

    // Card 2: Windows Update - a switch, because it describes an on/off state.
    m_cardUpdate = createCard(QStringLiteral("🔄"), m_titleUpdate, m_descUpdate, m_statusUpdate, slot);
    m_titleUpdate->setText(I18n::tr("sysopt.update.title"));
    m_descUpdate->setText(I18n::tr("sysopt.update.desc"));
    m_statusUpdate->setText(I18n::tr("sysopt.update.status_checking"));
    m_switchUpdate = new ToggleSwitch;
    connect(m_switchUpdate, &QAbstractButton::clicked, this, &SystemOptPanel::onToggleAutoUpdate);
    slot->addWidget(m_switchUpdate, 0, Qt::AlignVCenter);
    cardLay->addWidget(m_cardUpdate);

    // Card 3: Network Fix
    m_cardNet = createCard(QStringLiteral("🌐"), m_titleNet, m_descNet, m_statusNet, slot);
    m_titleNet->setText(I18n::tr("sysopt.netfix.title"));
    m_descNet->setText(I18n::tr("sysopt.netfix.desc"));
    m_statusNet->setText(QString());
    m_btnNet = makeCardButton(QStringLiteral("sysopt.netfix.btn"));
    connect(m_btnNet, &QPushButton::clicked, this, &SystemOptPanel::onNetEmergencyFix);
    slot->addWidget(m_btnNet, 0, Qt::AlignVCenter);
    cardLay->addWidget(m_cardNet);

    // Card 4: DNS Flush
    m_cardDns = createCard(QStringLiteral("⚡"), m_titleDns, m_descDns, m_statusDns, slot);
    m_titleDns->setText(I18n::tr("sysopt.dns.title"));
    m_descDns->setText(I18n::tr("sysopt.dns.desc"));
    m_statusDns->setText(QString());
    m_btnDns = makeCardButton(QStringLiteral("sysopt.dns.btn"));
    connect(m_btnDns, &QPushButton::clicked, this, &SystemOptPanel::onFlushDns);
    slot->addWidget(m_btnDns, 0, Qt::AlignVCenter);
    cardLay->addWidget(m_cardDns);

    // Card 5: Store Reset
    // The glyph is a surrogate pair plus a variation selector, written as code
    // units because MSVC rejects lone surrogates in \u escapes.
    QString storeIcon;
    storeIcon += QChar(0xD83D);
    storeIcon += QChar(0xDECD);
    storeIcon += QChar(0xFE0F);
    m_cardStore = createCard(storeIcon, m_titleStore, m_descStore, m_statusStore, slot);
    m_titleStore->setText(I18n::tr("sysopt.store.title"));
    m_descStore->setText(I18n::tr("sysopt.store.desc"));
    m_statusStore->setText(QString());
    m_btnStore = makeCardButton(QStringLiteral("sysopt.store.btn"));
    connect(m_btnStore, &QPushButton::clicked, this, &SystemOptPanel::onResetStore);
    slot->addWidget(m_btnStore, 0, Qt::AlignVCenter);
    cardLay->addWidget(m_cardStore);

    cardLay->addStretch(1);
    scrollArea->setWidget(scrollContent);
    mainLay->addWidget(scrollArea, 1);

#ifdef _WIN32
    // A single reused process: the cards never run in parallel, so one instance
    // is enough and it keeps the "one job at a time" rule easy to enforce.
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    m_stepTimer = new QTimer(this);
    m_stepTimer->setSingleShot(true);

    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        m_stepTimer->stop();
        if (m_stepIndex < m_steps.size()) {
            const OptStep& step = m_steps.at(m_stepIndex);
            // netsh exits with a non-zero code as soon as one sub-section is
            // denied, while its success lines show the reset did run. Judge the
            // step by the output, never by the exit code alone.
            const QString output = QString::fromLocal8Bit(m_proc->readAllStandardOutput());
            const bool ok = (exitCode == 0) || step.allowFailure
                            || stepOutputShowsSuccess(step.successMarkers, output);
            if (!ok)
                m_stepErrors << I18n::tr(step.labelKey);
            else if (stepOutputAsksForReboot(output))
                m_rebootNeeded = true;
            ++m_stepIndex;
        }
        runNextStep();
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // finished() is not emitted when the binary could not be launched at all.
        if (error != QProcess::FailedToStart)
            return;
        m_stepTimer->stop();
        if (m_stepIndex < m_steps.size()) {
            if (!m_steps.at(m_stepIndex).allowFailure)
                m_stepErrors << I18n::tr(m_steps.at(m_stepIndex).labelKey);
            ++m_stepIndex;
        }
        runNextStep();
    });
    connect(m_stepTimer, &QTimer::timeout, this, [this]() {
        if (m_proc->state() != QProcess::NotRunning)
            m_proc->kill();   // finished() then reports a non-zero exit code
    });
#endif
}

QFrame* SystemOptPanel::createCard(const QString& iconText,
                                   QLabel*& titleLabel,
                                   QLabel*& descLabel,
                                   QLabel*& statusLabel,
                                   QHBoxLayout*& actionSlot,
                                   QLabel** badgeLabel)
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("optCard"));
    card->setStyleSheet(QStringLiteral(
        "QFrame#optCard {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "}"
        "QFrame#optCard:hover {"
        "  border-color: %3;"
        "}"
    ).arg(QString::fromLatin1(C::SURFACE()),
          QString::fromLatin1(C::BORDER()),
          QString::fromLatin1(C::PRIMARY())));

    auto* lay = new QHBoxLayout(card);
    lay->setContentsMargins(16, 14, 16, 14);
    lay->setSpacing(14);

    // Icon glyph
    auto* icon = new QLabel(iconText);
    icon->setFixedWidth(36);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QStringLiteral("font-size: 22px;"));
    lay->addWidget(icon, 0, Qt::AlignVCenter);

    // Center content
    auto* col = new QVBoxLayout;
    col->setSpacing(4);

    // Title with an optional highlight pill next to it — used to mark the one
    // feature this release is about.
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    titleLabel = new QLabel;
    titleLabel->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    titleRow->addWidget(titleLabel);
    if (badgeLabel) {
        *badgeLabel = new QLabel;
        (*badgeLabel)->setVisible(false);   // no text until retranslate fills it
        (*badgeLabel)->setObjectName(QStringLiteral("cardBadge"));
        (*badgeLabel)->setStyleSheet(QStringLiteral(
            "QLabel#cardBadge {"
            "  color: #FFFFFF;"
            "  background-color: %1;"
            "  font-size: 10px;"
            "  font-weight: 600;"
            "  border-radius: 7px;"
            "  padding: 2px 8px;"
            "}").arg(QString::fromLatin1(C::PRIMARY())));
        titleRow->addWidget(*badgeLabel);
        titleRow->addStretch(1);
    }
    col->addLayout(titleRow);

    descLabel = new QLabel;
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.4;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));
    col->addWidget(descLabel);

    statusLabel = new QLabel;
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 500; color: %1;")
        .arg(QString::fromLatin1(C::PRIMARY())));
    col->addWidget(statusLabel);

    lay->addLayout(col, 1);

    // The caller decides whether this slot holds a button or a switch.
    actionSlot = lay;
    return card;
}

QPushButton* SystemOptPanel::makeCardButton(const QString& textKey)
{
    auto* btn = new QPushButton(I18n::tr(textKey));
    btn->setObjectName(QStringLiteral("primary"));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setMinimumWidth(110);
    btn->setFixedHeight(32);
    return btn;
}

void SystemOptPanel::setActionsEnabled(bool enabled)
{
    if (m_switchUpdate)
        m_switchUpdate->setEnabled(enabled);
    const QList<QPushButton*> buttons = {m_btnNet, m_btnDns, m_btnStore, m_btnSync};
    for (QPushButton* btn : buttons) {
        if (btn)
            btn->setEnabled(enabled);
    }
}

void SystemOptPanel::setStatus(QLabel* status, const QString& text, const char* color)
{
    if (!status)
        return;
    status->setText(text);
    status->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 500; color: %1;")
        .arg(QString::fromLatin1(color)));
}

// The cards live in a scroll area, and Qt scrolls such an area whenever focus
// moves — closing a message box is enough for the list to jump to the bottom.
// Remembering the position and putting it back afterwards keeps the list still.
void SystemOptPanel::rememberScroll()
{
    if (!m_scrollArea)
        return;
    m_scrollPos = m_scrollArea->verticalScrollBar()->value();
}

void SystemOptPanel::restoreScroll()
{
    if (!m_scrollArea || m_scrollPos < 0)
        return;
    const int pos = m_scrollPos;
    m_scrollArea->verticalScrollBar()->setValue(pos);
    // The dialog is still being torn down and the status labels may grow right
    // after, so apply the position again once the event loop is idle.
    QTimer::singleShot(0, this, [this, pos]() {
        if (m_scrollArea)
            m_scrollArea->verticalScrollBar()->setValue(pos);
    });
}

void SystemOptPanel::checkAutoUpdateStatus()
{
#ifdef _WIN32
    if (isBusy())
        return;
    const UpdateBlockState st = queryUpdateBlockState();
    m_autoUpdateEnabled = !st.blocked();
    loadGuardSetting(st.blocked());

    if (m_autoUpdateEnabled) {
        setStatus(m_statusUpdate, I18n::tr("sysopt.update.status_enabled"), C::PRIMARY());
    } else {
        // Naming what is wrong is the difference between "updates are off" and
        // "something turned them off, here is what".
        QStringList reasons;
        for (const QString& key : st.reasonKeys())
            reasons << I18n::tr(key);
        setStatus(m_statusUpdate,
                  I18n::tr("sysopt.update.status_blocked",
                           QMap<QString, QString>{{"reason", reasons.join(QStringLiteral("、"))}}),
                  C::DANGER());
    }
    if (m_switchUpdate) {
        m_switchUpdate->setChecked(m_autoUpdateEnabled);
        m_switchUpdate->setToolTip(I18n::tr(m_autoUpdateEnabled
                                                ? "sysopt.update.tooltip_on"
                                                : "sysopt.update.tooltip_off"));
    }
#else
    m_statusUpdate->setText(I18n::tr("sysopt.update.status_disabled"));
#endif
}

void SystemOptPanel::loadGuardSetting(bool systemBlocked)
{
    if (m_guardLoaded)
        return;
    m_guardLoaded = true;

    QSettings reg(kRegRoot, QSettings::NativeFormat);
    if (reg.contains(kRegUpdateGuard)) {
        // Stored as a number, not as a bool: QSettings writes bools in a form
        // whose round trip through a foreign reader is ambiguous, and this value
        // decides whether the app touches the machine's update configuration.
        m_guardEnabled = reg.value(kRegUpdateGuard).toInt() != 0;
        return;
    }
    // First run, nothing stored yet: "updates are on and should stay on" is only
    // assumed when they really are on. A machine somebody else configured to be
    // off is left alone until its owner says otherwise here.
    m_guardEnabled = !systemBlocked;
    saveGuardSetting(m_guardEnabled);
}

void SystemOptPanel::saveGuardSetting(bool enabled)
{
    QSettings reg(kRegRoot, QSettings::NativeFormat);
    reg.setValue(kRegUpdateGuard, enabled ? 1 : 0);
    reg.sync();
}

void SystemOptPanel::maybeGuardAutoUpdate()
{
#ifdef _WIN32
    if (isBusy() || !m_guardEnabled)
        return;
    if (m_guardCooldown.isValid() && m_guardCooldown.elapsed() < kGuardCooldownMs)
        return;

    const UpdateBlockState st = queryUpdateBlockState();
    if (!st.blocked())
        return;

    m_guardCooldown.start();
    m_autoRepairReasons.clear();
    for (const QString& key : st.reasonKeys())
        m_autoRepairReasons << I18n::tr(key);

    Logger::warn(QStringLiteral("[sysopt] auto-update is blocked (%1); repairing because the "
                                "guard is on")
                     .arg(m_autoRepairReasons.join(QStringLiteral(", "))));

    rememberScroll();
    runUpdateSteps(/*enable=*/true, /*automatic=*/true);
#endif
}

void SystemOptPanel::onToggleAutoUpdate()
{
#ifdef _WIN32
    if (isBusy())
        return;

    const bool disabling = !m_switchUpdate->isChecked();
    const QString confirmMsg = disabling
        ? I18n::tr("sysopt.update.confirm_disable")
        : I18n::tr("sysopt.update.confirm_enable");

    rememberScroll();
    const bool confirmed = Dialogs::confirm(this, I18n::tr("sysopt.update.title"), confirmMsg);
    restoreScroll();
    if (!confirmed) {
        // The switch already flipped itself; put it back where it was.
        m_switchUpdate->setChecked(m_autoUpdateEnabled);
        return;
    }

    // The standing preference the guard acts on later: "off" must never be
    // undone behind the user's back, and "on" has to survive another program
    // turning it back off.
    saveGuardSetting(!disabling);

    runUpdateSteps(/*enable=*/!disabling, /*automatic=*/false);
#endif
}

void SystemOptPanel::runUpdateSteps(bool enable, bool automatic)
{
    if (isBusy())
        return;
    const QList<OptStep> steps = buildUpdateSteps(enable);
    if (steps.isEmpty())
        return;
    startSteps(I18n::tr("sysopt.update.title"), steps, m_statusUpdate, nullptr, QString());
    m_jobTouchesUpdate = true;
    // After startSteps, which clears both flags: this job is the exception.
    m_autoRepair = automatic;
}

QList<SystemOptPanel::OptStep> SystemOptPanel::buildUpdateSteps(bool enable)
{
    const QString reg = systemExe(QStringLiteral("reg.exe"));
    const QString sc = systemExe(QStringLiteral("sc.exe"));
    const QString ps = powerShellExe();
    const QString kAU = QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU");
    const QString kWU = QStringLiteral("HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate");
    // The pre-Windows-10 location tools still write to.
    const QString kLegacy = QStringLiteral(
        "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update");

    // Why a policy + service pair instead of a single switch:
    //  - NoAutoUpdate only stops the *automatic* scan/download/install. The
    //    Settings page's "check for updates" button keeps working.
    //  - DoNotConnectToWindowsUpdateInternetLocations is what makes Windows
    //    report "some settings are managed by your organization" and fail the
    //    manual check.
    //  - SetDisableUXWUAccess removes the check-for-updates entry altogether.
    //  - The services are stopped (not just disabled) so the change applies
    //    immediately instead of after the next reboot, and WaaSMedicSvc is
    //    disabled because that repair service silently reverts them.
    const auto mk = [](const QString& key, const QString& prog, const QStringList& args,
                       int timeoutMs, bool allowFailure = false) {
        OptStep s;
        s.labelKey = key;
        s.program = prog;
        s.args = args;
        s.timeoutMs = timeoutMs;
        s.allowFailure = allowFailure;
        return s;
    };

    QList<OptStep> steps;
    // Queried once, up front: the enable path needs to know whether a blocker
    // has locked the services' security descriptors, and both directions share
    // the hosts / WSUS clean-up below.
    const UpdateBlockState st = queryUpdateBlockState();

    // A blocker that rewrote a service's security descriptor makes every
    // "sc config" below fail with access denied — the run would then quietly
    // do nothing and the very same reasons stay on screen. The lock bites BOTH
    // ways (a disabled service is just as impossible to re-enable as an
    // enabled one is to disable), so the repair runs before either branch.
    // "sc sdset" repairs the descriptor only while the caller still holds
    // WRITE_DAC, which a deny ACE can take away; the native repair below
    // takes ownership first, which no DACL can refuse. Only the services
    // actually found locked are touched.
    for (const wchar_t* name : {L"wuauserv", L"UsoSvc", L"BITS", L"WaaSMedicSvc"}) {
        if (!serviceConfigLocked(name))
            continue;
        bool needsReboot = false;
        const bool repaired = resetServiceSecurityNative(name, &needsReboot);
        Logger::info(QStringLiteral("[sysopt] service %1 descriptor locked; native repair=%2 reboot=%3")
                         .arg(QString::fromWCharArray(name))
                         .arg(repaired ? QStringLiteral("ok") : QStringLiteral("failed"))
                         .arg(needsReboot ? QStringLiteral("yes") : QStringLiteral("no")));
        if (needsReboot)
            m_sddlResetReboot = true;
        // The repair worked and the service accepts configuration again:
        // the sdset step would be busy-work (and, when the DACL now grants
        // WRITE_DAC, harmless but pointless). Skip it.
        if (repaired && !serviceConfigLocked(name))
            continue;
        const QString sddl = repairServiceSddl(serviceSddl(name));
        steps << mk(QStringLiteral("sysopt.step.upd_unlock"), sc,
                    {QStringLiteral("sdset"), QString::fromWCharArray(name), sddl},
                    30000, true);
    }

    // Scheduled tasks are the other thing that pulls the services back up:
    // "Scheduled Start" launches wuauserv on every logon and idle timer, and
    // the UpdateOrchestrator/sih family keep re-arming it. Left enabled, they
    // quietly turn the switch back on minutes after the services go down.
    // WaaSMedicSvc additionally repairs the whole set (start types included)
    // and refuses "sc config" even on a clean machine — its Start value has to
    // go through the registry, which admins can always write.
    const QString schtasks = systemExe(QStringLiteral("schtasks.exe"));
    const QStringList kUpdateTasks = {
        QStringLiteral("\\Microsoft\\Windows\\WindowsUpdate\\Scheduled Start"),
        QStringLiteral("\\Microsoft\\Windows\\WindowsUpdate\\sih"),
        QStringLiteral("\\Microsoft\\Windows\\WindowsUpdate\\sihproxy"),
        QStringLiteral("\\Microsoft\\Windows\\UpdateOrchestrator\\Schedule Scan"),
        QStringLiteral("\\Microsoft\\Windows\\UpdateOrchestrator\\Schedule Scan Static Task"),
        QStringLiteral("\\Microsoft\\Windows\\UpdateOrchestrator\\Universal Orchestrator Start"),
        QStringLiteral("\\Microsoft\\Windows\\UpdateOrchestrator\\UpdateModelTask"),
    };
    const QString kSvcKey = QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services");

    if (!enable) {
        steps << mk(QStringLiteral("sysopt.step.upd_noauto"), reg,
                    {QStringLiteral("add"), kAU, QStringLiteral("/v"), QStringLiteral("NoAutoUpdate"),
                     QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"),
                     QStringLiteral("1"), QStringLiteral("/f")}, 30000);
        steps << mk(QStringLiteral("sysopt.step.upd_nonet"), reg,
                    {QStringLiteral("add"), kWU,
                     QStringLiteral("/v"), QStringLiteral("DoNotConnectToWindowsUpdateInternetLocations"),
                     QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"),
                     QStringLiteral("1"), QStringLiteral("/f")}, 30000);
        steps << mk(QStringLiteral("sysopt.step.upd_noaccess"), reg,
                    {QStringLiteral("add"), kWU, QStringLiteral("/v"),
                     QStringLiteral("DisableWindowsUpdateAccess"), QStringLiteral("/t"),
                     QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("1"),
                     QStringLiteral("/f")}, 30000);
        steps << mk(QStringLiteral("sysopt.step.upd_noux"), reg,
                    {QStringLiteral("add"), kWU, QStringLiteral("/v"),
                     QStringLiteral("SetDisableUXWUAccess"), QStringLiteral("/t"),
                     QStringLiteral("REG_DWORD"), QStringLiteral("/d"), QStringLiteral("1"),
                     QStringLiteral("/f")}, 30000);
        // wuauserv depends on UsoSvc, so it has to go down first.
        steps << mk(QStringLiteral("sysopt.step.upd_stop"), sc,
                    {QStringLiteral("stop"), QStringLiteral("wuauserv")}, 60000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_stop"), sc,
                    {QStringLiteral("stop"), QStringLiteral("UsoSvc")}, 60000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_disable"), sc,
                    {QStringLiteral("config"), QStringLiteral("wuauserv"), QStringLiteral("start="),
                     QStringLiteral("disabled")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_disable"), sc,
                    {QStringLiteral("config"), QStringLiteral("UsoSvc"), QStringLiteral("start="),
                     QStringLiteral("disabled")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_disable"), sc,
                    {QStringLiteral("config"), QStringLiteral("WaaSMedicSvc"), QStringLiteral("start="),
                     QStringLiteral("disabled")}, 30000, true);
        // Belt and braces: "sc config" can still be refused (a locked
        // descriptor that even the repair could not clear, a race with a
        // running blocker), while the registry value behind the same setting
        // is always writable by an admin. A Start written here is what the
        // service actually reads on the next boot. WaaSMedicSvc NEEDS this
        // path on every machine — its own DACL denies SERVICE_CHANGE_CONFIG
        // by design, so the step above never worked for it.
        for (const wchar_t* name : {L"wuauserv", L"UsoSvc", L"WaaSMedicSvc"}) {
            steps << mk(QStringLiteral("sysopt.step.upd_disable"), reg,
                        {QStringLiteral("add"), kSvcKey + QLatin1Char('\\') + QString::fromWCharArray(name),
                         QStringLiteral("/v"), QStringLiteral("Start"),
                         QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"),
                         QStringLiteral("4"), QStringLiteral("/f")}, 30000, true);
        }
        // Stop the medic before it notices what just changed — a running one
        // reverts the start types above and re-arms everything else.
        steps << mk(QStringLiteral("sysopt.step.upd_stop"), sc,
                    {QStringLiteral("stop"), QStringLiteral("WaaSMedicSvc")}, 60000, true);
        // And take away the launchers: with the tasks disabled, nothing is
        // left that starts the services on its own.
        for (const QString& task : kUpdateTasks) {
            steps << mk(QStringLiteral("sysopt.step.upd_task_off"), schtasks,
                        {QStringLiteral("/Change"), QStringLiteral("/TN"), task,
                         QStringLiteral("/Disable")}, 30000, true);
        }
    } else {
        steps << mk(QStringLiteral("sysopt.step.upd_restore_policy"), reg,
                    {QStringLiteral("delete"), kAU, QStringLiteral("/v"), QStringLiteral("NoAutoUpdate"),
                     QStringLiteral("/f")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_restore_policy"), reg,
                    {QStringLiteral("delete"), kWU, QStringLiteral("/v"),
                     QStringLiteral("DoNotConnectToWindowsUpdateInternetLocations"),
                     QStringLiteral("/f")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_restore_policy"), reg,
                    {QStringLiteral("delete"), kWU, QStringLiteral("/v"),
                     QStringLiteral("DisableWindowsUpdateAccess"), QStringLiteral("/f")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_restore_policy"), reg,
                    {QStringLiteral("delete"), kWU, QStringLiteral("/v"),
                     QStringLiteral("SetDisableUXWUAccess"), QStringLiteral("/f")}, 30000, true);
        // The services' security descriptors were repaired above, before the
        // branch — the "sc config" steps here can assume a writable service.
        steps << mk(QStringLiteral("sysopt.step.upd_enable"), sc,
                    {QStringLiteral("config"), QStringLiteral("WaaSMedicSvc"), QStringLiteral("start="),
                     QStringLiteral("demand")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_enable"), sc,
                    {QStringLiteral("config"), QStringLiteral("UsoSvc"), QStringLiteral("start="),
                     QStringLiteral("auto")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_enable"), sc,
                    {QStringLiteral("config"), QStringLiteral("wuauserv"), QStringLiteral("start="),
                     QStringLiteral("demand")}, 30000, true);
        // Mirror of the disable-side registry fallback: the Start value is
        // what actually persists, and for WaaSMedicSvc it is the only path
        // that works at all (its DACL refuses SERVICE_CHANGE_CONFIG).
        {
            const wchar_t* names[] = {L"wuauserv", L"UsoSvc", L"WaaSMedicSvc"};
            const wchar_t* starts[] = {L"3", L"2", L"3"};   // demand / auto / demand
            for (int i = 0; i < 3; ++i) {
                steps << mk(QStringLiteral("sysopt.step.upd_enable"), reg,
                            {QStringLiteral("add"), kSvcKey + QLatin1Char('\\') + QString::fromWCharArray(names[i]),
                             QStringLiteral("/v"), QStringLiteral("Start"),
                             QStringLiteral("/t"), QStringLiteral("REG_DWORD"), QStringLiteral("/d"),
                             QString::fromWCharArray(starts[i]), QStringLiteral("/f")}, 30000, true);
            }
        }
        // The task launchers disabled on the way down come back on here; the
        // update stack only works end-to-end with them armed again.
        for (const QString& task : kUpdateTasks) {
            steps << mk(QStringLiteral("sysopt.step.upd_task_on"), schtasks,
                        {QStringLiteral("/Change"), QStringLiteral("/TN"), task,
                         QStringLiteral("/Enable")}, 30000, true);
        }
        steps << mk(QStringLiteral("sysopt.step.upd_start"), sc,
                    {QStringLiteral("start"), QStringLiteral("wuauserv")}, 60000, true);
    }

    // The blocks that only ever come from a third-party tool are removed only
    // when the scan actually found them: there is no reason to touch a value
    // that is not there, and every extra command is one more thing that can
    // fail on somebody's machine. Both directions need them — turning updates
    // on while the hosts file still redirects Microsoft's update servers, for
    // example, would leave the user with a switch that says on and updates
    // that still do not work.
    if (st.hostsBlocked) {
        // Rewrites the file in place, keeping every other line and leaving a
        // backup next to it, because the hosts file is not ours to lose.
        steps << mk(QStringLiteral("sysopt.step.upd_hosts"), ps,
                    {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                     QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                     QStringLiteral("-Command"),
                     QStringLiteral("$p = Join-Path $env:SystemRoot 'System32\\drivers\\etc\\hosts'; "
                                    "Copy-Item -LiteralPath $p -Destination ($p + '.ncduwin-bak') -Force "
                                    "-ErrorAction SilentlyContinue; "
                                    "(Get-Content -LiteralPath $p) | Where-Object { $_ -notmatch "
                                    "'windowsupdate|update\\.microsoft' } | "
                                    "Set-Content -LiteralPath $p -Encoding ASCII")},
                    60000);
    }
    if (st.wsusRedirected) {
        steps << mk(QStringLiteral("sysopt.step.upd_wsus"), reg,
                    {QStringLiteral("delete"), kWU, QStringLiteral("/v"), QStringLiteral("WUServer"),
                     QStringLiteral("/f")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_wsus"), reg,
                    {QStringLiteral("delete"), kWU, QStringLiteral("/v"),
                     QStringLiteral("WUStatusServer"), QStringLiteral("/f")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_wsus"), reg,
                    {QStringLiteral("delete"), kAU, QStringLiteral("/v"), QStringLiteral("UseWUServer"),
                     QStringLiteral("/f")}, 30000, true);
    }
    if (st.neverCheck) {
        steps << mk(QStringLiteral("sysopt.step.upd_legacy"), reg,
                    {QStringLiteral("delete"), kLegacy, QStringLiteral("/v"), QStringLiteral("AUOptions"),
                     QStringLiteral("/f")}, 30000, true);
    }
    if (st.legacyNoAuto) {
        steps << mk(QStringLiteral("sysopt.step.upd_legacy"), reg,
                    {QStringLiteral("delete"), kLegacy, QStringLiteral("/v"),
                     QStringLiteral("NoAutoUpdate"), QStringLiteral("/f")}, 30000, true);
    }
    if (st.bitsDisabled) {
        // BITS ships as "automatic (delayed start)" — restoring it to demand
        // would leave downloads stalled on the next boot.
        steps << mk(QStringLiteral("sysopt.step.upd_enable"), sc,
                    {QStringLiteral("config"), QStringLiteral("BITS"), QStringLiteral("start="),
                     QStringLiteral("delayed-auto")}, 30000, true);
        steps << mk(QStringLiteral("sysopt.step.upd_start"), sc,
                    {QStringLiteral("start"), QStringLiteral("BITS")}, 60000, true);
    }
    return steps;
}

void SystemOptPanel::onNetEmergencyFix()
{
    if (isBusy())
        return;

    rememberScroll();
    const bool confirmed = Dialogs::confirm(this, I18n::tr("sysopt.netfix.title"),
                                           I18n::tr("sysopt.netfix.confirm"));
    restoreScroll();
    if (!confirmed)
        return;

#ifdef _WIN32
    const QString netsh = systemExe(QStringLiteral("netsh.exe"));
    const QString ipconfig = systemExe(QStringLiteral("ipconfig.exe"));
    const QString ps = powerShellExe();

    const auto mk = [](const QString& key, const QString& prog, const QStringList& args,
                       int timeoutMs, bool allowFailure = false,
                       const QStringList& markers = {}) {
        OptStep s;
        s.labelKey = key;
        s.program = prog;
        s.args = args;
        s.timeoutMs = timeoutMs;
        s.allowFailure = allowFailure;
        s.successMarkers = markers;
        return s;
    };

    // Restoring DNS/IP to automatic is the part that actually fixes a machine
    // that was left without a network after someone pointed it at a custom DNS
    // server, so it runs before the IP is re-requested.
    QList<OptStep> steps;
    steps << mk(QStringLiteral("sysopt.step.winsock"), netsh,
                {QStringLiteral("winsock"), QStringLiteral("reset")}, 60000,
                false, kNetshResetSuccessMarkers);
    steps << mk(QStringLiteral("sysopt.step.ipreset"), netsh,
                {QStringLiteral("int"), QStringLiteral("ip"), QStringLiteral("reset")}, 60000,
                false, kNetshResetSuccessMarkers);
    steps << mk(QStringLiteral("sysopt.step.ipv6reset"), netsh,
                {QStringLiteral("int"), QStringLiteral("ipv6"), QStringLiteral("reset")}, 60000,
                false, kNetshResetSuccessMarkers);
    steps << mk(QStringLiteral("sysopt.step.arp"), netsh,
                {QStringLiteral("interface"), QStringLiteral("ip"),
                 QStringLiteral("delete"), QStringLiteral("arpcache")}, 30000, true);
    steps << mk(QStringLiteral("sysopt.step.firewall"), netsh,
                {QStringLiteral("advfirewall"), QStringLiteral("reset")}, 60000);
    steps << mk(QStringLiteral("sysopt.step.dnsauto"), ps,
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                 QStringLiteral("-Command"),
                 QStringLiteral("Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | "
                                "Set-DnsClientServerAddress -ResetServerAddresses")}, 90000);
    steps << mk(QStringLiteral("sysopt.step.ipauto"), ps,
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                 QStringLiteral("-Command"),
                 QStringLiteral("Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object {"
                                " Set-NetIPInterface -InterfaceIndex $_.ifIndex -Dhcp Enabled;"
                                " Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4"
                                " -PrefixOrigin Manual -ErrorAction SilentlyContinue |"
                                " Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue }")},
                120000);
    steps << mk(QStringLiteral("sysopt.step.flushdns"), ipconfig,
                {QStringLiteral("/flushdns")}, 30000);
    steps << mk(QStringLiteral("sysopt.step.renew"), ipconfig,
                {QStringLiteral("/renew")}, 180000, true);

    startSteps(I18n::tr("sysopt.netfix.title"), steps, m_statusNet, m_btnNet,
               QStringLiteral("sysopt.netfix.btn"));
#endif
}

void SystemOptPanel::onFlushDns()
{
    if (isBusy())
        return;

    rememberScroll();
    const bool confirmed = Dialogs::confirm(this, I18n::tr("sysopt.dns.title"),
                                           I18n::tr("sysopt.dns.confirm"));
    restoreScroll();
    if (!confirmed)
        return;

#ifdef _WIN32
    // dnsapi!DnsFlushResolverCache is what "ipconfig /flushdns" calls internally,
    // so use it directly: it needs no elevation, spawns no window and returns a
    // real success flag.
    typedef BOOL(WINAPI * DnsFlushResolverCacheFn)();
    HMODULE hDnsApi = LoadLibraryW(L"dnsapi.dll");
    if (hDnsApi) {
        auto fn = reinterpret_cast<DnsFlushResolverCacheFn>(GetProcAddress(hDnsApi, "DnsFlushResolverCache"));
        if (fn)
            fn();
        FreeLibrary(hDnsApi);
    }

    // A machine with a hand-written DNS server keeps the cached entries of that
    // server, so flushing alone is not enough: put DNS and the IP configuration
    // back on automatic as well.
    const QString ps = powerShellExe();
    const auto mk = [](const QString& key, const QString& prog, const QStringList& args,
                       int timeoutMs, bool allowFailure = false) {
        OptStep s;
        s.labelKey = key;
        s.program = prog;
        s.args = args;
        s.timeoutMs = timeoutMs;
        s.allowFailure = allowFailure;
        return s;
    };

    QList<OptStep> steps;
    steps << mk(QStringLiteral("sysopt.step.dnsauto"), ps,
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                 QStringLiteral("-Command"),
                 QStringLiteral("Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | "
                                "Set-DnsClientServerAddress -ResetServerAddresses")}, 90000);
    steps << mk(QStringLiteral("sysopt.step.ipauto"), ps,
                {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                 QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                 QStringLiteral("-Command"),
                 QStringLiteral("Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object {"
                                " Set-NetIPInterface -InterfaceIndex $_.ifIndex -Dhcp Enabled;"
                                " Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4"
                                " -PrefixOrigin Manual -ErrorAction SilentlyContinue |"
                                " Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue }")},
                120000);
    steps << mk(QStringLiteral("sysopt.step.flushdns"), systemExe(QStringLiteral("ipconfig.exe")),
                {QStringLiteral("/flushdns")}, 30000);

    startSteps(I18n::tr("sysopt.dns.title"), steps, m_statusDns, m_btnDns,
               QStringLiteral("sysopt.dns.btn"));
#endif
}

void SystemOptPanel::onResetStore()
{
    if (isBusy())
        return;

    rememberScroll();
    const bool confirmed = Dialogs::confirm(this, I18n::tr("sysopt.store.title"),
                                           I18n::tr("sysopt.store.confirm"));
    restoreScroll();
    if (!confirmed)
        return;

#ifdef _WIN32
    const QString wsreset = systemExe(QStringLiteral("wsreset.exe"));
    if (!QFileInfo::exists(wsreset)) {
        setStatus(m_statusStore, I18n::tr("sysopt.op_failed"), C::DANGER());
        return;
    }

    // Started detached: wsreset brings up the Store itself, so the app only
    // reports that the reset was dispatched instead of waiting on it.
    if (QProcess::startDetached(wsreset, QStringList())) {
        setStatus(m_statusStore, I18n::tr("sysopt.store.launched"), C::PRIMARY());
    } else {
        setStatus(m_statusStore, I18n::tr("sysopt.op_failed"), C::DANGER());
    }
#endif
}

void SystemOptPanel::onOpenAppSync()
{
    emit requestAppPathSync();
}

void SystemOptPanel::startSteps(const QString& jobName, const QList<OptStep>& steps,
                                QLabel* status, QPushButton* btn, const QString& btnKey)
{
#ifdef _WIN32
    if (isBusy() || steps.isEmpty())
        return;

    m_steps = steps;
    m_stepIndex = 0;
    m_stepErrors.clear();
    m_rebootNeeded = false;
    m_jobStatus = status;
    m_jobButton = btn;
    m_jobBtnKey = btnKey;
    m_jobName = jobName;
    m_jobActive = true;
    m_jobTouchesUpdate = false;
    m_autoRepair = false;

    setActionsEnabled(false);
    if (btn)
        btn->setText(I18n::tr("sysopt.running"));
    runNextStep();
#endif
}

void SystemOptPanel::runNextStep()
{
#ifdef _WIN32
    if (m_stepIndex >= m_steps.size()) {
        finishSteps();
        return;
    }

    const OptStep& step = m_steps.at(m_stepIndex);
    setStatus(m_jobStatus,
              I18n::tr("sysopt.step_progress", QMap<QString, QString>{
                  {"current", QString::number(m_stepIndex + 1)},
                  {"total", QString::number(m_steps.size())},
                  {"step", I18n::tr(step.labelKey)}}),
              C::PRIMARY());

    m_proc->setProgram(step.program);
    m_proc->setArguments(step.args);
    m_proc->start();
    m_stepTimer->start(step.timeoutMs);
#endif
}

void SystemOptPanel::finishSteps()
{
    const int total = m_steps.size();
    const int failed = m_stepErrors.size();
    QLabel* status = m_jobStatus;
    QPushButton* btn = m_jobButton;
    const QString btnKey = m_jobBtnKey;
    const bool touchesUpdate = m_jobTouchesUpdate;
    const bool rebootNeeded = m_rebootNeeded || m_sddlResetReboot;
    const bool sddlResetReboot = m_sddlResetReboot;

    m_steps.clear();
    m_stepIndex = 0;
    m_jobStatus = nullptr;
    m_jobButton = nullptr;
    m_jobBtnKey.clear();
    m_jobActive = false;
    m_rebootNeeded = false;
    m_sddlResetReboot = false;

    if (btn) {
        btn->setEnabled(true);
        btn->setText(I18n::tr(btnKey));
    }
    setActionsEnabled(true);

    if (failed == 0) {
        // netsh ip/ipv6/winsock reset only take effect after a restart; saying
        // so here is what stops the user thinking the fix did nothing.
        setStatus(status, I18n::tr(rebootNeeded ? "sysopt.task_done_reboot"
                                                : "sysopt.task_done",
                                   QMap<QString, QString>{
                                       {"total", QString::number(total)}}),
                  C::SUCCESS());
    } else {
        setStatus(status, I18n::tr("sysopt.task_partial", QMap<QString, QString>{
                                {"failed", QString::number(failed)},
                                {"total", QString::number(total)},
                                {"steps", m_stepErrors.join(QStringLiteral("、"))}}),
                  C::DANGER());
    }
    m_stepErrors.clear();

    if (touchesUpdate) {
        // Re-read the real system state; only report the plain status when every
        // step went through, otherwise keep the failure detail on screen.
        const QString detail = status ? status->text() : QString();
        checkAutoUpdateStatus();
        if (m_autoRepair) {
            // A repair the user never clicked: it has to say what it found and
            // what it did, or the settings changing by themselves look like a
            // fault of this app.
            const QString reasons = m_autoRepairReasons.join(QStringLiteral("、"));
            const bool ok = (failed == 0);
            setStatus(status,
                      I18n::tr(ok ? "sysopt.update.repaired" : "sysopt.update.repair_failed",
                               QMap<QString, QString>{{"reasons", reasons}}),
                      ok ? C::SUCCESS() : C::DANGER());
            Logger::info(QStringLiteral("[sysopt] auto-update repair %1 (%2)")
                             .arg(ok ? QStringLiteral("succeeded") : QStringLiteral("failed"),
                                  reasons));
            m_autoRepair = false;
            m_autoRepairReasons.clear();
        } else if (failed > 0 && status) {
            setStatus(status, detail, C::DANGER());
        }
        // The descriptor could only be reset through its registry blob, which
        // applies at the next boot: say so, or the user will click the switch
        // again and watch it do nothing until then.
        if (sddlResetReboot && status) {
            const QString base = status->text();
            setStatus(status, base + QStringLiteral("\n") + I18n::tr("sysopt.update.reboot_hint"),
                      C::DANGER());
        }
    }

    // The status label may have just grown by a line; keep the list still.
    restoreScroll();
}

void SystemOptPanel::retranslate()
{
    m_headerTitle->setText(I18n::tr("sysopt.title"));
    m_headerDesc->setText(I18n::tr("sysopt.desc"));

    m_titleUpdate->setText(I18n::tr("sysopt.update.title"));
    m_descUpdate->setText(I18n::tr("sysopt.update.desc"));
    checkAutoUpdateStatus();

    m_titleNet->setText(I18n::tr("sysopt.netfix.title"));
    m_descNet->setText(I18n::tr("sysopt.netfix.desc"));
    m_btnNet->setText(I18n::tr("sysopt.netfix.btn"));

    m_titleDns->setText(I18n::tr("sysopt.dns.title"));
    m_descDns->setText(I18n::tr("sysopt.dns.desc"));
    m_btnDns->setText(I18n::tr("sysopt.dns.btn"));

    m_titleStore->setText(I18n::tr("sysopt.store.title"));
    m_descStore->setText(I18n::tr("sysopt.store.desc"));
    m_btnStore->setText(I18n::tr("sysopt.store.btn"));

    m_titleSync->setText(I18n::tr("sysopt.sync.title"));
    m_descSync->setText(I18n::tr("sysopt.sync.desc"));
    m_btnSync->setText(I18n::tr("sysopt.sync.btn"));
    if (m_badgeSync) {
        m_badgeSync->setText(I18n::tr("sysopt.sync.badge"));
        m_badgeSync->setVisible(true);
    }
}

void SystemOptPanel::refreshTheme()
{
    m_headerTitle->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 700; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_headerDesc->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
        .arg(QString::fromLatin1(C::TEXT_MUTED())));

    auto updateCardStyle = [](QFrame* card) {
        if (!card) return;
        card->setStyleSheet(QStringLiteral(
            "QFrame#optCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 10px;"
            "}"
            "QFrame#optCard:hover {"
            "  border-color: %3;"
            "}"
        ).arg(QString::fromLatin1(C::SURFACE()),
              QString::fromLatin1(C::BORDER()),
              QString::fromLatin1(C::PRIMARY())));
    };

    updateCardStyle(m_cardUpdate);
    updateCardStyle(m_cardNet);
    updateCardStyle(m_cardDns);
    updateCardStyle(m_cardStore);
    updateCardStyle(m_cardSync);

    m_titleUpdate->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_descUpdate->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.4;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));

    m_titleNet->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_descNet->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.4;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));

    m_titleDns->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_descDns->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.4;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));

    m_titleStore->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_descStore->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.4;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));

    m_titleSync->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;")
        .arg(QString::fromLatin1(C::FG())));
    m_descSync->setStyleSheet(QStringLiteral("font-size: 12px; color: %1; line-height: 1.4;")
        .arg(QString::fromLatin1(C::TEXT_SEC())));
    if (m_badgeSync)
        m_badgeSync->setStyleSheet(QStringLiteral(
            "QLabel#cardBadge {"
            "  color: #FFFFFF;"
            "  background-color: %1;"
            "  font-size: 10px;"
            "  font-weight: 600;"
            "  border-radius: 7px;"
            "  padding: 2px 8px;"
            "}").arg(QString::fromLatin1(C::PRIMARY())));

    if (m_switchUpdate)
        m_switchUpdate->refreshTheme();

    checkAutoUpdateStatus();
}