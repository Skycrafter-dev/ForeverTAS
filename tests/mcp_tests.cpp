// The MCP bridge and relay, in process, on a socket of their own
// (FOREVERTAS_MCP_SOCKET): off by default and costing nothing; the protocol,
// tools and their argument checks; simulations, batches and cancelling one;
// never taking another window's socket, reclaiming a crashed one's; turning
// off and on with a client connected; the relay with no window, a window with
// MCP off, and one it cannot start. With PACKS REPLAY, also on a map.
#include "app/app_settings.h"
#include "app/mcp_bridge.h"
#include "app/mcp_protocol.h"
#include "app/mcp_relay.h"
#include "app/search_controller.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QStandardPaths>
#include <QThread>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using forevertas::app::McpBridge;
using forevertas::app::McpRelay;

void Check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

bool WaitFor(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

// A client of the bridge: one request, its answer (the event loop runs while
// it waits: the bridge is on this thread).
class Client {
public:
    bool connect() {
        socket_.connectToServer(forevertas::app::McpSocketName());
        return WaitFor([this] { return socket_.state() == QLocalSocket::ConnectedState; }, 2000);
    }
    QJsonObject call(const QJsonObject &request, int timeoutMs = 20000) {
        send(request);
        const QJsonValue id = request.value(QStringLiteral("id"));
        QJsonObject answer;
        Check(WaitFor([&] {
                  while (socket_.canReadLine()) {
                      const QJsonObject message = QJsonDocument::fromJson(socket_.readLine()).object();
                      if (message.value(QStringLiteral("id")) == id) {
                          answer = message;
                          return true;
                      }
                  }
                  return false;
              }, timeoutMs),
              "no answer to " + request.value(QStringLiteral("method")).toString().toStdString());
        return answer;
    }
    void send(const QJsonObject &message) {
        socket_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        socket_.flush();
    }
    void sendRaw(const QByteArray &line) {
        socket_.write(line + '\n');
        socket_.flush();
    }
    QJsonObject readAny(int timeoutMs = 5000) {
        QJsonObject answer;
        WaitFor([&] {
            if (!socket_.canReadLine()) return false;
            answer = QJsonDocument::fromJson(socket_.readLine()).object();
            return true;
        }, timeoutMs);
        return answer;
    }
    QLocalSocket socket_;
};

int nextId = 1;

QJsonObject Request(const QString &method, const QJsonObject &params = {}) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), nextId++},
            {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
}

QJsonObject Tool(const QString &name, const QJsonObject &arguments = {}) {
    return Request(QStringLiteral("tools/call"),
                   {{QStringLiteral("name"), name}, {QStringLiteral("arguments"), arguments}});
}

// A tool's answer: its JSON payload, and whether it failed.
std::pair<QJsonObject, bool> Payload(const QJsonObject &answer) {
    const QJsonObject result = answer.value(QStringLiteral("result")).toObject();
    const QString text = result.value(QStringLiteral("content")).toArray().at(0).toObject()
                                 .value(QStringLiteral("text")).toString();
    return {QJsonDocument::fromJson(text.toUtf8()).object(), result.value(QStringLiteral("isError")).toBool()};
}

}  // namespace

int main(int argc, char **argv) {
    qputenv("FOREVERTAS_MCP_SOCKET", QByteArrayLiteral("forevertas-mcp-tests-") +
                                             QByteArray::number(QCoreApplication::applicationPid()));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("McpTests"));
    QStandardPaths::setTestModeEnabled(true);
    forevertas::app::AppSettings::Instance().clear();
    const bool withMap = argc == 3;
    try {
        forevertas::app::SearchController controller;
        QQmlApplicationEngine engine;

        // Off by default: nothing listens.
        {
            McpBridge bridge(controller, &engine, false);
            Check(!bridge.enabled() && !bridge.listening(), "MCP was on by default");
            Client client;
            Check(!client.connect(), "a client reached MCP while it was off");
        }
        std::cout << "off by default: nothing listens\n";

        // A socket left by a crashed window is reclaimed.
#if !defined(Q_OS_WIN)
        {
            QFile stale(QDir(QDir::tempPath()).filePath(forevertas::app::McpSocketName()));
            Check(stale.open(QIODevice::WriteOnly), "could not leave a stale socket");
            stale.close();
        }
#endif
        McpBridge bridge(controller, &engine, true);
        Check(bridge.listening(), "MCP did not listen once on (a stale socket in the way?)");
        Client client;
        Check(client.connect(), "a client could not connect");

        // The protocol.
        const QJsonObject initialize = client.call(Request(QStringLiteral("initialize"),
                                                           {{QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")}}));
        Check(initialize.value(QStringLiteral("result")).toObject().value(QStringLiteral("protocolVersion")).toString() ==
                      QStringLiteral("2025-06-18"),
              "initialize did not agree on the protocol version");
        const QJsonArray tools =
                client.call(Request(QStringLiteral("tools/list"))).value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
        Check(tools.size() == 18, "tools/list has " + std::to_string(tools.size()) + " tools");
        client.sendRaw("{this is not json");
        Check(client.readAny().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() == -32700,
              "a malformed line got no parse error");
        Check(client.call(Request(QStringLiteral("no/such/method"))).value(QStringLiteral("error")).toObject()
                              .value(QStringLiteral("code")).toInt() == -32601,
              "an unknown method got no error");
        {
            QJsonObject bad = Request(QStringLiteral("tools/call"),
                                      {{QStringLiteral("name"), QStringLiteral("get_status")},
                                       {QStringLiteral("arguments"), QStringLiteral("nope")}});
            Check(client.call(bad).value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() == -32602,
                  "arguments that are not an object were accepted");
        }
        Check(Payload(client.call(Tool(QStringLiteral("no_such_tool")))).second, "an unknown tool did not fail");
        const auto [status, statusFailed] = Payload(client.call(Tool(QStringLiteral("get_status"))));
        Check(!statusFailed && status.contains(QStringLiteral("configuration")), "get_status failed");
        // A script that does not parse is refused and the base is kept.
        const QString base = controller.baseInputScript();
        Check(Payload(client.call(Tool(QStringLiteral("set_input_script"),
                                       {{QStringLiteral("script"), QStringLiteral("0.00 frobnicate up")}}))).second &&
                      controller.baseInputScript() == base,
              "a broken script was applied");
        Check(Payload(client.call(Tool(QStringLiteral("set_input_script"), {{QStringLiteral("script"), 42}}))).second,
              "a number was taken for a script");
        Check(Payload(client.call(Tool(QStringLiteral("simulate"), {{QStringLiteral("scripts"), QJsonArray{1, 2}}}))).second,
              "numbers were taken for scripts");
        // documentation: bundled with the app, so this test gets a clean refusal
        {
            const auto [docs, docsFailed] = Payload(client.call(Tool(QStringLiteral("get_documentation"),
                                                                     {{QStringLiteral("topic"), QStringLiteral("mcp")}})));
            Check(docsFailed ? docs.value(QStringLiteral("error")).toString().contains(QStringLiteral("bundled"))
                             : docs.value(QStringLiteral("markdown")).toString().contains(QStringLiteral("MCP")),
                  "get_documentation gave neither the page nor a clear refusal");
        }
        std::cout << "protocol, tools and argument checks hold\n";

        // A second window of the same user never takes the socket.
        {
            McpBridge second(controller, &engine, true);
            Check(!second.listening() && second.status().contains(QStringLiteral("Another")),
                  "a second window took the socket");
            Check(Payload(client.call(Tool(QStringLiteral("get_status")))).first.contains(QStringLiteral("version")),
                  "the first window stopped answering");
        }
        std::cout << "a second window leaves the socket to the first\n";

        if (withMap) {
            const auto [loaded, loadFailed] = Payload(client.call(
                    Tool(QStringLiteral("load_scenario"),
                         {{QStringLiteral("path"), QString::fromLocal8Bit(argv[2])},
                          {QStringLiteral("packs_directory"), QString::fromLocal8Bit(argv[1])},
                          {QStringLiteral("extract_inputs"), true}}),
                    60000));
            Check(!loadFailed, "load_scenario failed: " + QJsonDocument(loaded).toJson().toStdString());
            const QJsonObject baseResult = loaded.value(QStringLiteral("base_script_result")).toObject();
            Check(baseResult.value(QStringLiteral("checkpoints_collected")).toInt() > 0,
                  "the replay's own inputs collected no checkpoint");
            const QString script = loaded.value(QStringLiteral("base_script")).toString();
            const auto [one, oneFailed] = Payload(client.call(Tool(QStringLiteral("simulate"),
                                                                   {{QStringLiteral("script"), script},
                                                                    {QStringLiteral("horizon_ms"), 5000}})));
            Check(!oneFailed && one.value(QStringLiteral("samples")).toArray().size() > 5, "simulate gave no telemetry");
            const auto [batch, batchFailed] = Payload(client.call(Tool(
                    QStringLiteral("simulate"),
                    {{QStringLiteral("scripts"), QJsonArray{script, QString(), script}},
                     {QStringLiteral("horizon_ms"), 3000},
                     {QStringLiteral("threads"), 3}})));
            const QJsonArray results = batch.value(QStringLiteral("results")).toArray();
            Check(!batchFailed && results.size() == 3 &&
                          results[0].toObject().value(QStringLiteral("end_position")) ==
                                  results[2].toObject().value(QStringLiteral("end_position")),
                  "a batch's identical scripts gave different results");
            // A cancelled request stops at the next tick.
            QJsonArray many;
            for (int index = 0; index < 256; ++index) many.append(script);
            const QJsonObject heavy = Tool(QStringLiteral("simulate"),
                                           {{QStringLiteral("scripts"), many}, {QStringLiteral("threads"), 1}});
            client.send(heavy);
            WaitFor([] { return false; }, 300);
            client.send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"), QStringLiteral("notifications/cancelled")},
                         {QStringLiteral("params"), QJsonObject{{QStringLiteral("requestId"), heavy.value(QStringLiteral("id"))}}}});
            QElapsedTimer cancelling;
            cancelling.start();
            QJsonObject cancelled;
            Check(WaitFor([&] {
                      const QJsonObject answer = client.readAny(50);
                      if (answer.value(QStringLiteral("id")) == heavy.value(QStringLiteral("id"))) cancelled = answer;
                      return !cancelled.isEmpty();
                  }, 20000),
                  "the cancelled batch never answered");
            const QJsonArray partial = Payload(cancelled).first.value(QStringLiteral("results")).toArray();
            bool anyCancelled = Payload(cancelled).second;
            for (const QJsonValue &entry : partial)
                anyCancelled |= entry.toObject().value(QStringLiteral("error")).toString() == QStringLiteral("Cancelled");
            Check(anyCancelled, "a cancelled batch ran to its end");
            std::cout << "simulate, batches and cancelling work on the map (cancelled in "
                      << cancelling.elapsed() << " ms)\n";
        }

        // The Gates target is refused with a GPU backend or approximate physics.
        {
            const auto [configured, configureFailed] = Payload(client.call(Tool(
                    QStringLiteral("configure_search"),
                    {{QStringLiteral("backend"), QStringLiteral("cuda")},
                     {QStringLiteral("target"), QJsonObject{{QStringLiteral("id"), QStringLiteral("gate-entry")},
                                                            {QStringLiteral("settings"),
                                                             QJsonObject{{QStringLiteral("gates"),
                                                                          QJsonArray{QJsonObject{{QStringLiteral("x"), 0},
                                                                                                 {QStringLiteral("y"), 0},
                                                                                                 {QStringLiteral("z"), 0},
                                                                                                 {QStringLiteral("dir"), 0}}}}}}}}})));
            const bool onGpu = configured.value(QStringLiteral("configuration")).toObject()
                                       .value(QStringLiteral("backend")).toString() == QStringLiteral("cuda");
            Check(!configureFailed && (!onGpu || configured.value(QStringLiteral("validation_message")).toString()
                                                          .contains(QStringLiteral("Gates target runs on the CPU"))),
                  "the Gates target was not refused on a GPU backend");
            std::cout << (onGpu ? "the Gates target is refused on a GPU backend\n"
                                : "(no CUDA here: the Gates refusal on a GPU backend is not checked)\n");
            client.call(Tool(QStringLiteral("configure_search"), {{QStringLiteral("backend"), QStringLiteral("optimized-cpu")}}));
        }

        // Off with a client connected: it is disconnected and nothing listens;
        // on again, it listens.
        bridge.setEnabled(false);
        Check(WaitFor([&] { return client.socket_.state() == QLocalSocket::UnconnectedState; }, 3000) &&
                      !bridge.listening(),
              "turning MCP off left a client connected or a socket open");
        bridge.setEnabled(true);
        Client again;
        Check(bridge.listening() && again.connect(), "MCP did not come back on");
        std::cout << "off and on again with a client connected\n";

        // The relay.
        const auto relayCall = [](McpRelay::Options options, const QJsonObject &request, int timeoutMs) {
            QJsonObject answer;
            McpRelay relay(std::move(options), [&](const QByteArray &line) {
                const QJsonObject message = QJsonDocument::fromJson(line).object();
                if (message.value(QStringLiteral("id")) == request.value(QStringLiteral("id"))) answer = message;
            });
            relay.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact));
            WaitFor([&] { return !answer.isEmpty(); }, timeoutMs);
            return answer;
        };
        McpRelay::Options options;
        options.socketName = forevertas::app::McpSocketName();
        options.appRunning = [] { return true; };
        // tools/list without a window at all
        Check(relayCall(options, Request(QStringLiteral("tools/list")), 1000).value(QStringLiteral("result")).toObject()
                              .value(QStringLiteral("tools")).toArray().size() == 18,
              "the relay did not list the tools itself");
        Check(Payload(relayCall(options, Tool(QStringLiteral("get_status")), 5000)).first.contains(QStringLiteral("version")),
              "the relay did not reach the window");
        bridge.setEnabled(false);
        options.openWindowGraceMs = 500;
        const auto [off, offFailed] = Payload(relayCall(options, Tool(QStringLiteral("get_status")), 5000));
        Check(offFailed && off.value(QStringLiteral("error")).toString().contains(QStringLiteral("MCP off")),
              "the relay did not say the open window has MCP off");
        options.appRunning = [] { return false; };
        options.appProgram = {};
        Check(Payload(relayCall(options, Tool(QStringLiteral("get_status")), 3000)).first.value(QStringLiteral("error"))
                      .toString().contains(QStringLiteral("not open")),
              "the relay did not say ForeverTAS is not open");
        options.appProgram = QStringLiteral("/bin/true");
        options.launchTimeoutMs = 1500;
        Check(Payload(relayCall(options, Tool(QStringLiteral("get_status")), 6000)).first.value(QStringLiteral("error"))
                      .toString().contains(QStringLiteral("did not open")),
              "the relay did not give up on a window that never opened");
        std::cout << "the relay: answers itself, reaches the window, says when MCP is off or nothing opens\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        forevertas::app::AppSettings::Instance().clear();
        return 1;
    }
    forevertas::app::AppSettings::Instance().clear();
    return 0;
}
