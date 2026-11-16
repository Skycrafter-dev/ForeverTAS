#ifndef FOREVERTAS_APP_REMOTE_REMOTE_HUB_H
#define FOREVERTAS_APP_REMOTE_REMOTE_HUB_H

// The client's side of remote searching (docs/REMOTE.md): the workers that
// joined, and a search device run on one of them. Workers join through a room
// on the relay (openRoom: a pairing code), or connect to a WebSocket server on
// this computer (listen: tests and development). A hub can instead be a
// group's (openGroup, ForeverTAS@home): its workers are the machines the
// relay gives this computer's search, and runOnGroup runs a search on them.

#include "searches/search_runner.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

class QNetworkAccessManager;
class QWebSocket;
class QWebSocketServer;

namespace forevertas::app::remote {

// The relay ForeverTAS uses unless FOREVERTAS_RELAY names another.
QUrl RelayUrl();

class RemoteHub final : public QObject {
    Q_OBJECT

public:
    explicit RemoteHub(QObject *parent = nullptr);
    ~RemoteHub() override;

    // Listens for workers on this computer (0: any free port); the port, or 0.
    quint16 listen(quint16 port = 0);
    // Opens a room on the relay for workers to join (roomChanged gives its code).
    void openRoom(const QUrl &relay = RelayUrl());
    void closeRoom();
    QString roomCode() const;
    // Connects to a group's search side (its client address, with the
    // member's credentials); groupChanged says when it is connected or not.
    void openGroup(const QUrl &clientUrl);
    bool groupOpen() const { return groupOpen_; }
    // The machines the relay has given this computer's search (a group's).
    int groupMachines() const { return groupMachines_.load(); }

    // The workers that said hello: their ids.
    QStringList workers() const;
    // What a worker said in its hello (GPUs, packs).
    QJsonObject workerHello(const QString &id) const;

    // Runs one device of a search on a worker (any thread but the hub's):
    // the device's calls go over the connection; its result's best is
    // simulated here before it is returned. Throws as RunSearch does.
    SearchResult runOnWorker(const QString &id, const SearchRequest &request, const SearchRunControl *control);
    // Runs one device of a search on a group's machines (any thread but the
    // hub's): the relay gives the search machines while it runs and takes
    // some back for other searches; each runs the device's share as a worker
    // does, and the device's best is the best of theirs that re-simulates
    // here. With no machine given, it waits until the search ends.
    SearchResult runOnGroup(const SearchRequest &request, const SearchRunControl *control);

signals:
    void workersChanged();
    // The room's code ("" when closed), or why it could not open.
    void roomChanged(const QString &code);
    void roomFailed(const QString &why);
    void groupChanged(bool open);

private:
    struct Mailbox {
        std::mutex lock;
        std::condition_variable arrived;
        std::deque<QJsonObject> messages;
        bool closed = false;
    };
    struct Worker {
        QWebSocket *socket = nullptr;   // a direct connection; nullptr: through the room
        QJsonObject hello;
    };
    void accept();
    void received(const QString &workerId, const QJsonObject &message);
    void roomMessage(const QString &text);
    void joined(const QString &workerId, QWebSocket *socket);
    void left(const QString &workerId);
    void send(const QString &workerId, const QJsonObject &message);
    // A message to the relay itself (a group's coordinator: search, best, stop).
    void sendRelay(const QJsonObject &message);
    void connectRelay(const QUrl &url, const QString &code);

    QWebSocketServer *server_ = nullptr;
    QNetworkAccessManager *network_ = nullptr;
    QWebSocket *room_ = nullptr;
    QString roomCode_;
    bool groupOpen_ = false;
    // a group's: its search running (one at a time), and what the relay says of it
    bool groupMode_ = false;
    QString groupSearch_;
    std::shared_ptr<Mailbox> groupMailbox_;
    std::atomic<int> groupMachines_{0};
    mutable std::mutex lock_;
    std::map<QString, Worker> workers_;                   // worker id -> worker
    std::map<QString, std::shared_ptr<Mailbox>> jobs_;    // job id -> its mailbox
    std::map<QString, QString> jobWorkers_;               // job id -> worker id
    quint64 nextJob_ = 1;
    quint64 nextLocal_ = 1;
};

}  // namespace forevertas::app::remote

#endif
