#include "app/remote/remote_search.h"

#include "app/app_settings.h"
#include "app/compute_modules.h"
#include "app/remote/remote_groups.h"
#include "app/remote/remote_worker.h"
#include "diagnostics/diagnostics_log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QSysInfo>

#include <algorithm>
#include <mutex>
#include <thread>

namespace forevertas::app::remote {

namespace {

// A group's hub lives as long as the window (a search may be running on it when its group is left or unticked).
std::mutex groupHubsLock;
std::map<QString, std::unique_ptr<RemoteHub>> groupHubs;

RemoteHub *GroupHub(const QString &id, bool make) {
    std::lock_guard<std::mutex> guard(groupHubsLock);
    std::unique_ptr<RemoteHub> &hub = groupHubs[id];
    if (!hub && make) hub = std::make_unique<RemoteHub>();
    return hub.get();
}

const QString kGroupDevice = QStringLiteral("group:");

}  // namespace

RemoteSearch::RemoteSearch(QObject *parent) : QObject(parent) {
    ComputeModules::Instance().SetRemoteRunner([this](const SearchRequest &request, const SearchRunControl *control) {
        const QString device = QString::fromStdString(request.remoteDevice);
        if (device.startsWith(kGroupDevice)) {
            RemoteHub *const hub = GroupHub(device.mid(kGroupDevice.size()), false);
            if (hub == nullptr) throw std::runtime_error("remote: that group is not connected");
            return hub->runOnGroup(request, control);
        }
        return hub_.runOnWorker(device, request, control);
    });
    connect(&hub_, &RemoteHub::roomChanged, this, [this](const QString &code) {
        if (code.isEmpty()) {
            open_ = false;
            status_ = tr("The room closed.");
        } else {
            status_ = tr("Machines join with the code %1.").arg(code);
        }
        emit changed();
        emit devicesChanged();
    });
    connect(&hub_, &RemoteHub::roomFailed, this, [this](const QString &why) {
        open_ = false;
        status_ = tr("No room: %1").arg(why);
        emit changed();
    });
    connect(&hub_, &RemoteHub::workersChanged, this, [this] {
        emit changed();
        emit devicesChanged();
    });
    load();
    statusTimer_.setInterval(60 * 1000);
    connect(&statusTimer_, &QTimer::timeout, this, &RemoteSearch::refreshGroups);
    statusTimer_.start();
    QTimer::singleShot(0, this, [this] {
        for (auto &[id, group] : groups_) updateGroup(group);
        refreshGroups();
    });
}

RemoteSearch::~RemoteSearch() {
    if (QProcess *const process = lender_) {
        stopLender();
        process->waitForFinished(2000);
    }
    // (before the application: their sockets are its objects)
    std::lock_guard<std::mutex> guard(groupHubsLock);
    groupHubs.clear();
}

void RemoteSearch::setOpen(bool open) {
    if (open == open_) return;
    open_ = open;
    if (open) {
        status_ = tr("Opening a room...");
        hub_.openRoom();
    } else {
        hub_.closeRoom();
        status_.clear();
    }
    emit changed();
}

QVariantList RemoteSearch::workers() const {
    // one entry a thing a machine lends: each GPU, its CPU (key: machine/backend)
    QVariantList list;
    for (const QString &id : hub_.workers()) {
        const QJsonObject hello = hub_.workerHello(id);
        for (const QJsonValue &value : hello.value("gpus").toArray()) {
            const QJsonObject gpu = value.toObject();
            const QString key = id + QLatin1Char('/') + gpu.value("backend").toString();
            list.append(QVariantMap{{"id", id}, {"key", key}, {"name", gpu.value("name").toString()},
                                    {"backend", gpu.value("backend").toString()}, {"threads", 0},
                                    {"used", !unused_.contains(key)}});
        }
        const QJsonObject cpu = hello.value("cpu").toObject();
        if (cpu.value("threads").toInt() > 0) {
            const QString key = id + QStringLiteral("/cpu");
            list.append(QVariantMap{{"id", id}, {"key", key}, {"name", cpu.value("name").toString()},
                                    {"backend", QStringLiteral("cpu")}, {"threads", cpu.value("threads").toInt()},
                                    {"used", !unused_.contains(key)}});
        }
    }
    return list;
}

QString RemoteSearch::workerCommand() const {
    const QString code = hub_.roomCode();
    return code.isEmpty() ? QString()
                          : QStringLiteral("ForeverTAS --worker %1 --packs /path/to/TmForever/Packs").arg(code);
}

void RemoteSearch::setUsed(const QString &id, bool used) {
    if (used == !unused_.contains(id)) return;
    if (used) {
        unused_.remove(id);
    } else {
        unused_.insert(id);
    }
    emit changed();
    emit devicesChanged();
}

std::vector<SearchDevice> RemoteSearch::devices() const {
    std::vector<SearchDevice> devices;
    for (const QVariant &value : workers()) {
        const QVariantMap worker = value.toMap();
        if (!worker.value("used").toBool()) continue;
        SearchDevice device;
        device.remote = worker.value("id").toString().toStdString();
        const int threads = worker.value("threads").toInt();
        if (threads > 0) {   // its CPU, on every thread it lends
            device.backend = threads > 1 ? PhysicsBackend::MultiThreadedCpu : PhysicsBackend::OptimizedCpu;
            device.parallelSampleCount = static_cast<std::uint32_t>(threads);
        } else {
            const auto backend = ParsePhysicsBackend(worker.value("backend").toString().toStdString());
            if (!backend) continue;
            device.backend = *backend;
            device.calibrate = true;
            device.parallelSampleCount = 1u;
        }
        devices.push_back(device);
    }
    // the groups searched with: each one device, whatever machines it is given
    for (const auto &[id, group] : groups_) {
        if (!group.search) continue;
        const RemoteHub *const hub = GroupHub(id, false);
        if (hub == nullptr || !hub->groupOpen()) continue;
        SearchDevice device;
        device.backend = PhysicsBackend::Cuda;   // (each machine searches on its own kind of GPU)
        device.calibrate = true;
        device.parallelSampleCount = 1u;
        device.remote = (kGroupDevice + id).toStdString();
        devices.push_back(device);
    }
    return devices;
}

// ---- groups (ForeverTAS@home) ----

QNetworkAccessManager *RemoteSearch::network() {
    if (network_ == nullptr) network_ = new QNetworkAccessManager(this);
    return network_;
}

void RemoteSearch::load() {
    const QJsonArray saved =
            QJsonDocument::fromJson(AppSettings::Instance().value(QStringLiteral("remote/groups")).toByteArray()).array();
    for (const QJsonValue &value : saved) {
        const QJsonObject object = value.toObject();
        Joined group;
        group.id = object.value("id").toString();
        if (group.id.isEmpty()) continue;
        group.name = object.value("name").toString();
        group.invite = object.value("invite").toString();
        group.owner = object.value("owner").toBool();
        group.lend = object.value("lend").toBool();
        group.search = object.value("search").toBool();
        groups_[group.id] = std::move(group);
    }
}

void RemoteSearch::save() const {
    QJsonArray saved;
    for (const auto &[id, group] : groups_)
        saved.append(QJsonObject{{"id", id}, {"name", group.name}, {"invite", group.invite},
                                 {"owner", group.owner}, {"lend", group.lend}, {"search", group.search}});
    AppSettings::Instance().setValue(QStringLiteral("remote/groups"),
                                     QJsonDocument(saved).toJson(QJsonDocument::Compact));
}

QString RemoteSearch::memberName() const {
    const QString name = CurrentMember().name;
    return name.isEmpty() ? QSysInfo::machineHostName() : name;
}

void RemoteSearch::setMemberName(const QString &name) {
    if (name.trimmed() == memberName()) return;
    SetMemberName(name);
    // the groups learn it when this member joins again (a member's join keeps its credit)
    for (const auto &[id, group] : groups_) {
        Member member = CurrentMember();
        member.name = memberName();
        GroupRequest(network(), id, QStringLiteral("join"), member, QJsonObject{{"invite", group.invite}},
                     [](const QJsonObject &, const QString &) {});
    }
    emit groupsChanged();
}

QVariantList RemoteSearch::groups() const {
    QVariantList list;
    for (const auto &[id, group] : groups_) {
        const RemoteHub *const hub = GroupHub(id, false);
        QVariantList credits;
        for (const QJsonValue &value : group.status.value("credits").toArray()) credits.append(value.toObject().toVariantMap());
        list.append(QVariantMap{{"id", id},
                                {"name", group.name},
                                {"invite", id == QLatin1String("public") ? QString() : group.invite},
                                {"owner", group.owner},
                                {"isPublic", id == QLatin1String("public")},
                                {"lend", group.lend},
                                {"search", group.search},
                                {"lending", lender_ != nullptr && lender_->state() == QProcess::Running &&
                                                    lentTo_.contains(id)},
                                {"connected", hub != nullptr && hub->groupOpen()},
                                {"machines", group.status.value("machines").toInt()},
                                {"searches", group.status.value("searches").toInt()},
                                {"yours", group.status.value("yours").toInt()},
                                {"members", group.status.value("members").toInt()},
                                {"credits", credits}});
    }
    return list;
}

void RemoteSearch::joined(const QJsonObject &answer, const QString &invite, bool owner) {
    const QString id = answer.value("id").toString();
    Joined &group = groups_[id];
    group.id = id;
    group.name = answer.value("name").toString();
    if (!invite.isEmpty()) group.invite = invite;
    group.owner = group.owner || owner;
    save();
    groupMessage_ = id == QLatin1String("public") ? tr("You are in the public pool.") : tr("You are in %1.").arg(group.name);
    updateGroup(group);
    refreshGroups();
    emit groupsChanged();
}

void RemoteSearch::createGroup(const QString &name) {
    Member member = CurrentMember();
    member.name = memberName();
    GroupRequest(network(), QString(), QStringLiteral("create"), member, QJsonObject{{"name", name.trimmed()}},
                 [this](const QJsonObject &answer, const QString &error) {
                     if (!error.isEmpty()) {
                         groupMessage_ = tr("No group: %1").arg(error);
                         emit groupsChanged();
                         return;
                     }
                     joined(answer, answer.value("invite").toString(), true);
                 });
}

void RemoteSearch::joinGroup(const QString &invite) {
    const QString id = GroupOfInvite(invite);
    if (id.isEmpty()) {
        groupMessage_ = tr("That is not an invite (it looks like ABCD2345-0123456789AB).");
        emit groupsChanged();
        return;
    }
    Member member = CurrentMember();
    member.name = memberName();
    GroupRequest(network(), id, QStringLiteral("join"), member, QJsonObject{{"invite", invite.trimmed()}},
                 [this, id, invite](QJsonObject answer, const QString &error) {
                     if (!error.isEmpty()) {
                         groupMessage_ = tr("Not joined: %1").arg(error);
                         emit groupsChanged();
                         return;
                     }
                     answer.insert("id", id);
                     joined(answer, id == QLatin1String("public") ? QString() : invite.trimmed(), false);
                 });
}

void RemoteSearch::leaveGroup(const QString &id) {
    const auto found = groups_.find(id);
    if (found == groups_.end()) return;
    if (RemoteHub *const hub = GroupHub(id, false)) hub->closeRoom();
    Member member = CurrentMember();
    GroupRequest(network(), id, QStringLiteral("leave"), member, {}, [](const QJsonObject &, const QString &) {});
    groupMessage_ = tr("You left %1.").arg(found->second.name);
    groups_.erase(found);
    save();
    updateLending();
    emit groupsChanged();
    emit devicesChanged();
}

void RemoteSearch::newInvite(const QString &id) {
    GroupRequest(network(), id, QStringLiteral("invite"), CurrentMember(), {},
                 [this, id](const QJsonObject &answer, const QString &error) {
                     const auto found = groups_.find(id);
                     if (found == groups_.end()) return;
                     if (!error.isEmpty()) {
                         groupMessage_ = tr("No new invite: %1").arg(error);
                     } else {
                         found->second.invite = answer.value("invite").toString();
                         save();
                         groupMessage_ = tr("New invite for %1: the old one no longer works.").arg(found->second.name);
                     }
                     emit groupsChanged();
                 });
}

void RemoteSearch::setLending(const QString &id, bool lend) {
    const auto found = groups_.find(id);
    if (found == groups_.end() || found->second.lend == lend) return;
    found->second.lend = lend;
    save();
    updateGroup(found->second);
    emit groupsChanged();
}

void RemoteSearch::setSearching(const QString &id, bool search) {
    const auto found = groups_.find(id);
    if (found == groups_.end() || found->second.search == search) return;
    found->second.search = search;
    save();
    updateGroup(found->second);
    emit groupsChanged();
    emit devicesChanged();
}

void RemoteSearch::refreshGroups() {
    for (const auto &[id, group] : groups_) {
        GroupRequest(network(), id, QStringLiteral("status"), CurrentMember(), {},
                     [this, id = id](const QJsonObject &answer, const QString &error) {
                         const auto found = groups_.find(id);
                         if (found == groups_.end()) return;
                         if (error.isEmpty()) {
                             found->second.status = answer;
                             if (!answer.value("name").toString().isEmpty()) found->second.name = answer.value("name").toString();
                         }
                         emit groupsChanged();
                     });
    }
}

void RemoteSearch::setPacksDirectory(const QString &directory) {
    if (directory == packsDirectory_) return;
    packsDirectory_ = directory;
    restartLenders();
}

void RemoteSearch::setLocalGpuBusy(bool busy) {
    if (busy == localGpuBusy_) return;
    localGpuBusy_ = busy;
    for (auto &[id, group] : groups_) updateGroup(group);
    emit groupsChanged();
}

void RemoteSearch::dropGroupConnections() {
    for (const auto &[id, group] : groups_)
        if (RemoteHub *const hub = GroupHub(id, false)) hub->closeRoom();
}

bool RemoteSearch::hasGpu() const {
    return ComputeModules::Instance().Status(PhysicsBackend::Cuda).ready ||
           ComputeModules::Instance().Status(PhysicsBackend::Hip).ready;
}

QVariantList RemoteSearch::localGpus() const {
    QVariantList gpus;
    for (const PhysicsBackend backend : {PhysicsBackend::Cuda, PhysicsBackend::Hip}) {
        const compute::GpuStatus status = ComputeModules::Instance().Status(backend);
        if (status.deviceName.empty() && !status.ready) continue;
        gpus.append(QVariantMap{{"name", QString::fromStdString(status.deviceName)},
                                {"backend", backend == PhysicsBackend::Cuda ? QStringLiteral("CUDA") : QStringLiteral("HIP")},
                                {"ready", status.ready},
                                {"status", status.ready ? QString() : QString::fromStdString(status.diagnostic)}});
    }
    return gpus;
}

QString RemoteSearch::cpuName() const {
    return CpuName();
}

QVariantList RemoteSearch::roomMachines() const {
    QVariantList machines;
    std::map<QString, QVariantList> parts;
    for (const QVariant &value : workers()) {
        const QVariantMap entry = value.toMap();
        const int threads = entry.value("threads").toInt();
        parts[entry.value("id").toString()].append(QVariantMap{
                {"key", entry.value("key")},
                {"name", entry.value("name")},
                {"detail", threads == 1 ? tr("1 CPU thread") : threads > 1 ? tr("%1 CPU threads").arg(threads)
                                                  : entry.value("backend").toString().toUpper()},
                {"used", entry.value("used")}});
    }
    for (auto &[id, list] : parts) {
        const QString name = hub_.workerHello(id).value("machine").toString();
        machines.append(QVariantMap{{"id", id}, {"name", name.isEmpty() ? id : name}, {"parts", list}});
    }
    return machines;
}

bool RemoteSearch::canLend() const {
    return (lendGpu() && hasGpu()) || lendCpuThreads() > 0;
}

bool RemoteSearch::lendGpu() const {
    return AppSettings::Instance().value(QStringLiteral("remote/lendGpu"), true).toBool();
}

void RemoteSearch::setLendGpu(bool lend) {
    if (lend == lendGpu()) return;
    AppSettings::Instance().setValue(QStringLiteral("remote/lendGpu"), lend);
    restartLenders();
}

int RemoteSearch::cpuThreads() const {
    return static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
}

int RemoteSearch::lendCpuThreads() const {
    // by default all but two per GPU lent (each GPU is fed from the CPU)
    const int automatic = std::max(0, cpuThreads() - (lendGpu() && hasGpu() ? 2 : 0));
    return std::clamp(AppSettings::Instance().value(QStringLiteral("remote/lendCpuThreads"), automatic).toInt(), 0,
                      cpuThreads());
}

void RemoteSearch::setLendCpuThreads(int threads) {
    threads = std::clamp(threads, 0, cpuThreads());
    if (threads == lendCpuThreads()) return;
    AppSettings::Instance().setValue(QStringLiteral("remote/lendCpuThreads"), threads);
    restartLenders();
}

// what is lent changed: the lent machines start again with it
void RemoteSearch::restartLenders() {
    stopLender();
    updateLending();
    emit groupsChanged();
}

void RemoteSearch::updateGroup(Joined &group) {
    if (group.search) {
        const bool made = GroupHub(group.id, false) == nullptr;
        RemoteHub *const hub = GroupHub(group.id, true);
        if (made) {
            const QString id = group.id;
            connect(hub, &RemoteHub::groupChanged, this, [this, id](bool open) {
                emit groupsChanged();
                emit devicesChanged();
                if (open) return;
                // dropped (the network, the relay): connected again a little later, while still searched with
                QTimer::singleShot(5000, this, [this, id] {
                    const auto found = groups_.find(id);
                    if (found != groups_.end() && found->second.search) updateGroup(found->second);
                });
            });
        }
        if (!hub->groupOpen()) hub->openGroup(GroupUrl(group.id, QStringLiteral("client"), CurrentMember()));
    } else if (RemoteHub *const hub = GroupHub(group.id, false)) {
        hub->closeRoom();
    }
    updateLending();
}

void RemoteSearch::updateLending() {
    QStringList wanted;
    if (!localGpuBusy_ && QDir(packsDirectory_).exists() && canLend()) {
        for (const auto &[id, group] : groups_)
            if (group.lend && id != QLatin1String("public")) wanted << id;
        const auto pool = groups_.find(QStringLiteral("public"));
        if (pool != groups_.end() && pool->second.lend) wanted << pool->first;   // (the public pool last)
    }
    if (wanted == lentTo_ && (lender_ != nullptr || wanted.isEmpty())) return;
    stopLender();
    if (!wanted.isEmpty()) startLender(wanted);
}

// The lent machine: this ForeverTAS as a worker for every group lent to, in a process of its own (in bubblewrap on
// Linux: the system read-only, no home folder but the Packs and the GPU modules; on Windows an AppContainer).
void RemoteSearch::startLender(const QStringList &ids) {
    auto *process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList urls;
    for (const QString &id : ids)
        urls << GroupUrl(id, QStringLiteral("worker"), CurrentMember()).toString(QUrl::FullyEncoded);
    environment.insert(QStringLiteral("FOREVERTAS_WORKER_URLS"), urls.join(QLatin1Char('\n')));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    const QString app = lenderApp_.isEmpty() ? QCoreApplication::applicationFilePath() : lenderApp_;
    const LendCommand command = LenderCommand(
            app, packsDirectory_,
            {ComputeModules::InstallDirectory(), qEnvironmentVariable("FOREVERTAS_COMPUTE_DIR"),
             qEnvironmentVariable("APPDIR", QCoreApplication::applicationDirPath())},
            {QStringLiteral("--lend"), lendGpu() ? QStringLiteral("gpu,cpu") : QStringLiteral("cpu"),
             QStringLiteral("--cpu-threads"), QString::number(lendCpuThreads())});
    connect(process, &QProcess::readyRead, this, [process] {
        while (process->canReadLine()) diagnostics::Log("lent: %s", process->readLine().trimmed().constData());
    });
    connect(process, &QProcess::finished, this, [this, process](int code) {
        diagnostics::Log("lent: the worker ended (%d)", code);
        process->deleteLater();
        if (lender_ != process) return;
        lender_ = nullptr;
        lentTo_.clear();
        emit groupsChanged();
        // it ended by itself (the relay, the network): again a little later
        QTimer::singleShot(30 * 1000, this, [this] { updateLending(); });
    });
    lender_ = process;
    lentTo_ = ids;
    bool sandboxed = command.sandboxed;
#if defined(Q_OS_WIN)
    // Windows: an AppContainer (a GPU that does not work in it is simply not lent)
    QString why;
    if (qEnvironmentVariable("FOREVERTAS_LEND_UNSANDBOXED").isEmpty()) {
        sandboxed = SandboxLender(process, {packsDirectory_, ComputeModules::InstallDirectory(),
                                            qEnvironmentVariable("FOREVERTAS_COMPUTE_DIR"), QFileInfo(app).absolutePath()},
                                  &why);
        if (!sandboxed) diagnostics::Log("lending: %s", qUtf8Printable(why));
    }
#endif
    diagnostics::Log("lending to %s (%s)", qUtf8Printable(ids.join(QStringLiteral(", "))),
                     sandboxed ? "in a sandbox" : "no sandbox");
    process->start(command.program, command.arguments);
    emit groupsChanged();
}

// Never waits (the window's thread): the worker is killed (on Windows a process without a window cannot be asked
// to close; its connections simply drop, and the relays give its searches' parts to others), and forgotten once it
// has ended.
void RemoteSearch::stopLender() {
    QProcess *const process = lender_;
    if (process == nullptr) return;
    lender_ = nullptr;
    lentTo_.clear();
    process->kill();
    emit groupsChanged();
}

}  // namespace forevertas::app::remote
