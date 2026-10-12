// forevertas-mcp: stdio Model Context Protocol server for ForeverTAS.
//
// MCP clients (Claude desktop, Claude Code, ...) launch this console program
// and speak newline-delimited JSON-RPC over stdin/stdout. `initialize`,
// `ping` and `tools/list` are answered here so the client can start without
// the GUI; every `tools/call` is forwarded to the running ForeverTAS
// application through its local socket, starting the application first when
// it is not running.

#include "app/claude_tool_catalog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using forevertas::app::ClaudeInitializeResult;
using forevertas::app::ClaudeToolCatalog;
using forevertas::app::kClaudeBridgeSocketName;

constexpr int kLaunchTimeoutMs = 180000;
constexpr int kRetryIntervalMs = 500;

void WriteLine(const QByteArray &line) {
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void WriteMessage(const QJsonObject &message) {
    WriteLine(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

QJsonObject ToolError(const QJsonValue &id, const QString &text) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"),
             QJsonObject{{QStringLiteral("content"),
                          QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                 {QStringLiteral("text"), text}}}},
                         {QStringLiteral("isError"), true}}}};
}

QString IdKey(const QJsonValue &id) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact));
}

class Relay final : public QObject {
public:
    explicit Relay(bool allowLaunch) : allowLaunch_(allowLaunch) {
        connect(&socket_, &QLocalSocket::readyRead, this, [this] { readSocket(); });
        connect(&socket_, &QLocalSocket::connected, this, [this] { flushQueue(); });
        connect(&socket_, &QLocalSocket::disconnected, this, [this] {
            launched_ = false;
            failPending(QStringLiteral("ForeverTAS closed while the request was running."));
        });
        connect(&socket_, &QLocalSocket::errorOccurred, this,
                [this](QLocalSocket::LocalSocketError) {
                    if (socket_.state() == QLocalSocket::UnconnectedState && connecting_) {
                        QTimer::singleShot(kRetryIntervalMs, this, [this] { tryConnect(); });
                    }
                });
    }

    void handleLine(const QByteArray &line) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty()) return;
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            WriteMessage({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), QJsonValue::Null},
                          {QStringLiteral("error"),
                           QJsonObject{{QStringLiteral("code"), -32700},
                                       {QStringLiteral("message"), QStringLiteral("Parse error")}}}});
            return;
        }
        const QJsonObject message = document.object();
        const QString method = message.value(QStringLiteral("method")).toString();
        const bool hasId = message.contains(QStringLiteral("id"));
        const QJsonValue id = message.value(QStringLiteral("id"));
        if (!hasId) {
            // Notifications need no answer; the GUI ignores them as well.
            return;
        }
        const auto respond = [&id](const QJsonObject &result) {
            WriteMessage({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), id},
                          {QStringLiteral("result"), result}});
        };
        if (method == QLatin1String("initialize")) {
            respond(ClaudeInitializeResult(message.value(QStringLiteral("params")).toObject(),
                                           QStringLiteral(FOREVERTAS_VERSION)));
        } else if (method == QLatin1String("ping")) {
            respond(QJsonObject{});
        } else if (method == QLatin1String("tools/list")) {
            respond(QJsonObject{{QStringLiteral("tools"), ClaudeToolCatalog()}});
        } else if (method == QLatin1String("resources/list")) {
            respond(QJsonObject{{QStringLiteral("resources"), QJsonArray{}}});
        } else if (method == QLatin1String("prompts/list")) {
            respond(QJsonObject{{QStringLiteral("prompts"), QJsonArray{}}});
        } else if (method == QLatin1String("tools/call")) {
            pending_.insert(IdKey(id), id);
            queue_.append(trimmed);
            if (socket_.state() == QLocalSocket::ConnectedState) {
                flushQueue();
            } else if (!connecting_) {
                connecting_ = true;
                connectStarted_ = QDateTime::currentMSecsSinceEpoch();
                tryConnect();
            }
        } else {
            WriteMessage({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), id},
                          {QStringLiteral("error"),
                           QJsonObject{{QStringLiteral("code"), -32601},
                                       {QStringLiteral("message"),
                                        QStringLiteral("Method not found: %1").arg(method)}}}});
        }
    }

private:
    void tryConnect() {
        if (socket_.state() != QLocalSocket::UnconnectedState) return;
        const qint64 waited = QDateTime::currentMSecsSinceEpoch() - connectStarted_;
        if (waited > kLaunchTimeoutMs) {
            connecting_ = false;
            queue_.clear();
            failPending(QStringLiteral("ForeverTAS did not start within %1 s. Open it manually "
                                       "and retry.").arg(kLaunchTimeoutMs / 1000));
            return;
        }
        if (!launched_ && waited >= 1000 && allowLaunch_) {
            launched_ = true;
            const QString executable = QDir(QCoreApplication::applicationDirPath())
                                               .filePath(QStringLiteral("ForeverTAS.exe"));
            const QString program = QFileInfo::exists(executable)
                    ? executable
                    : QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("ForeverTAS"));
            if (!QProcess::startDetached(program, {}, QCoreApplication::applicationDirPath())) {
                connecting_ = false;
                queue_.clear();
                failPending(QStringLiteral("Could not launch %1").arg(program));
                return;
            }
        }
        socket_.connectToServer(QString::fromLatin1(kClaudeBridgeSocketName));
    }

    void flushQueue() {
        connecting_ = false;
        for (const QByteArray &line : std::as_const(queue_)) {
            socket_.write(line + '\n');
        }
        queue_.clear();
        socket_.flush();
    }

    void readSocket() {
        buffer_.append(socket_.readAll());
        qsizetype newline = -1;
        while ((newline = buffer_.indexOf('\n')) >= 0) {
            const QByteArray line = buffer_.left(newline).trimmed();
            buffer_.remove(0, newline + 1);
            if (line.isEmpty()) continue;
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject()) {
                pending_.remove(IdKey(document.object().value(QStringLiteral("id"))));
            }
            WriteLine(line);
        }
    }

    void failPending(const QString &text) {
        for (const QJsonValue &id : std::as_const(pending_)) WriteMessage(ToolError(id, text));
        pending_.clear();
    }

    QLocalSocket socket_;
    QByteArray buffer_;
    QList<QByteArray> queue_;
    QHash<QString, QJsonValue> pending_;
    bool allowLaunch_ = true;
    bool launched_ = false;
    bool connecting_ = false;
    qint64 connectStarted_ = 0;
};

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("forevertas-mcp"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FOREVERTAS_VERSION));
    const bool allowLaunch = !application.arguments().contains(QStringLiteral("--no-launch")) &&
            qEnvironmentVariableIntValue("FOREVERTAS_MCP_NO_LAUNCH") == 0;
    Relay relay(allowLaunch);

    std::thread reader([&relay] {
        std::string line;
        while (std::getline(std::cin, line)) {
            QByteArray bytes(line.data(), static_cast<qsizetype>(line.size()));
            QMetaObject::invokeMethod(&relay, [&relay, bytes] { relay.handleLine(bytes); },
                                      Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::quit(); },
                                  Qt::QueuedConnection);
    });
    reader.detach();
    return application.exec();
}
