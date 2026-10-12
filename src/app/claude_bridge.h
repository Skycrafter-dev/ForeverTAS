#ifndef FOREVERTAS_APP_CLAUDE_BRIDGE_H
#define FOREVERTAS_APP_CLAUDE_BRIDGE_H

// In-process Model Context Protocol server. It listens on a per-user local
// socket (named pipe on Windows) and lets an MCP client such as Claude drive
// the same SearchController the GUI uses, plus headless analysis tools that
// simulate input scripts on the optimized CPU engine.

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QQmlApplicationEngine;

namespace forevertas::app {

class SearchController;
struct ClaudeSimulationCache;

class ClaudeBridge final : public QObject {
    Q_OBJECT

public:
    ClaudeBridge(SearchController &controller,
                 QQmlApplicationEngine &engine,
                 QObject *parent = nullptr);
    ~ClaudeBridge() override;

    bool listen();

private:
    using Reply = std::function<void(const QJsonObject &payload, bool isError)>;

    void acceptConnections();
    void readSocket(QLocalSocket *socket);
    void handleMessage(const QPointer<QLocalSocket> &socket,
                       const QJsonObject &message);
    void callTool(const QString &name, const QJsonObject &arguments,
                  Reply reply);

    // Tools.
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

    // Polls `done` every 100 ms on the GUI thread until it holds or the
    // timeout elapses, then calls `finish(timedOut)`.
    void waitUntil(std::function<bool()> done, int timeoutMs,
                   std::function<void(bool)> finish);
    // Runs `work` on the dedicated simulation thread and delivers its result
    // (or exception text) on the GUI thread.
    void runSimulationJob(std::function<QJsonObject(ClaudeSimulationCache &)> work,
                          Reply reply);

    SearchController &controller_;
    QQmlApplicationEngine &engine_;
    QLocalServer *server_ = nullptr;
    QHash<QLocalSocket *, QByteArray> buffers_;
    QThreadPool simulationPool_;
    std::shared_ptr<ClaudeSimulationCache> cache_;
};

}  // namespace forevertas::app

#endif
