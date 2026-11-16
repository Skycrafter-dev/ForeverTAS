#include "app/remote/remote_worker.h"

#include "app/compute_modules.h"
#include "app/remote/remote_codec.h"
#include "app/remote/group_search.h"
#include "app/remote/remote_groups.h"
#include "app/remote/remote_hub.h"
#include "app/search_worker.h"
#include "diagnostics/diagnostics_log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QEventLoop>
#include <QJsonDocument>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QSysInfo>
#include <QTimer>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QWebSocket>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <iostream>

namespace forevertas::app::remote {

struct RemoteWorker::Job {
    QString id;
    std::unique_ptr<QTemporaryDir> folder;
    std::optional<SearchRequest> request;
    SearchRunControl control;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> cancel{false};
    std::mutex lock;
    std::condition_variable claimed;
    // id blocks claimed ahead, per block size (a round trip never stalls the search)
    std::map<std::uint32_t, std::deque<std::uint64_t>> blocks;
    std::map<std::uint32_t, int> asked;
    std::optional<std::vector<SandboxInputEvent>> offered;
    std::chrono::steady_clock::time_point lastStatistics{};
    // what the console shows (the worker's own window: a notebook, a terminal)
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    int lastStage = -1;
    std::chrono::steady_clock::time_point lastProgress{};
    std::chrono::steady_clock::time_point lastPrinted{};
};

namespace {

// "[  12.3 s] text" on the console, the time since the search began
void Print(std::chrono::steady_clock::time_point started, const std::string &text) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "[%7.1f s] ", seconds);
    std::cout << stamp << text << std::endl;
}

}  // namespace

RemoteWorker::RemoteWorker(QString packDirectory, SearchRunner runSearch, QJsonObject lends, QObject *parent)
    : QObject(parent), packDirectory_(std::move(packDirectory)), runSearch_(std::move(runSearch)),
      lends_(std::move(lends)), packs_(PackHashes(packDirectory_)) {}

RemoteWorker::~RemoteWorker() {
    std::map<QString, std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> guard(lock_);
        jobs.swap(jobs_);
    }
    for (auto &[id, job] : jobs) {
        job->cancel = true;
        job->claimed.notify_all();
        if (job->thread.joinable()) job->thread.join();
    }
}

QJsonObject RemoteWorker::PackHashes(const QString &packDirectory) {
    // a pack's hash is kept while its size and time stay (hashing 90 MB takes a moment)
    static std::mutex cacheLock;
    static std::map<QString, std::pair<QString, QString>> cache;   // path -> (size:time, hash)
    QJsonObject hashes;
    const QDir directory(packDirectory);
    for (const QFileInfo &pack : directory.entryInfoList({QStringLiteral("*.pak")}, QDir::Files, QDir::Name)) {
        const QString stamp = QStringLiteral("%1:%2").arg(pack.size()).arg(pack.lastModified().toMSecsSinceEpoch());
        {
            std::lock_guard<std::mutex> guard(cacheLock);
            const auto found = cache.find(pack.absoluteFilePath());
            if (found != cache.end() && found->second.first == stamp) {
                hashes.insert(pack.fileName(), found->second.second);
                continue;
            }
        }
        QFile file(pack.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        const QString hex = QString::fromLatin1(hash.result().toHex());
        hashes.insert(pack.fileName(), hex);
        std::lock_guard<std::mutex> guard(cacheLock);
        cache[pack.absoluteFilePath()] = {stamp, hex};
    }
    return hashes;
}

void RemoteWorker::connectTo(const QUrl &url) {
    if (socket_ != nullptr) socket_->deleteLater();
    QWebSocket *const socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket_ = socket;
    auto opened = std::make_shared<bool>(false);
    connect(socket, &QWebSocket::connected, this, [this, opened] {
        *opened = true;
        if (!joined_) std::cout << "ForeverTAS worker: connected" << std::endl;
        joined_ = true;
        send(QJsonObject{{"type", "hello"},
                         {"version", QCoreApplication::applicationVersion()},
                         {"gpus", lends_.value("gpus")},
                         {"cpu", lends_.value("cpu")},
                         {"machine", lends_.value("machine")},
                         {"packs", packs_}});
        if (group_ && busy_) send(QJsonObject{{"type", "busy"}});
    });
    connect(socket, &QWebSocket::textMessageReceived, this, &RemoteWorker::received);
    // a connection that never opens (a network that drops it silently) is given up after a while
    QTimer::singleShot(20000, socket, [socket, opened] {
        if (!*opened) socket->abort();
    });
    connect(socket, &QWebSocket::disconnected, this, [this, socket, opened] {
        if (socket != socket_) return;   // (an older connection's)
        lastCloseCode_ = socket->closeCode();
        const QString error = socket->errorString();
        const QString why = *opened ? socket->closeReason()
                : error.contains(QLatin1String("404")) ? QStringLiteral("there is no such room or group (a pairing code "
                                                                        "works only while its room is open)")
                : error.contains(QLatin1String("403")) ? QStringLiteral("not let in (an invite works only until the group "
                                                                        "makes a new one)")
                                                       : QStringLiteral("could not reach the relay: ") + error;
        diagnostics::Log("remote worker: disconnected (%s)", qUtf8Printable(why));
        std::cerr << "ForeverTAS worker: disconnected" << (why.isEmpty() ? std::string() : ": " + why.toStdString())
                  << std::endl;
        {
            std::lock_guard<std::mutex> guard(lock_);
            for (auto &[id, job] : jobs_) {
                job->cancel = true;
                job->claimed.notify_all();
            }
        }
        groupSearch_.reset();   // (its count cannot be handed in: the relay credits its speed x time)
        emit finished();
    });
    socket->open(url);
}

// A group's messages: a job (this machine's part of a search), a better overall best, a new waiting period, stop.
void RemoteWorker::groupMessage(const QString &type, const QJsonObject &message) {
    const QString search = message.value("search").toString();
    if (type == QLatin1String("job")) {
        groupSearch_.reset();   // (a previous one has handed in its count already, or is dropped)
        // the same game: every pack both sides have is the same file
        const QJsonObject theirs = message.value("job").toObject().value("packs").toObject();
        for (auto it = theirs.begin(); it != theirs.end(); ++it) {
            if (packs_.contains(it.key()) && packs_.value(it.key()) != it.value()) {
                send(QJsonObject{{"type", "error"}, {"search", search},
                                 {"message", QStringLiteral("this machine's %1 is another version of the game's").arg(it.key())}});
                return;
            }
        }
        // the machine busy with an earlier connection's search: this one hands in nothing at once
        if (holding_ && desk_ != nullptr) {
            holding_ = false;
            desk_->done(this);
        }
        if (desk_ != nullptr && !desk_->take(this)) {
            send(QJsonObject{{"type", "final"}, {"search", search}, {"iterations", 0}, {"seconds", 0}});
            return;
        }
        holding_ = desk_ != nullptr;
        try {
            groupSearch_ = std::make_unique<GroupSearch>(
                    message, packDirectory_, lends_, runSearch_, [this](const QJsonObject &out) { send(out); },
                    [this, search] {
                        QMetaObject::invokeMethod(this, [this, search] {
                            if (holding_ && groupSearch_ && groupSearch_->search() == search && desk_ != nullptr) {
                                holding_ = false;
                                desk_->done(this);
                            }
                        }, Qt::QueuedConnection);
                    });
        } catch (const std::exception &error) {
            send(QJsonObject{{"type", "error"}, {"search", search}, {"message", QString::fromUtf8(error.what())}});
            if (holding_) {
                holding_ = false;
                desk_->done(this);
            }
        }
        return;
    }
    if (!groupSearch_ || groupSearch_->search() != search) return;
    if (type == QLatin1String("overall")) groupSearch_->overall(message.value("best").toObject());
    else if (type == QLatin1String("cooldown")) groupSearch_->cooldown(message.value("cooldownMs").toInt());
    else if (type == QLatin1String("stop")) groupSearch_->stop();
}

// ---- the machine's desk ----

int MachineDesk::order(const RemoteWorker *connection) const {
    const auto found = std::find(connections_.begin(), connections_.end(), connection);
    return static_cast<int>(found - connections_.begin());
}

bool MachineDesk::take(RemoteWorker *connection) {
    if (holder_ == connection) {
        ++holds_;
        return true;
    }
    if (holder_ != nullptr && order(connection) > order(holder_)) return false;
    if (holder_ != nullptr) holder_->yieldSearch();   // (a later one's search hands in its count and goes)
    holder_ = connection;
    holds_ = 1;
    tell();
    return true;
}

void MachineDesk::done(RemoteWorker *connection) {
    if (connection != holder_ || --holds_ > 0) return;
    holder_ = nullptr;
    holds_ = 0;
    tell();
}

void MachineDesk::tell() {
    for (RemoteWorker *connection : connections_)
        connection->setBusy(holder_ != nullptr && order(connection) > order(holder_));
}

void RemoteWorker::setBusy(bool busy) {
    if (busy == busy_) return;
    busy_ = busy;
    if (group_) send(QJsonObject{{"type", busy ? "busy" : "free"}});
}

void RemoteWorker::yieldSearch() {
    if (groupSearch_) groupSearch_->stop();
    std::lock_guard<std::mutex> guard(lock_);
    for (auto &[id, job] : jobs_) job->stop = true;
    holding_ = false;
}

void RemoteWorker::send(const QJsonObject &message) {
    const QString text = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    QMetaObject::invokeMethod(this, [this, text] {
        if (socket_ != nullptr) socket_->sendTextMessage(text);
    }, Qt::QueuedConnection);
}

void RemoteWorker::received(const QString &text) {
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = message.value("type").toString();
    if (group_) {
        groupMessage(type, message);
        return;
    }
    if (type == QLatin1String("job")) {
        start(message);
        return;
    }
    if (type == QLatin1String("error")) {
        diagnostics::Log("remote worker: %s", qUtf8Printable(message.value("message").toString()));
        std::cerr << "ForeverTAS worker: " << message.value("message").toString().toStdString() << std::endl;
        return;
    }
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> guard(lock_);
        const auto found = jobs_.find(message.value("job").toString());
        if (found == jobs_.end()) return;
        job = found->second;
    }
    if (type == QLatin1String("claimed")) {
        std::lock_guard<std::mutex> guard(job->lock);
        const auto count = static_cast<std::uint32_t>(message.value("count").toDouble());
        job->blocks[count].push_back(message.value("first").toString().toULongLong());
        --job->asked[count];
        job->claimed.notify_all();
    } else if (type == QLatin1String("baseline")) {
        std::lock_guard<std::mutex> guard(job->lock);
        job->offered = message.contains("inputs")
                ? std::optional(DecodeEvents(message.value("inputs").toArray()))
                : std::nullopt;
    } else if (type == QLatin1String("stop")) {
        job->stop = true;
    } else if (type == QLatin1String("cancel")) {
        job->cancel = true;
        job->claimed.notify_all();
    }
}

void RemoteWorker::start(const QJsonObject &message) {
    auto job = std::make_shared<Job>();
    job->id = message.value("job").toString();
    const auto fail = [&](const QString &why) {
        send(QJsonObject{{"type", "error"}, {"job", job->id}, {"kind", "failed"}, {"message", why}});
    };
    // the same game: every pack both sides have is the same file
    const QJsonObject theirs = message.value("packs").toObject();
    for (auto it = theirs.begin(); it != theirs.end(); ++it) {
        if (packs_.contains(it.key()) && packs_.value(it.key()) != it.value()) {
            fail(QStringLiteral("this worker's %1 is another version of the game's").arg(it.key()));
            return;
        }
    }
    job->folder = std::make_unique<QTemporaryDir>();
    const QString replayPath = job->folder->filePath(QFileInfo(message.value("replayName").toString()).fileName());
    QFile replay(replayPath);
    if (!job->folder->isValid() || !replay.open(QIODevice::WriteOnly) ||
        replay.write(QByteArray::fromBase64(message.value("replay").toString().toLatin1())) < 0) {
        fail(QStringLiteral("this worker cannot write the replay"));
        return;
    }
    replay.close();
    try {
        job->request.emplace(DecodeRequest(message.value("request").toObject(), packDirectory_.toStdString(),
                                           replayPath.toStdString()));
        DecodeShare(message.value("share").toObject(), &job->control);
        // what this machine lends: a CPU device runs on at most the threads it lends
        if (IsGpuBackend(job->request->backend)) {
            if (lends_.value("gpus").toArray().isEmpty()) throw std::runtime_error("this machine lends no GPU");
        } else {
            const int threads = lends_.value("cpu").toObject().value("threads").toInt();
            if (threads <= 0) throw std::runtime_error("this machine lends no CPU");
            job->request->parallelSampleCount = static_cast<std::uint32_t>(
                    std::clamp<qint64>(job->request->parallelSampleCount, 1, threads));
            job->request->backend = job->request->parallelSampleCount > 1u ? PhysicsBackend::MultiThreadedCpu
                                                                            : PhysicsBackend::OptimizedCpu;
        }
    } catch (const std::exception &error) {
        fail(QString::fromUtf8(error.what()));
        return;
    }
    const QString id = job->id;
    Job *const j = job.get();
    SearchRunControl &control = job->control;
    control.cancellationRequested = [j] { return j->cancel.load(); };
    control.stopRequested = [j] { return j->stop.load(); };
    control.beginIteration = [j] { return !j->stop.load() && !j->cancel.load(); };
    control.claimIterations = [this, j, id](std::uint32_t count) -> std::uint64_t {
        std::unique_lock<std::mutex> guard(j->lock);
        auto ask = [&] {
            ++j->asked[count];
            send(QJsonObject{{"type", "claim"}, {"job", id}, {"count", static_cast<double>(count)}});
        };
        if (j->blocks[count].empty() && j->asked[count] == 0) ask();
        j->claimed.wait(guard, [&] { return !j->blocks[count].empty() || j->cancel.load(); });
        if (j->blocks[count].empty()) throw SearchCancelled();
        const std::uint64_t first = j->blocks[count].front();
        j->blocks[count].pop_front();
        if (j->blocks[count].empty() && j->asked[count] == 0) ask();   // the next block, ahead
        return first;
    };
    control.promotedBaselineInputs = [j]() -> std::optional<std::vector<SandboxInputEvent>> {
        std::lock_guard<std::mutex> guard(j->lock);
        return j->offered;
    };
    control.liveChanged = [this, id](const SearchLiveUpdate &live) {
        send(QJsonObject{{"type", "live"}, {"job", id}, {"live", EncodeLive(live)}});
    };
    const std::string backend(PhysicsBackendId(job->request->backend));
    control.progressChanged = [this, j, id, backend](const SearchProgress &progress) {
        const auto now = std::chrono::steady_clock::now();
        const int stage = static_cast<int>(progress.stage);
        if (stage == j->lastStage && now - j->lastProgress < std::chrono::milliseconds(250)) return;
        if (stage != j->lastStage) Print(j->started, SearchStageStatus(progress.stage, backend).toStdString());
        j->lastStage = stage;
        j->lastProgress = now;
        send(QJsonObject{{"type", "progress"}, {"job", id}, {"stage", stage},
                         {"completed", QString::number(progress.completedWork)},
                         {"total", QString::number(progress.totalWork)}});
    };
    control.statisticsChanged = [this, j, id](const SearchStatisticsUpdate &statistics) {
        const auto now = std::chrono::steady_clock::now();
        if (now - j->lastPrinted >= std::chrono::seconds(10)) {
            j->lastPrinted = now;
            const double seconds = std::chrono::duration<double>(statistics.elapsed).count();
            char line[96];
            std::snprintf(line, sizeof line, "%llu attempts (%.0f a second)",
                          static_cast<unsigned long long>(statistics.iterations),
                          seconds > 0.0 ? static_cast<double>(statistics.iterations) / seconds : 0.0);
            Print(j->started, line);
        }
        if (now - j->lastStatistics < std::chrono::milliseconds(250)) return;
        j->lastStatistics = now;
        send(QJsonObject{{"type", "statistics"}, {"job", id}, {"iterations", QString::number(statistics.iterations)},
                         {"elapsed", QString::number(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                             statistics.elapsed).count())}});
    };
    if (message.value("share").toObject().value("attempts").toBool()) {
        control.attemptCompleted = [this, id](std::optional<std::uint64_t> index,
                                              std::optional<SearchLiveUpdate> attempt) {
            QJsonObject message{{"type", "attempt"}, {"job", id}};
            if (index) message.insert("index", QString::number(*index));
            if (attempt) message.insert("live", EncodeLive(*attempt));
            send(message);
        };
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        jobs_[id] = job;
    }
    if (desk_ != nullptr) desk_->take(this);   // (a room is first in the desk's order)
    diagnostics::Log("remote worker: %s started", qUtf8Printable(id));
    Print(job->started, "a search began");
    job->thread = std::thread([this, job] {
        QJsonObject outcome{{"job", job->id}};
        try {
            const SearchResult result = runSearch_(*job->request, &job->control);
            outcome.insert("type", "result");
            outcome.insert("result", EncodeResult(result));
        } catch (const NoEligibleEvaluation &) {
            outcome.insert("type", "error");
            outcome.insert("kind", "no-eligible");
        } catch (const SearchCancelled &) {
            outcome.insert("type", "error");
            outcome.insert("kind", "cancelled");
        } catch (const std::exception &error) {
            outcome.insert("type", "error");
            outcome.insert("kind", "failed");
            outcome.insert("message", QString::fromUtf8(error.what()));
        }
        send(outcome);
        const QString kind = outcome.value("type") == QLatin1String("result") ? QStringLiteral("finished")
                : outcome.value("kind") == QLatin1String("failed") ? QStringLiteral("failed: ") + outcome.value("message").toString()
                : outcome.value("kind").toString();
        Print(job->started, "the search " + kind.toStdString());
        diagnostics::Log("remote worker: %s %s", qUtf8Printable(job->id),
                         qUtf8Printable(outcome.value("type").toString()));
        // forget it on the worker's thread (its own thread cannot join itself)
        QMetaObject::invokeMethod(this, [this, id = job->id] {
            std::shared_ptr<Job> done;
            {
                std::lock_guard<std::mutex> guard(lock_);
                const auto found = jobs_.find(id);
                if (found == jobs_.end()) return;
                done = found->second;
                jobs_.erase(found);
            }
            if (done->thread.joinable()) done->thread.join();
            if (desk_ != nullptr) desk_->done(this);
        }, Qt::QueuedConnection);
    });
}

QString CpuName() {
#if defined(Q_OS_WIN)
    const QSettings processor(
            QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
            QSettings::NativeFormat);
    const QString name = processor.value(QStringLiteral("ProcessorNameString")).toString().simplified();
#else
    QString name;
    QFile info(QStringLiteral("/proc/cpuinfo"));
    if (info.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : info.readAll().split('\n')) {
            if (line.startsWith("model name")) {
                name = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).simplified();
                break;
            }
        }
    }
#endif
    return name.isEmpty() ? QStringLiteral("CPU") : name;
}

QUrl WorkerUrl(const QString &codeOrUrl) {
    if (codeOrUrl.contains(QLatin1String("://"))) return QUrl(codeOrUrl);
    const QUrl relay = RelayUrl();
    QUrl url = relay.resolved(QUrl(QStringLiteral("/rooms/%1/worker").arg(codeOrUrl.trimmed().toUpper())));
    url.setScheme(relay.scheme() == QLatin1String("http") ? QStringLiteral("ws") : QStringLiteral("wss"));
    return url;
}

int RunRemoteWorker(int argc, char **argv, const QString &version) {
#if defined(_WIN32)
    // a GUI program started from a console: its messages go there (unless they go to a pipe or a file already:
    // a window lending, a test)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((output == nullptr || output == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationVersion(version);
    // a sandboxed lender cannot ask Windows for its proxy (the question never comes back): it connects directly
    if (!qEnvironmentVariable("FOREVERTAS_LEND_NO_PROXY").isEmpty()) {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
    // as the window's: a member's identity is kept in the same settings
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTAS"));
    QCoreApplication::setApplicationName(QStringLiteral("ForeverTAS"));
    QString url, packs, name, lend = QStringLiteral("gpu,cpu");   // (url: --worker's codes)
    int cpuThreads = -1;   // -1: all but what the GPUs need
    for (int i = 1; i + 1 < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QLatin1String("--worker")) url = QString::fromLocal8Bit(argv[i + 1]);
        else if (argument == QLatin1String("--packs")) packs = QString::fromLocal8Bit(argv[i + 1]);
        else if (argument == QLatin1String("--name")) name = QString::fromLocal8Bit(argv[i + 1]);
        else if (argument == QLatin1String("--lend")) lend = QString::fromLocal8Bit(argv[i + 1]).toLower();
        else if (argument == QLatin1String("--cpu-threads")) cpuThreads = QString::fromLocal8Bit(argv[i + 1]).toInt();
    }
    // what it lends to, in order: a pairing code (its room) first, then groups' invites as given, the public pool
    // last; or, lent from the window, the groups' addresses (with this member's credentials: never on the command
    // line), one a line, in the window's order
    QStringList targets;
    const QString lent = qEnvironmentVariable("FOREVERTAS_WORKER_URLS");
    if (!lent.isEmpty()) {
        targets = lent.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    } else {
        for (const QString &part : url.split(QLatin1Char(','), Qt::SkipEmptyParts)) targets << part.trimmed();
        std::stable_sort(targets.begin(), targets.end(), [](const QString &a, const QString &b) {
            const auto rank = [](const QString &target) {
                const QString group = GroupOfInvite(target);
                return group.isEmpty() ? 0 : group == QLatin1String("public") ? 2 : 1;
            };
            return rank(a) < rank(b);
        });
    }
    if (targets.isEmpty() || packs.isEmpty() || !QDir(packs).exists()) {
        std::cerr << "usage: ForeverTAS --worker CODE[,CODE...] --packs PACKS_DIRECTORY [--name NAME]\n"
                     "       (CODE: a pairing code, a group's invite, or public: one search at a time, in that order)\n"
                     "       [--lend gpu,cpu (what this machine lends)] [--cpu-threads N (default: all but 2 per GPU)]\n";
        return 2;
    }
    QJsonArray gpus;
    for (const PhysicsBackend backend : {PhysicsBackend::Cuda, PhysicsBackend::Hip}) {
        if (!lend.contains(QLatin1String("gpu"))) break;
        const compute::GpuStatus status = ComputeModules::Instance().Status(backend);
        if (!status.ready) continue;
        gpus.append(QJsonObject{{"backend", QString::fromLatin1(PhysicsBackendId(backend).data(),
                                                                static_cast<int>(PhysicsBackendId(backend).size()))},
                                {"name", QString::fromStdString(status.deviceName)}});
    }
    // its CPU: all its threads but two for each GPU it feeds, unless told
    const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    if (!lend.contains(QLatin1String("cpu"))) cpuThreads = 0;
    else if (cpuThreads < 0) cpuThreads = std::max(0, hardware - 2 * static_cast<int>(gpus.size()));
    cpuThreads = std::min(cpuThreads, hardware);
    const QJsonObject cpu{{"name", CpuName()}, {"threads", cpuThreads}};
    if (gpus.isEmpty() && cpuThreads == 0) {
        std::cerr << "ForeverTAS worker: this machine lends nothing (no GPU ForeverTAS searches on, no CPU thread)\n";
        return 2;
    }
    std::cout << "ForeverTAS " << version.toStdString() << " worker: " << gpus.size() << " GPU(s), " << cpuThreads
              << " CPU thread(s), packs " << packs.toStdString() << std::endl;

    // each target its connection; groups' invites (and the public pool) are joined as a member first
    struct Connection {
        std::unique_ptr<RemoteWorker> worker;
        QUrl address;
        QString label;
        bool lending = false;   // a group's: connects again after a drop, until it leaves the group
        bool over = false;
    };
    std::vector<Connection> connections;
    MachineDesk desk;
    QNetworkAccessManager network;
    if (!name.isEmpty()) SetMemberName(name);
    for (const QString &target : targets) {
        Connection connection;
        const QString group = lent.isEmpty() ? GroupOfInvite(target) : QString();
        if (!lent.isEmpty()) {
            connection.address = QUrl(target);
            connection.lending = true;
            connection.label = QStringLiteral("a group, lent from the window");
        } else if (!group.isEmpty()) {
            Member member = CurrentMember();
            if (member.name.isEmpty()) member.name = QSysInfo::machineHostName();
            QEventLoop waiting;
            QString failure, groupName;
            GroupRequest(&network, group, QStringLiteral("join"), member, QJsonObject{{"invite", target}},
                         [&](const QJsonObject &answer, const QString &error) {
                             failure = error;
                             groupName = answer.value("name").toString();
                             waiting.quit();
                         });
            waiting.exec();
            if (!failure.isEmpty()) {
                std::cerr << "ForeverTAS worker: cannot join " << target.toStdString() << ": " << failure.toStdString() << '\n';
                continue;
            }
            std::cout << "ForeverTAS worker: lending to " << groupName.toStdString() << " as " << member.name.toStdString()
                      << std::endl;
            connection.address = GroupUrl(group, QStringLiteral("worker"), member);
            connection.lending = true;
            connection.label = groupName;
        } else {
            connection.address = WorkerUrl(target);
            connection.label = QStringLiteral("room ") + target.toUpper();
        }
        connection.worker = std::make_unique<RemoteWorker>(
                packs,
                [](const SearchRequest &request, const SearchRunControl *control) {
                    return ComputeModules::Instance().RunSearch(request, control);
                },
                QJsonObject{{"gpus", gpus}, {"cpu", cpu},
                            {"machine", name.isEmpty() ? QSysInfo::machineHostName() : name}});
        connection.worker->setGroup(connection.lending);
        connection.worker->setDesk(&desk);
        desk.add(connection.worker.get());
        connections.push_back(std::move(connection));
    }
    if (connections.empty()) return 1;
    for (std::size_t index = 0; index < connections.size(); ++index) {
        RemoteWorker *const worker = connections[index].worker.get();
        QObject::connect(worker, &RemoteWorker::finished, &application, [&, index] {
            Connection &connection = connections[index];
            if (!connection.lending || worker->lastCloseCode() == 4003 || !worker->joined()) {
                connection.over = true;   // (a room that ended, a group left, a code never let in)
                if (std::all_of(connections.begin(), connections.end(), [](const Connection &c) { return c.over; })) {
                    const bool anyJoined = std::any_of(connections.begin(), connections.end(),
                                                       [](const Connection &c) { return c.worker->joined(); });
                    application.exit(anyJoined ? 0 : 1);
                }
                return;
            }
            std::cout << "ForeverTAS worker: " << connection.label.toStdString() << ": connecting again in 10 s" << std::endl;
            QTimer::singleShot(10000, worker, [&connections, index] {
                connections[index].worker->connectTo(connections[index].address);
            });
        });
        worker->connectTo(connections[index].address);
    }
    return application.exec();
}

}  // namespace forevertas::app::remote
