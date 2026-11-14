#ifndef FOREVERTAS_APP_MCP_RELAY_H
#define FOREVERTAS_APP_MCP_RELAY_H

// The stdio MCP server an MCP client runs (ForeverTAS --mcp-stdio, or
// forevertas-mcp on Windows): it answers initialize, ping and tools/list
// itself, so a client starts at once, and passes everything else to the
// ForeverTAS window over its local socket. With no window open it starts one
// with MCP on for that session; with one open and MCP off it says so, never
// starting a second.

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace forevertas::app {

class McpRelay final : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString socketName;
        // The window to start ({} = never start one), and its arguments.
        QString appProgram;
        QStringList appArguments;
        int launchTimeoutMs = 120000;
        // How long an open window with MCP off gets to turn it on.
        int openWindowGraceMs = 2500;
        // Whether a ForeverTAS window is open.
        std::function<bool()> appRunning;
    };
    using Writer = std::function<void(const QByteArray &line)>;

    McpRelay(Options options, Writer writer, QObject *parent = nullptr);
    ~McpRelay() override;

    // One line from the client (a JSON-RPC message).
    void handleLine(const QByteArray &line);
    // The client closed its end: finish once what it asked is answered.
    void endOfInput();

signals:
    void finished();

private:
    void write(const QJsonObject &message);
    void connectToApp();
    void tryConnect();
    void sendQueued();
    void readApp();
    void failPending(const QString &text);
    void finishIfDone();

    Options options_;
    Writer writer_;
    QLocalSocket socket_;
    QTimer retry_;
    QElapsedTimer connecting_;
    QByteArray buffer_;
    QList<QByteArray> queue_;
    // Requests sent to the window and not answered yet, by id.
    QHash<QString, QJsonValue> pending_;
    bool connectingNow_ = false;
    bool launched_ = false;
    bool inputEnded_ = false;
};

// What to start for a window: the AppImage when run from one, else
// ForeverTAS next to this program, else this program.
QString McpAppProgram();

// ForeverTAS --mcp-stdio / forevertas-mcp: the relay on stdin and stdout.
int RunMcpRelay(int argc, char **argv, const QString &version);

}  // namespace forevertas::app

#endif
