#ifndef FOREVERTAS_APP_MCP_PROTOCOL_H
#define FOREVERTAS_APP_MCP_PROTOCOL_H

// What the MCP (Model Context Protocol) bridge in the app and the stdio relay
// an MCP client runs share: where they meet, the tools, and the initialize
// answer. See docs/MCP.md.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace forevertas::app {

inline constexpr char kMcpServerName[] = "forevertas";
inline constexpr char kMcpDefaultProtocolVersion[] = "2025-06-18";

// The app's local socket (a named pipe on Windows), one per user: other users'
// ForeverTAS never take it, and theirs is never reachable from here.
QString McpSocketName();

// The file a running ForeverTAS window holds (whether MCP is on or not), so
// the relay knows not to start a second one.
QString McpPresenceLockPath();
// Whether a ForeverTAS window holds it now.
bool McpAppRunning();

// Where the bridge writes files given without a folder (CSV, screenshots).
QString McpOutputDirectory();

QJsonArray McpToolCatalog();
QString McpServerInstructions();
QJsonObject McpInitializeResult(const QJsonObject &params, const QString &serverVersion);

// A JSON-RPC answer, and a tool's answer (text content, isError on failure).
QJsonObject McpResponse(const QJsonValue &id, const QJsonObject &result);
QJsonObject McpErrorResponse(const QJsonValue &id, int code, const QString &message);
QJsonObject McpToolResult(const QJsonObject &payload, bool isError);

}  // namespace forevertas::app

#endif
