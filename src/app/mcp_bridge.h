#ifndef FOREVERTAS_APP_MCP_BRIDGE_H
#define FOREVERTAS_APP_MCP_BRIDGE_H

// The MCP (Model Context Protocol) bridge: an MCP client (through the stdio
// relay, mcp_relay.h) drives the same SearchController the window uses, and
// simulates scripts headlessly on its own thread. Off unless turned on in
// Edit > Preferences > MCP (or for one session with --enable-mcp, as the relay
// starts the window): off, it has no socket, thread or timer at all. See
// docs/MCP.md.

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QQmlApplicationEngine;
class QThreadPool;

namespace forevertas::app {

class SearchController;
struct McpSimulationCache;

class McpBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool listening READ listening NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(int clientCount READ clientCount NOTIFY changed)
    Q_PROPERTY(QString clientCommand READ clientCommand CONSTANT)
    Q_PROPERTY(QString clientConfig READ clientConfig CONSTANT)

public:
    // sessionEnabled: on for this run without changing the saved choice.
    McpBridge(SearchController &controller, QQmlApplicationEngine *engine, bool sessionEnabled,
              QObject *parent = nullptr);
    ~McpBridge() override;

    bool enabled() const { return enabled_; }
    // Saved; takes effect at once.
    void setEnabled(bool enabled);
    bool listening() const;
    QString status() const { return status_; }
    int clientCount() const { return static_cast<int>(buffers_.size()); }
    // What an MCP client runs for ForeverTAS, and a ready configuration.
    QString clientCommand() const;
    QString clientConfig() const;

signals:
    void changed();

private:
    using Reply = std::function<void(const QJsonObject &payload, bool isError)>;

    void apply();
    void startListening();
    void stopListening();
    void setStatus(const QString &status);
    void acceptConnections();
    void readSocket(QLocalSocket *socket);
    void handleMessage(const QPointer<QLocalSocket> &socket, const QJsonObject &message);
    void callTool(const QString &name, const QJsonObject &arguments, const QString &requestKey,
                  Reply reply);

    void toolGetStatus(const QJsonObject &arguments, Reply reply);
    void toolListComponents(const QJsonObject &arguments, Reply reply);
    void toolGetDocumentation(const QJsonObject &arguments, Reply reply);
    void toolLoadScenario(const QJsonObject &arguments, Reply reply);
    void toolGetMapLayout(const QJsonObject &arguments, Reply reply);
    void toolGetInputScript(const QJsonObject &arguments, Reply reply);
    void toolSetInputScript(const QJsonObject &arguments, Reply reply);
    void toolUseBestAsBase(const QJsonObject &arguments, Reply reply);
    void toolSimulate(const QJsonObject &arguments, Reply reply);
    void toolConfigureSearch(const QJsonObject &arguments, Reply reply);
    void toolEvaluateBase(const QJsonObject &arguments, Reply reply);
    void toolStartSearch(const QJsonObject &arguments, Reply reply);
    void toolWaitForSearch(const QJsonObject &arguments, Reply reply);
    void toolStopSearch(const QJsonObject &arguments, Reply reply);
    void toolGetBestResult(const QJsonObject &arguments, Reply reply);
    void toolGetSearchHistory(const QJsonObject &arguments, Reply reply);
    void toolSaveInputScript(const QJsonObject &arguments, Reply reply);
    void toolCaptureWindow(const QJsonObject &arguments, Reply reply);

    QJsonObject statusObject(bool includeInputs) const;
    QJsonObject configurationObject() const;
    QJsonObject searchProgressObject() const;
    std::uint32_t configuredHorizonMs() const;

    // Checks `done` every 100 ms on this thread until it holds or the
    // timeout passes, then calls `finish(timedOut)`. Ends early (timed out)
    // when MCP is turned off.
    void waitUntil(std::function<bool()> done, int timeoutMs, std::function<void(bool)> finish);
    // Runs `work` on the simulation thread; its result (or failure) comes
    // back here. The request's cancel flag, MCP turned off and the app
    // closing all stop it at the next tick.
    using SimulationWork =
            std::function<QJsonObject(McpSimulationCache &, const std::atomic<bool> &cancelled)>;
    void runSimulationJob(const QString &requestKey, SimulationWork work, Reply reply);
    // Stops every running simulation at its next tick.
    void cancelJobs();

    SearchController &controller_;
    QPointer<QQmlApplicationEngine> engine_;
    bool enabled_ = false;
    QString status_;
    QLocalServer *server_ = nullptr;
    QTimer retry_;
    QTimer workersIdle_;
    QHash<QLocalSocket *, QByteArray> buffers_;
    std::unique_ptr<QThreadPool> simulationPool_;
    std::shared_ptr<McpSimulationCache> cache_;
    // Cancel flags of running simulations, by request id.
    QHash<QString, std::shared_ptr<std::atomic<bool>>> jobs_;
    quint64 internalJobs_ = 0u;
    // The request a tool runs for, while its handler runs.
    QString currentRequest_;
};

}  // namespace forevertas::app

#endif
