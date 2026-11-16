#ifndef FOREVERTAS_APP_REMOTE_REMOTE_WORKER_H
#define FOREVERTAS_APP_REMOTE_REMOTE_WORKER_H

// A remote worker (docs/REMOTE.md): connects to a client, says what it has
// (GPUs, the packs it holds) and runs the search devices it is sent on its
// own GPUs, with its own Packs.

#include "searches/search_runner.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

class QWebSocket;

namespace forevertas::app::remote {

class GroupSearch;
class RemoteWorker;

// The machine a worker process lends, through its connections (its room, groups, the public pool) in their
// order: one search at a time, the first in the order first (a later connection's search is stopped for it), and
// the later connections told the machine is busy (their relays give it nothing meanwhile).
class MachineDesk {
public:
    void add(RemoteWorker *connection) { connections_.push_back(connection); }
    // A search of this connection may run: the machine is free, or busy only for later connections (stopped).
    bool take(RemoteWorker *connection);
    // One of its searches ended.
    void done(RemoteWorker *connection);

private:
    int order(const RemoteWorker *connection) const;
    void tell();
    std::vector<RemoteWorker *> connections_;
    RemoteWorker *holder_ = nullptr;
    int holds_ = 0;   // its searches running (a room's GPU and CPU jobs at once)
};

class RemoteWorker final : public QObject {
    Q_OBJECT

public:
    // runSearch: how this worker runs a search (the app: ComputeModules).
    using SearchRunner = std::function<SearchResult(const SearchRequest &, const SearchRunControl *)>;
    // lends: what it searches on, {gpus: [{backend, name}], cpu: {name, threads}}.
    RemoteWorker(QString packDirectory, SearchRunner runSearch, QJsonObject lends, QObject *parent = nullptr);
    ~RemoteWorker() override;

    // A machine lent to a group (docs/REMOTE.md, "Groups"), not a room's worker.
    void setGroup(bool group) { group_ = group; }
    // The desk this connection shares the machine through (several connections), and what it hears from it.
    void setDesk(MachineDesk *desk) { desk_ = desk; }
    void setBusy(bool busy);
    void yieldSearch();   // a search of an earlier connection takes the machine
    // (again after a disconnection: a lent machine's)
    void connectTo(const QUrl &url);
    // How the last connection ended (4003: the member left the group).
    int lastCloseCode() const { return lastCloseCode_; }
    // It was let in at least once.
    bool joined() const { return joined_; }
    // The environment packs' content hashes ({"Stadium.pak": "sha256"}): what
    // a job checks against.
    static QJsonObject PackHashes(const QString &packDirectory);

signals:
    void finished();

private:
    struct Job;
    void received(const QString &text);
    void start(const QJsonObject &message);
    void send(const QJsonObject &message);
    void groupMessage(const QString &type, const QJsonObject &message);

    QString packDirectory_;
    SearchRunner runSearch_;
    QJsonObject lends_;
    QJsonObject packs_;
    QWebSocket *socket_ = nullptr;
    int lastCloseCode_ = 0;
    MachineDesk *desk_ = nullptr;
    bool busy_ = false;
    bool holding_ = false;   // the desk counts a search of this connection
    bool group_ = false;
    std::unique_ptr<GroupSearch> groupSearch_;   // its part of a group's search
    bool joined_ = false;
    std::mutex lock_;
    std::map<QString, std::shared_ptr<Job>> jobs_;
};

// This computer's CPU model ("CPU" when unknown).
QString CpuName();

// Where a worker joins a room by its pairing code (on RelayUrl()), or the URL
// given as is (ws://, wss://).
QUrl WorkerUrl(const QString &codeOrUrl);

// ForeverTAS --worker CODE|INVITE|public|URL --packs DIR [--name NAME] [--lend gpu,cpu] [--cpu-threads N]: a
// worker until the connection ends (a group's: until it leaves the group).
int RunRemoteWorker(int argc, char **argv, const QString &version);

}  // namespace forevertas::app::remote

#endif
