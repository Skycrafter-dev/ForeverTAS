#include "app/remote/remote_hub.h"

#include "app/remote/remote_codec.h"
#include "app/remote/remote_worker.h"
#include "diagnostics/diagnostics_log.h"
#include "mutations/input_event_formatter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>

namespace forevertas::app::remote {

QUrl RelayUrl() {
    const QString relay = qEnvironmentVariable("FOREVERTAS_RELAY");
    return QUrl(relay.isEmpty() ? QStringLiteral("https://forevertas.tmtas.exchange") : relay);
}

RemoteHub::RemoteHub(QObject *parent) : QObject(parent) {}

RemoteHub::~RemoteHub() {
    std::lock_guard<std::mutex> guard(lock_);
    for (auto &[id, mailbox] : jobs_) {
        std::lock_guard<std::mutex> closing(mailbox->lock);
        mailbox->closed = true;
        mailbox->arrived.notify_all();
    }
}

quint16 RemoteHub::listen(quint16 port) {
    if (server_ == nullptr) {
        server_ = new QWebSocketServer(QStringLiteral("ForeverTAS"), QWebSocketServer::NonSecureMode, this);
        connect(server_, &QWebSocketServer::newConnection, this, &RemoteHub::accept);
    }
    if (!server_->isListening() && !server_->listen(QHostAddress::LocalHost, port)) {
        diagnostics::Log("remote: cannot listen for workers: %s", qUtf8Printable(server_->errorString()));
        return 0;
    }
    diagnostics::Log("remote: listening for workers on port %u", server_->serverPort());
    return server_->serverPort();
}

void RemoteHub::openRoom(const QUrl &relay) {
    closeRoom();
    if (network_ == nullptr) network_ = new QNetworkAccessManager(this);
    QNetworkRequest request(relay.resolved(QUrl(QStringLiteral("/rooms"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = network_->post(request, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, relay] {
        reply->deleteLater();
        const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || answer.value("code").toString().isEmpty()) {
            const QString why = reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                                         : QStringLiteral("the relay gave no room");
            diagnostics::Log("remote: no room: %s", qUtf8Printable(why));
            emit roomFailed(why);
            return;
        }
        const QString code = answer.value("code").toString();
        QUrl url = relay.resolved(QUrl(QStringLiteral("/rooms/%1/client").arg(code)));
        url.setScheme(relay.scheme() == QLatin1String("http") ? QStringLiteral("ws") : QStringLiteral("wss"));
        url.setQuery(QStringLiteral("secret=") + answer.value("secret").toString());
        connectRelay(url, code);
    });
}

void RemoteHub::openGroup(const QUrl &clientUrl) {
    closeRoom();
    groupMode_ = true;
    connectRelay(clientUrl, QString());
}

// The socket to a room (code: its pairing code) or to a group (code empty).
void RemoteHub::connectRelay(const QUrl &url, const QString &code) {
    room_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(room_, &QWebSocket::connected, this, [this, code] {
        if (code.isEmpty()) {
            groupOpen_ = true;
            diagnostics::Log("remote: group connected");
            // connected again while a search runs: it carries on (the relay keeps it a while)
            QString search;
            {
                std::lock_guard<std::mutex> guard(lock_);
                search = groupSearch_;
            }
            if (!search.isEmpty())
                room_->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                        {"type", "search"}, {"id", search}, {"resume", true}}).toJson(QJsonDocument::Compact)));
            emit groupChanged(true);
            return;
        }
        roomCode_ = code;
        diagnostics::Log("remote: room %s open", qUtf8Printable(code));
        emit roomChanged(code);
    });
    connect(room_, &QWebSocket::textMessageReceived, this, &RemoteHub::roomMessage);
    QWebSocket *const socket = room_;
    connect(room_, &QWebSocket::disconnected, this, [this, socket, code] {
        if (room_ != socket) return;
        QStringList through;
        {
            std::lock_guard<std::mutex> guard(lock_);
            for (const auto &[id, worker] : workers_)
                if (worker.socket == nullptr) through << id;
        }
        for (const QString &id : through) left(id);
        diagnostics::Log("remote: %s closed (%s)", code.isEmpty() ? "group" : "room",
                         qUtf8Printable(socket->closeReason()));
        room_->deleteLater();
        room_ = nullptr;
        if (code.isEmpty()) {
            groupOpen_ = false;
            emit groupChanged(false);
        } else {
            roomCode_.clear();
            emit roomChanged(QString());
        }
    });
    room_->open(url);
}

void RemoteHub::closeRoom() {
    if (room_ != nullptr) room_->close();
}

QString RemoteHub::roomCode() const {
    return roomCode_;
}

QStringList RemoteHub::workers() const {
    std::lock_guard<std::mutex> guard(lock_);
    QStringList ids;
    for (const auto &[id, worker] : workers_)
        if (!worker.hello.isEmpty()) ids << id;
    return ids;
}

QJsonObject RemoteHub::workerHello(const QString &id) const {
    std::lock_guard<std::mutex> guard(lock_);
    const auto found = workers_.find(id);
    return found == workers_.end() ? QJsonObject{} : found->second.hello;
}

void RemoteHub::accept() {
    while (QWebSocket *socket = server_->nextPendingConnection()) {
        const QString id = QStringLiteral("local-%1").arg(nextLocal_++);
        joined(id, socket);
        connect(socket, &QWebSocket::textMessageReceived, this, [this, id](const QString &text) {
            received(id, QJsonDocument::fromJson(text.toUtf8()).object());
        });
        connect(socket, &QWebSocket::disconnected, this, [this, id, socket] {
            left(id);
            socket->deleteLater();
        });
    }
}

void RemoteHub::joined(const QString &workerId, QWebSocket *socket) {
    std::lock_guard<std::mutex> guard(lock_);
    workers_[workerId] = Worker{socket, {}};
}

void RemoteHub::roomMessage(const QString &text) {
    const QJsonObject envelope = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString event = envelope.value("event").toString();
    if (groupMode_) {
        // a group's events, for the search running on it
        if (event == QLatin1String("machines")) groupMachines_ = envelope.value("count").toInt();
        std::shared_ptr<Mailbox> mailbox;
        {
            std::lock_guard<std::mutex> guard(lock_);
            mailbox = groupMailbox_;
        }
        if (mailbox) {
            std::lock_guard<std::mutex> guard(mailbox->lock);
            mailbox->messages.push_back(envelope);
            mailbox->arrived.notify_all();
        }
        return;
    }
    const QString worker = envelope.value(event.isEmpty() ? "from" : "worker").toString();
    if (worker.isEmpty()) return;
    if (event == QLatin1String("joined")) {
        joined(worker, nullptr);
    } else if (event == QLatin1String("left")) {
        left(worker);
    } else if (event.isEmpty()) {
        received(worker, envelope.value("message").toObject());
    }
}

void RemoteHub::received(const QString &workerId, const QJsonObject &message) {
    const QString type = message.value("type").toString();
    if (type == QLatin1String("hello")) {
        // the same ForeverTAS on both sides: a search must run the same everywhere
        const QString version = QCoreApplication::applicationVersion();
        if (message.value("version").toString() != version) {
            send(workerId, QJsonObject{{"type", "error"},
                                       {"message", QStringLiteral("this machine runs ForeverTAS %1; the client runs %2")
                                                           .arg(message.value("version").toString(), version)}});
            diagnostics::Log("remote: %s refused: ForeverTAS %s", qUtf8Printable(workerId),
                             qUtf8Printable(message.value("version").toString()));
            return;
        }
        {
            std::lock_guard<std::mutex> guard(lock_);
            const auto found = workers_.find(workerId);
            if (found == workers_.end()) return;
            found->second.hello = message;
        }
        diagnostics::Log("remote: %s joined (%s)", qUtf8Printable(workerId),
                         qUtf8Printable(QString::fromUtf8(QJsonDocument(message.value("gpus").toArray()).toJson(
                                 QJsonDocument::Compact))));
        emit workersChanged();
        return;
    }
    std::shared_ptr<Mailbox> mailbox;
    {
        std::lock_guard<std::mutex> guard(lock_);
        const auto found = jobs_.find(message.value("job").toString());
        if (found == jobs_.end()) return;
        mailbox = found->second;
    }
    std::lock_guard<std::mutex> guard(mailbox->lock);
    mailbox->messages.push_back(message);
    mailbox->arrived.notify_all();
}

void RemoteHub::left(const QString &workerId) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (workers_.erase(workerId) == 0) return;
        for (const auto &[job, worker] : jobWorkers_) {
            if (worker != workerId) continue;
            const auto mailbox = jobs_.find(job);
            if (mailbox == jobs_.end()) continue;
            std::lock_guard<std::mutex> closing(mailbox->second->lock);
            mailbox->second->closed = true;
            mailbox->second->arrived.notify_all();
        }
    }
    diagnostics::Log("remote: %s left", qUtf8Printable(workerId));
    emit workersChanged();
}

void RemoteHub::send(const QString &workerId, const QJsonObject &message) {
    QMetaObject::invokeMethod(this, [this, workerId, message] {
        QWebSocket *direct = nullptr;
        {
            std::lock_guard<std::mutex> guard(lock_);
            const auto found = workers_.find(workerId);
            if (found == workers_.end()) return;
            direct = found->second.socket;
        }
        if (direct != nullptr) {
            direct->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
        } else if (room_ != nullptr) {
            room_->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(QJsonObject{{"to", workerId}, {"message", message}}).toJson(QJsonDocument::Compact)));
        }
    }, Qt::QueuedConnection);
}

void RemoteHub::sendRelay(const QJsonObject &message) {
    QMetaObject::invokeMethod(this, [this, message] {
        if (room_ != nullptr) room_->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }, Qt::QueuedConnection);
}

namespace {

// A remote best, simulated here on exact physics: its state, snapshot and
// timeline, and proof of the worker's score (nullopt: it does not score so).
std::optional<SearchResult> Rebuilt(const SearchRequest &request, const std::vector<SandboxInputEvent> &inputs,
                                    double score) {
    const InputScriptParseResult parsed = ParseInputScript(FormatInputScript(inputs));
    if (!parsed) return std::nullopt;
    SearchRequest check = request;
    check.remoteDevice.clear();
    check.approximatePhysics = false;   // the check is on exact physics, whatever the search ran on
    check.baseInputCommands = parsed.commands;
    SearchRunControl loaded;
    loaded.reuseLoadedSandbox = true;   // (the map stays loaded from one check to the next)
    std::optional<SearchResult> rebuilt = EvaluateBaseline(check, &loaded);
    if (!rebuilt || rebuilt->bestScore != score) return std::nullopt;
    rebuilt->bestInputs = inputs;
    return rebuilt;
}

}  // namespace

SearchResult RemoteHub::runOnWorker(const QString &id, const SearchRequest &request, const SearchRunControl *control) {
    QFile replay(QString::fromStdString(request.replayPath));
    if (!replay.open(QIODevice::ReadOnly)) throw std::runtime_error("remote: cannot read the replay to send");
    const QByteArray replayBytes = replay.readAll();
    auto mailbox = std::make_shared<Mailbox>();
    QString job;
    {
        std::lock_guard<std::mutex> guard(lock_);
        const bool present = workers_.count(id) != 0;
        if (!present) throw std::runtime_error("remote: worker " + id.toStdString() + " is not connected");
        job = QStringLiteral("job-%1").arg(nextJob_++);
        jobs_[job] = mailbox;
        jobWorkers_[job] = id;
    }
    struct Forget {
        RemoteHub *hub;
        QString job;
        ~Forget() {
            std::lock_guard<std::mutex> guard(hub->lock_);
            hub->jobs_.erase(job);
            hub->jobWorkers_.erase(job);
        }
    } forget{this, job};

    send(id, QJsonObject{{"type", "job"},
                         {"job", job},
                         {"request", EncodeRequest(request)},
                         {"share", EncodeShare(control != nullptr ? *control : SearchRunControl{})},
                         {"packs", RemoteWorker::PackHashes(QString::fromStdString(request.packDirectory))},
                         {"replayName", QFileInfo(replay).fileName()},
                         {"replay", QString::fromLatin1(replayBytes.toBase64())}});

    bool stopSent = false, cancelSent = false;
    std::optional<std::vector<SandboxInputEvent>> offered;
    auto lastOffer = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (true) {
        std::deque<QJsonObject> messages;
        {
            std::unique_lock<std::mutex> guard(mailbox->lock);
            mailbox->arrived.wait_for(guard, std::chrono::milliseconds(20),
                                      [&] { return mailbox->closed || !mailbox->messages.empty(); });
            messages.swap(mailbox->messages);
            if (messages.empty() && mailbox->closed) throw std::runtime_error("remote: the worker left");
        }
        for (const QJsonObject &message : messages) {
            const QString type = message.value("type").toString();
            if (type == QLatin1String("claim")) {
                const auto count = static_cast<std::uint32_t>(message.value("count").toDouble());
                const std::uint64_t first =
                        control != nullptr && control->claimIterations ? control->claimIterations(count) : 0u;
                send(id, QJsonObject{{"type", "claimed"}, {"job", job}, {"count", static_cast<double>(count)},
                                     {"first", QString::number(first)}});
            } else if (type == QLatin1String("live")) {
                if (control != nullptr && control->liveChanged)
                    control->liveChanged(DecodeLive(message.value("live").toObject()));
            } else if (type == QLatin1String("progress")) {
                // the worker's own stages (loading the map, calibrating): the same app, the same stages
                const int stage = message.value("stage").toInt(-1);
                if (control != nullptr && control->progressChanged && stage >= 0 &&
                    stage <= static_cast<int>(SearchProgressStage::ExactRebuild)) {
                    control->progressChanged({static_cast<SearchProgressStage>(stage),
                                              message.value("completed").toString().toULongLong(),
                                              message.value("total").toString().toULongLong()});
                }
            } else if (type == QLatin1String("statistics")) {
                if (control != nullptr && control->statisticsChanged) {
                    SearchStatisticsUpdate statistics;
                    statistics.iterations = message.value("iterations").toString().toULongLong();
                    statistics.elapsed = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::nanoseconds(message.value("elapsed").toString().toLongLong()));
                    control->statisticsChanged(statistics);
                }
            } else if (type == QLatin1String("attempt")) {
                if (control != nullptr && control->attemptCompleted) {
                    std::optional<std::uint64_t> index;
                    if (message.contains("index")) index = message.value("index").toString().toULongLong();
                    std::optional<SearchLiveUpdate> attempt;
                    if (message.contains("live")) attempt = DecodeLive(message.value("live").toObject());
                    control->attemptCompleted(index, std::move(attempt));
                }
            } else if (type == QLatin1String("error")) {
                const QString kind = message.value("kind").toString();
                if (kind == QLatin1String("no-eligible")) throw NoEligibleEvaluation();
                if (kind == QLatin1String("cancelled")) throw SearchCancelled();
                throw std::runtime_error("remote worker: " + message.value("message").toString().toStdString());
            } else if (type == QLatin1String("result")) {
                const RemoteOutcome outcome = DecodeResult(message.value("result").toObject());
                // The best, simulated here: its state, snapshot and timeline, and proof of the worker's score.
                std::optional<SearchResult> rebuilt = Rebuilt(request, outcome.bestInputs, outcome.bestScore);
                if (!rebuilt) {
                    throw std::runtime_error("remote: the worker's best does not score " +
                                             std::to_string(outcome.bestScore) + " here");
                }
                SearchResult result = std::move(*rebuilt);
                result.winnerSource = outcome.winnerSource;
                result.winningIterationIndex = outcome.winningIterationIndex;
                result.winningMutationCount = outcome.winningMutationCount;
                result.bestInputs = outcome.bestInputs;
                result.iterations = outcome.iterations;
                result.evaluatorCalls = outcome.evaluatorCalls;
                result.mutationImprovementCount = outcome.mutationImprovementCount;
                result.totalMutationCount = outcome.totalMutationCount;
                result.elapsed = outcome.elapsed;
                result.lastImprovementElapsed = outcome.lastImprovementElapsed;
                result.approximate = outcome.approximate;
                return result;
            }
        }
        // the search's side: stop, cancel and a shared best to start from (pushed: the worker never waits for it)
        if (control != nullptr) {
            if (!cancelSent && control->cancellationRequested && control->cancellationRequested()) {
                send(id, QJsonObject{{"type", "cancel"}, {"job", job}});
                cancelSent = true;
            }
            if (!stopSent && control->stopRequested && control->stopRequested()) {
                send(id, QJsonObject{{"type", "stop"}, {"job", job}});
                stopSent = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (control->promotedBaselineInputs && now - lastOffer > std::chrono::milliseconds(100)) {
                lastOffer = now;
                std::optional<std::vector<SandboxInputEvent>> offer = control->promotedBaselineInputs();
                const bool changed = offer.has_value() != offered.has_value() ||
                                     (offer && EncodeEvents(*offer) != EncodeEvents(*offered));
                if (changed) {
                    QJsonObject message{{"type", "baseline"}, {"job", job}};
                    if (offer) message.insert("inputs", EncodeEvents(*offer));
                    send(id, message);
                    offered = std::move(offer);
                }
            }
        }
    }
}


SearchResult RemoteHub::runOnGroup(const SearchRequest &request, const SearchRunControl *control) {
    const auto prefer = [control](const SearchLiveUpdate &candidate, const SearchLiveUpdate &incumbent) {
        return control != nullptr && control->preferLive ? control->preferLive(candidate, incumbent)
                                                         : candidate.bestScore > incumbent.bestScore;
    };
    const auto ended = [control] {
        return control != nullptr && ((control->stopRequested && control->stopRequested()) ||
                                      (control->cancellationRequested && control->cancellationRequested()));
    };
    // the job, sent once: the relay hands it to every machine it gives the search
    QFile replay(QString::fromStdString(request.replayPath));
    if (!replay.open(QIODevice::ReadOnly)) throw std::runtime_error("remote: cannot read the replay to send");
    SearchRequest shared = request;
    shared.remoteDevice.clear();
    const QJsonObject job{{"version", QCoreApplication::applicationVersion()},
                          {"packs", RemoteWorker::PackHashes(QString::fromStdString(request.packDirectory))},
                          {"request", EncodeRequest(shared)},
                          {"replayName", QFileInfo(replay).fileName()},
                          {"replay", QString::fromLatin1(replay.readAll().toBase64())}};
    const auto newId = [] {
        return QString::fromLatin1(QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0') +
                                   QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'));
    };
    auto mailbox = std::make_shared<Mailbox>();
    QString id = newId();
    {
        std::lock_guard<std::mutex> guard(lock_);
        groupMailbox_ = mailbox;
        groupSearch_ = id;
    }
    struct Forget {
        RemoteHub *hub;
        ~Forget() {
            std::lock_guard<std::mutex> guard(hub->lock_);
            hub->groupMailbox_.reset();
            hub->groupSearch_.clear();
        }
    } forget{this};
    sendRelay(QJsonObject{{"type", "search"}, {"id", id}, {"job", job}});

    const auto started = std::chrono::steady_clock::now();
    auto lastTick = started, lastStatistics = started;
    double estimate = 0.0;          // attempts: the machines' speeds x time, until their counts come
    double perSecond = 0.0;
    std::uint64_t counted = 0u;     // the final counts handed in
    std::optional<SearchLiveUpdate> shown;     // the search's best the group knows (sent down)
    std::vector<SearchLiveUpdate> candidates;  // bests waiting for their check
    QJsonArray rejected;
    bool stopping = false, over = false;
    std::optional<std::chrono::steady_clock::time_point> stopSent;
    // A candidate is checked here (exact physics: re-simulated with its score; approximate: as reported, checked
    // at the end) and, if it is the search's new best, sent down. One check at a time, on its own thread (this
    // loop never waits for one), always of the best waiting.
    std::optional<SearchLiveUpdate> waiting;                 // the best not checked yet
    std::optional<SearchLiveUpdate> checking;                // the one being checked
    std::future<bool> check;
    const auto accept = [&](SearchLiveUpdate &&live) {
        shown = std::move(live);
        if (control != nullptr && control->liveChanged) control->liveChanged(*shown);
        sendRelay(QJsonObject{{"type", "best"}, {"sample", EncodeLive(*shown)}});
    };
    const auto consider = [&](std::vector<SearchLiveUpdate> &&items) {
        for (SearchLiveUpdate &item : items) {
            if ((shown && !prefer(item, *shown)) || EncodeEvents(item.bestInputs) == rejected) continue;
            if (!waiting || prefer(item, *waiting)) waiting = std::move(item);
        }
    };
    const auto advanceChecks = [&] {
        if (checking && check.valid() && check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            if (check.get()) {
                if (!shown || prefer(*checking, *shown)) accept(std::move(*checking));
            } else {
                rejected = EncodeEvents(checking->bestInputs);
                diagnostics::Log("remote: a group machine's best (%.17g) does not re-simulate here", checking->bestScore);
            }
            checking.reset();
        }
        if (checking || !waiting) return;
        if (shown && !prefer(*waiting, *shown)) {
            waiting.reset();
            return;
        }
        if (request.approximatePhysics) {   // (a twin score: checked at the end)
            accept(std::move(*waiting));
            waiting.reset();
            return;
        }
        checking = std::move(waiting);
        waiting.reset();
        check = std::async(std::launch::async, [&request, inputs = checking->bestInputs, score = checking->bestScore] {
            return Rebuilt(request, inputs, score).has_value();
        });
    };
    while (!over) {
        std::deque<QJsonObject> messages;
        {
            std::unique_lock<std::mutex> guard(mailbox->lock);
            mailbox->arrived.wait_for(guard, std::chrono::milliseconds(50), [&] { return !mailbox->messages.empty(); });
            messages.swap(mailbox->messages);
        }
        std::vector<SearchLiveUpdate> arrived;
        for (const QJsonObject &message : messages) {
            const QString event = message.value("event").toString();
            if (event == QLatin1String("machines")) {
                perSecond = message.value("perSecond").toDouble();
                diagnostics::Log("remote: the group search has %d machine(s), %.0f attempts a second",
                                 message.value("count").toInt(), perSecond);
            } else if (event == QLatin1String("batch")) {
                for (const QJsonValue &best : message.value("bests").toArray())
                    arrived.push_back(DecodeLive(best.toObject().value("live").toObject()));
                for (const QJsonValue &value : message.value("finals").toArray()) {
                    const QJsonObject final = value.toObject();
                    counted += static_cast<std::uint64_t>(final.value("iterations").toDouble());
                    if (final.contains("live")) arrived.push_back(DecodeLive(final.value("live").toObject()));
                }
                for (const QJsonValue &value : message.value("failed").toArray())
                    diagnostics::Log("remote: a group machine failed: %s",
                                     qUtf8Printable(value.toObject().value("message").toString()));
            } else if (event == QLatin1String("error")) {
                throw std::runtime_error("remote: " + message.value("message").toString().toStdString());
            } else if (event == QLatin1String("gone")) {
                // the relay ended it while this computer was away: it starts again (machines given anew)
                id = newId();
                {
                    std::lock_guard<std::mutex> guard(lock_);
                    groupSearch_ = id;
                }
                if (!stopping) sendRelay(QJsonObject{{"type", "search"}, {"id", id}, {"job", job}});
                else over = true;
            } else if (event == QLatin1String("ended")) {
                over = true;
            }
        }
        if (!arrived.empty()) consider(std::move(arrived));
        advanceChecks();
        // this computer's own devices' finds go down too (the group's machines adopt or beat them)
        if (!stopping && control != nullptr && control->searchBest) {
            if (std::optional<SearchLiveUpdate> best = control->searchBest(); best && (!shown || prefer(*best, *shown))) {
                shown = std::move(best);
                sendRelay(QJsonObject{{"type", "best"}, {"sample", EncodeLive(*shown)}});
            }
        }
        const auto now = std::chrono::steady_clock::now();
        estimate += perSecond * std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        if (control != nullptr && control->statisticsChanged && now - lastStatistics >= std::chrono::milliseconds(250)) {
            lastStatistics = now;
            control->statisticsChanged({static_cast<std::uint64_t>(std::max(estimate, static_cast<double>(counted))),
                                        now - started});
        }
        const bool limitReached = control != nullptr && control->iterationLimit &&
                                  std::max(estimate, static_cast<double>(counted)) >= *control->iterationLimit;
        if (!stopping && (ended() || limitReached)) {
            diagnostics::Log("remote: the group search stops (%.0f attempts, %.0f a second)", estimate, perSecond);
            stopping = true;
            perSecond = 0.0;
            stopSent = now;
            sendRelay(QJsonObject{{"type", "stop"}});
        }
        // (machines that never answer: the relay frees them after its own grace)
        if (stopping && stopSent && now - *stopSent > std::chrono::seconds(40)) over = true;
    }
    // (a check still running ends first; one waiting is taken if it beats what was shown)
    if (check.valid()) check.wait();
    advanceChecks();
    if (check.valid()) {
        check.wait();
        advanceChecks();
    }
    if (control != nullptr && control->cancellationRequested && control->cancellationRequested()) throw SearchCancelled();

    // the device's best: the search's best the group found, re-simulated here on exact physics (an approximate
    // best's score is what this computer computes)
    SearchRequest exact = request;
    exact.remoteDevice.clear();
    exact.approximatePhysics = false;
    std::optional<SearchResult> best;
    if (shown) {
        const InputScriptParseResult parsed = ParseInputScript(FormatInputScript(shown->bestInputs));
        if (parsed) {
            exact.baseInputCommands = parsed.commands;
            best = EvaluateBaseline(exact, nullptr);
            if (best) {
                best->bestInputs = shown->bestInputs;
                best->winnerSource = shown->winnerSource;
                best->winningIterationIndex = shown->winningIterationIndex;
            }
        }
    }
    if (!best) {
        // no machine found anything: the baseline, as a device that found nothing
        exact.baseInputCommands = request.baseInputCommands;
        best = EvaluateBaseline(exact, nullptr);
        if (!best) throw NoEligibleEvaluation();
    }
    best->iterations = static_cast<std::uint64_t>(std::max(estimate, static_cast<double>(counted)));
    best->elapsed = std::chrono::steady_clock::now() - started;
    return std::move(*best);
}

}  // namespace forevertas::app::remote
