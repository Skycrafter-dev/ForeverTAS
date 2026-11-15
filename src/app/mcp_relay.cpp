#include "app/mcp_relay.h"

#include "app/mcp_protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

namespace forevertas::app {
namespace {

// A line larger than this is refused (a script batch is far below it).
constexpr qsizetype kMaximumLineBytes = 64 * 1024 * 1024;
constexpr int kRetryMs = 250;

QString IdKey(const QJsonValue &id) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact));
}

}  // namespace

McpRelay::McpRelay(Options options, Writer writer, QObject *parent)
    : QObject(parent), options_(std::move(options)), writer_(std::move(writer)) {
    retry_.setSingleShot(true);
    retry_.setInterval(kRetryMs);
    connect(&retry_, &QTimer::timeout, this, [this] { tryConnect(); });
    connect(&socket_, &QLocalSocket::connected, this, [this] {
        connectingNow_ = false;
        sendQueued();
    });
    connect(&socket_, &QLocalSocket::readyRead, this, [this] { readApp(); });
    connect(&socket_, &QLocalSocket::disconnected, this, [this] {
        buffer_.clear();
        // closed by the user, or crashed: a later call starts it again
        launched_ = false;
        failPending(QStringLiteral("ForeverTAS closed before answering. Call again to reopen it."));
        finishIfDone();
    });
    // (the error can come while the socket still says it is connecting)
    connect(&socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (connectingNow_) retry_.start();
    });
}

McpRelay::~McpRelay() {
    // the socket goes last and aborts as it goes: its signals must not reach
    // members already gone
    socket_.disconnect(this);
    retry_.stop();
}

void McpRelay::write(const QJsonObject &message) {
    writer_(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

void McpRelay::handleLine(const QByteArray &line) {
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;
    if (trimmed.size() > kMaximumLineBytes) {
        write(McpErrorResponse(QJsonValue::Null, -32600, QStringLiteral("Message too large")));
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        write(McpErrorResponse(QJsonValue::Null, -32700, QStringLiteral("Parse error")));
        return;
    }
    const QJsonObject message = document.object();
    const QString method = message.value(QStringLiteral("method")).toString();
    if (!message.contains(QStringLiteral("id"))) {
        // notifications (initialized, cancelled): the window acts on cancels
        if (socket_.state() == QLocalSocket::ConnectedState) socket_.write(trimmed + '\n');
        return;
    }
    const QJsonValue id = message.value(QStringLiteral("id"));
    if (method == QLatin1String("initialize")) {
        write(McpResponse(id, McpInitializeResult(message.value(QStringLiteral("params")).toObject(),
                                                  QCoreApplication::applicationVersion())));
    } else if (method == QLatin1String("ping")) {
        write(McpResponse(id, {}));
    } else if (method == QLatin1String("tools/list")) {
        write(McpResponse(id, {{QStringLiteral("tools"), McpToolCatalog()}}));
    } else if (method == QLatin1String("resources/list")) {
        write(McpResponse(id, {{QStringLiteral("resources"), QJsonArray{}}}));
    } else if (method == QLatin1String("prompts/list")) {
        write(McpResponse(id, {{QStringLiteral("prompts"), QJsonArray{}}}));
    } else if (method == QLatin1String("tools/call")) {
        pending_.insert(IdKey(id), id);
        queue_.append(trimmed);
        if (socket_.state() == QLocalSocket::ConnectedState) sendQueued();
        else connectToApp();
    } else {
        write(McpErrorResponse(id, -32601, QStringLiteral("Method not found: %1").arg(method)));
    }
}

void McpRelay::endOfInput() {
    inputEnded_ = true;
    finishIfDone();
}

void McpRelay::finishIfDone() {
    if (inputEnded_ && pending_.isEmpty() && queue_.isEmpty()) emit finished();
}

void McpRelay::connectToApp() {
    if (connectingNow_) return;
    connectingNow_ = true;
    connecting_.start();
    tryConnect();
}

void McpRelay::tryConnect() {
    if (!connectingNow_ || socket_.state() == QLocalSocket::ConnectedState) return;
    if (socket_.state() != QLocalSocket::UnconnectedState) {
        retry_.start();  // a connection on its way: see how it ends
        return;
    }
    const bool running = options_.appRunning ? options_.appRunning() : false;
    if (running && !launched_ && connecting_.elapsed() > options_.openWindowGraceMs) {
        connectingNow_ = false;
        failPending(QStringLiteral(
                "ForeverTAS is open with MCP off. Turn it on in Edit > Preferences > MCP, or close "
                "ForeverTAS and call again: it then starts with MCP on."));
        return;
    }
    if (!running && !launched_) {
        if (options_.appProgram.isEmpty()) {
            connectingNow_ = false;
            failPending(QStringLiteral("ForeverTAS is not open. Open it and turn MCP on in Edit > "
                                       "Preferences > MCP."));
            return;
        }
        launched_ = true;
        if (!QProcess::startDetached(options_.appProgram, options_.appArguments,
                                     QFileInfo(options_.appProgram).absolutePath())) {
            connectingNow_ = false;
            launched_ = false;
            failPending(QStringLiteral("Could not start ForeverTAS (%1).").arg(options_.appProgram));
            return;
        }
    }
    if (launched_ && connecting_.elapsed() > options_.launchTimeoutMs) {
        connectingNow_ = false;
        failPending(QStringLiteral("ForeverTAS did not open within %1 s. Open it, turn MCP on in Edit > "
                                   "Preferences > MCP, and call again.")
                            .arg(options_.launchTimeoutMs / 1000));
        return;
    }
    socket_.connectToServer(options_.socketName);
}

void McpRelay::sendQueued() {
    for (const QByteArray &line : std::as_const(queue_)) socket_.write(line + '\n');
    queue_.clear();
    socket_.flush();
}

void McpRelay::readApp() {
    buffer_.append(socket_.readAll());
    qsizetype newline = -1;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) pending_.remove(IdKey(document.object().value(QStringLiteral("id"))));
        writer_(line);
    }
    if (buffer_.size() > kMaximumLineBytes) {
        buffer_.clear();
        socket_.abort();
    }
    finishIfDone();
}

void McpRelay::failPending(const QString &text) {
    queue_.clear();
    const auto pending = pending_;
    pending_.clear();
    for (const QJsonValue &id : pending) {
        write(McpResponse(id, McpToolResult({{QStringLiteral("error"), text}}, true)));
    }
    finishIfDone();
}

QString McpAppProgram() {
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && QFileInfo(appImage).isFile()) return appImage;
    const QDir here(QCoreApplication::applicationDirPath());
    for (const QString &name : {QStringLiteral("ForeverTAS.exe"), QStringLiteral("ForeverTAS")}) {
        if (QFileInfo(here.filePath(name)).isFile()) return here.filePath(name);
    }
    return QCoreApplication::applicationFilePath();
}

int RunMcpRelay(int argc, char **argv, const QString &version) {
#if defined(Q_OS_WIN)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationVersion(version);
    McpRelay::Options options;
    options.socketName = McpSocketName();
    options.appRunning = &McpAppRunning;
    // FOREVERTAS_MCP_NO_LAUNCH=1: only ever talk to a window already open
    if (qEnvironmentVariableIntValue("FOREVERTAS_MCP_NO_LAUNCH") == 0) {
        options.appProgram = McpAppProgram();
        options.appArguments = {QStringLiteral("--enable-mcp")};
    }
    McpRelay relay(std::move(options), [](const QByteArray &line) {
        std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    });
    QObject::connect(&relay, &McpRelay::finished, &application, &QCoreApplication::quit,
                     Qt::QueuedConnection);
    std::thread reader([&relay] {
        std::string line;
        while (std::getline(std::cin, line)) {
            QByteArray bytes(line.data(), static_cast<qsizetype>(line.size()));
            QMetaObject::invokeMethod(&relay, [&relay, bytes] { relay.handleLine(bytes); },
                                      Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&relay, [&relay] { relay.endOfInput(); }, Qt::QueuedConnection);
    });
    reader.detach();
    return application.exec();
}

}  // namespace forevertas::app
