#include "app/remote/remote_groups.h"

#include "app/app_settings.h"
#include "app/remote/remote_hub.h"
#include "diagnostics/diagnostics_log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QProcess>
#include <QUrlQuery>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <psapi.h>
#include <sddl.h>
#include <userenv.h>

#include <memory>
#include <vector>
#endif

namespace forevertas::app::remote {

namespace {

QString RandomHex(int bytes) {
    QByteArray data(bytes, Qt::Uninitialized);
    for (char &byte : data) byte = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(data.toHex());
}

}  // namespace

Member CurrentMember() {
    AppSettings &settings = AppSettings::Instance();
    if (settings.value(QStringLiteral("remote/memberId")).toString().isEmpty()) {
        settings.setValue(QStringLiteral("remote/memberId"), RandomHex(16));
        settings.setValue(QStringLiteral("remote/memberSecret"), RandomHex(24));
    }
    return {settings.value(QStringLiteral("remote/memberId")).toString(),
            settings.value(QStringLiteral("remote/memberSecret")).toString(),
            settings.value(QStringLiteral("remote/memberName")).toString()};
}

void SetMemberName(const QString &name) {
    AppSettings::Instance().setValue(QStringLiteral("remote/memberName"), name.trimmed().left(40));
}

QString GroupOfInvite(const QString &invite) {
    const QString text = invite.trimmed();
    if (text.compare(QLatin1String("public"), Qt::CaseInsensitive) == 0) return QStringLiteral("public");
    static const QRegularExpression pattern(QStringLiteral("^([A-Za-z2-9]{8})-[0-9A-Fa-f]{12}$"));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? match.captured(1).toUpper() : QString();
}

QUrl GroupUrl(const QString &group, const QString &action, const Member &member) {
    const QUrl relay = RelayUrl();
    QUrl url = relay.resolved(QUrl(QStringLiteral("/groups/%1/%2").arg(group, action)));
    if (action == QLatin1String("worker") || action == QLatin1String("client") ||
        action == QLatin1String("status")) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("member"), member.id);
        query.addQueryItem(QStringLiteral("secret"), member.secret);
        url.setQuery(query);
    }
    if (action == QLatin1String("worker") || action == QLatin1String("client"))
        url.setScheme(relay.scheme() == QLatin1String("http") ? QStringLiteral("ws") : QStringLiteral("wss"));
    return url;
}

LendCommand LenderCommand(const QString &app, const QString &packs, const QStringList &readOnly,
                          const QStringList &lends) {
    LendCommand command{app, QStringList{QStringLiteral("--worker"), QStringLiteral("lent"), QStringLiteral("--packs"), packs} + lends,
                        false};
#ifdef Q_OS_LINUX
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (bwrap.isEmpty() || !qEnvironmentVariable("FOREVERTAS_LEND_UNSANDBOXED").isEmpty()) return command;
    QStringList sandbox{QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
                        QStringLiteral("--dev-bind"), QStringLiteral("/dev"), QStringLiteral("/dev"),
                        QStringLiteral("--proc"), QStringLiteral("/proc"),
                        QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                        QStringLiteral("--tmpfs"), QDir::homePath()};
    // what the app runs with, wherever it is (a build's Qt in the home folder, an AppImage's mount in /tmp): the
    // folders of the libraries it has loaded, and Qt's own
    QStringList kept = QStringList{packs} + readOnly;
    kept << QLibraryInfo::path(QLibraryInfo::LibrariesPath) << QLibraryInfo::path(QLibraryInfo::PluginsPath);
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (maps.open(QIODevice::ReadOnly)) {
        const QString home = QDir::homePath() + QLatin1Char('/');
        for (const QByteArray &line : maps.readAll().split('\n')) {
            const qsizetype slash = line.indexOf(" /");
            if (slash < 0) continue;
            const QString file = QString::fromLocal8Bit(line.mid(slash + 1).trimmed());
            if (file.startsWith(home) || file.startsWith(QLatin1String("/tmp/")))
                kept << QFileInfo(file).absolutePath();
        }
    }
    kept.removeDuplicates();
    for (const QString &folder : kept)
        if (!folder.isEmpty()) sandbox << QStringLiteral("--ro-bind-try") << folder << folder;
    sandbox << QStringLiteral("--unshare-all") << QStringLiteral("--share-net") << QStringLiteral("--die-with-parent")
            << QStringLiteral("--new-session") << app;
    command.arguments = sandbox + command.arguments;
    command.program = bwrap;
    command.sandboxed = true;
#else
    Q_UNUSED(readOnly);   // (no sandbox here)
#endif
    return command;
}

#if defined(Q_OS_WIN)
namespace {

// What the sandbox needs while its process is made (CreateProcess reads it).
struct AppContainerLaunch {
    PSID container = nullptr;
    BYTE capabilitySids[3][SECURITY_MAX_SID_SIZE] = {};
    SID_AND_ATTRIBUTES capabilities[3]{};
    SECURITY_CAPABILITIES security{};
    std::vector<char> attributes;
    STARTUPINFOEXW info{};
    ~AppContainerLaunch() {
        if (!attributes.empty())
            DeleteProcThreadAttributeList(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data()));
        if (container != nullptr) FreeSid(container);
    }
};

// The AppContainer may read (and run) a folder and what is in it.
bool GrantRead(const QString &folder, PSID container) {
    const std::wstring path = QDir::toNativeSeparators(QFileInfo(folder).absoluteFilePath()).toStdWString();
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr,
                              &descriptor) != ERROR_SUCCESS)
        return false;
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(container);
    PACL updated = nullptr;
    const bool ok = SetEntriesInAclW(1, &access, current, &updated) == ERROR_SUCCESS &&
                    SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                          nullptr, nullptr, updated, nullptr) == ERROR_SUCCESS;
    if (updated != nullptr) LocalFree(updated);
    LocalFree(descriptor);
    return ok;
}

// The AppContainer may use this session's window station and desktop (Qt opens a hidden message window even
// without a user interface; a desktop not shared with apps refuses it, and its libraries fail to start).
void GrantWindowStation(PSID container) {
    const auto grant = [container](HANDLE object, DWORD rights) {
        PACL current = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (GetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr,
                            &descriptor) != ERROR_SUCCESS)
            return;
        EXPLICIT_ACCESS_W access{};
        access.grfAccessPermissions = rights;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(container);
        PACL updated = nullptr;
        if (SetEntriesInAclW(1, &access, current, &updated) == ERROR_SUCCESS)
            SetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
        if (updated != nullptr) LocalFree(updated);
        LocalFree(descriptor);
    };
    wchar_t name[256] = {};
    DWORD needed = 0;
    if (GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, name, sizeof name, &needed)) {
        if (HWINSTA station = OpenWindowStationW(name, FALSE, READ_CONTROL | WRITE_DAC)) {
            grant(station, GENERIC_READ | GENERIC_EXECUTE);
            CloseWindowStation(station);
        }
    }
    if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof name, &needed)) {
        if (HDESK desktop = OpenDesktopW(name, 0, FALSE, READ_CONTROL | WRITE_DAC)) {
            grant(desktop, GENERIC_ALL);   // (its hidden window: where Qt's network events arrive)
            CloseDesktop(desktop);
        }
    }
}

// The folders of the libraries this app runs with (outside Windows' own).
QStringList LoadedLibraryFolders() {
    QStringList folders;
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &bytes)) return folders;
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const QString system = QDir::fromNativeSeparators(QString::fromWCharArray(windows)).toLower();
    for (DWORD index = 0; index < bytes / sizeof(HMODULE); ++index) {
        wchar_t name[MAX_PATH] = {};
        if (GetModuleFileNameW(modules[index], name, MAX_PATH) == 0) continue;
        const QString folder = QFileInfo(QDir::fromNativeSeparators(QString::fromWCharArray(name))).absolutePath();
        if (!folder.toLower().startsWith(system)) folders << folder;
    }
    folders.removeDuplicates();
    return folders;
}

}  // namespace

bool SandboxLender(QProcess *process, QStringList readable, QString *why) {
    auto launch = std::make_shared<AppContainerLaunch>();
    const wchar_t *name = L"ForeverTAS.Lending";
    // its network: the internet (the relay); the firewall reads it from the profile, so the profile carries it
    const WELL_KNOWN_SID_TYPE wanted[] = {WinCapabilityInternetClientSid, WinCapabilityInternetClientServerSid,
                                          WinCapabilityPrivateNetworkClientServerSid};
    for (int index = 0; index < 3; ++index) {
        DWORD size = SECURITY_MAX_SID_SIZE;
        if (!CreateWellKnownSid(wanted[index], nullptr, launch->capabilitySids[index], &size)) {
            *why = QStringLiteral("no network capability");
            return false;
        }
        launch->capabilities[index] = {launch->capabilitySids[index], SE_GROUP_ENABLED};
    }
    HRESULT result = CreateAppContainerProfile(name, L"ForeverTAS lending", L"ForeverTAS searching for a group",
                                               launch->capabilities, 3, &launch->container);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        result = DeriveAppContainerSidFromAppContainerName(name, &launch->container);
    if (FAILED(result)) {
        *why = QStringLiteral("no AppContainer (0x%1)").arg(static_cast<unsigned long>(result), 8, 16, QLatin1Char('0'));
        return false;
    }
    launch->security = {launch->container, launch->capabilities, 3, 0};
    GrantWindowStation(launch->container);
    readable << LoadedLibraryFolders();
    readable.removeDuplicates();
    for (const QString &folder : readable)
        if (!folder.isEmpty() && QFileInfo(folder).exists() && !GrantRead(folder, launch->container))
            diagnostics::Log("lending: could not let the sandbox read %s", qUtf8Printable(folder));
    // its own temporary folder (the replay it is sent goes there)
    LPWSTR sid = nullptr;
    QString temporary;
    if (ConvertSidToStringSidW(launch->container, &sid)) {
        PWSTR folder = nullptr;
        if (SUCCEEDED(GetAppContainerFolderPath(sid, &folder))) {
            temporary = QDir::fromNativeSeparators(QString::fromWCharArray(folder)) + QStringLiteral("/Temp");
            CoTaskMemFree(folder);
        }
        LocalFree(sid);
    }
    if (temporary.isEmpty() || !QDir().mkpath(temporary)) {
        *why = QStringLiteral("no folder for the sandbox");
        return false;
    }
    QProcessEnvironment environment = process->processEnvironment();
    if (environment.isEmpty()) environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TEMP"), QDir::toNativeSeparators(temporary));
    environment.insert(QStringLiteral("TMP"), QDir::toNativeSeparators(temporary));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(temporary);   // (it may open nothing else: not even a share it was started from)
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    launch->attributes.resize(bytes);
    auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(launch->attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &launch->security,
                                   sizeof launch->security, nullptr, nullptr)) {
        launch->attributes.clear();
        *why = QStringLiteral("no sandbox attributes");
        return false;
    }
    process->setCreateProcessArgumentsModifier([launch, list](QProcess::CreateProcessArguments *arguments) {
        launch->info.StartupInfo = *arguments->startupInfo;
        launch->info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        launch->info.lpAttributeList = list;
        arguments->startupInfo = &launch->info.StartupInfo;
        arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });
    return true;
}
#else
bool SandboxLender(QProcess *, QStringList, QString *why) {
    *why = QStringLiteral("only on Windows");
    return false;
}
#endif

void GroupRequest(QNetworkAccessManager *network, const QString &group, const QString &action,
                  const Member &member, QJsonObject body, GroupReply reply) {
    QNetworkReply *pending = nullptr;
    if (action == QLatin1String("status")) {
        pending = network->get(QNetworkRequest(GroupUrl(group, action, member)));
    } else {
        body.insert(QStringLiteral("member"), member.id);
        body.insert(QStringLiteral("secret"), member.secret);
        body.insert(QStringLiteral("memberName"), member.name);
        QNetworkRequest request(group.isEmpty() ? RelayUrl().resolved(QUrl(QStringLiteral("/groups")))
                                                : GroupUrl(group, action, member));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        pending = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    QObject::connect(pending, &QNetworkReply::finished, pending, [pending, reply] {
        pending->deleteLater();
        const QJsonObject answer = QJsonDocument::fromJson(pending->readAll()).object();
        const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200) {
            reply(answer, QString());
        } else {
            const QString why = answer.value(QStringLiteral("error")).toString();
            reply(answer, !why.isEmpty() ? why : pending->errorString());
        }
    });
}

}  // namespace forevertas::app::remote
