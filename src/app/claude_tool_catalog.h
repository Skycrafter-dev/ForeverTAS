#ifndef FOREVERTAS_APP_CLAUDE_TOOL_CATALOG_H
#define FOREVERTAS_APP_CLAUDE_TOOL_CATALOG_H

// Model Context Protocol (MCP) surface of ForeverTAS. The catalog is Qt Core
// only so that the stdio relay (forevertas-mcp) can answer `initialize` and
// `tools/list` without starting the GUI; `tools/call` is executed by the
// ClaudeBridge inside the running application.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace forevertas::app {

// Local socket (a named pipe on Windows) served by the running application.
inline constexpr char kClaudeBridgeSocketName[] = "ForeverTAS-Claude";
inline constexpr char kClaudeBridgeServerName[] = "forevertas";
inline constexpr char kClaudeBridgeDefaultProtocolVersion[] = "2025-06-18";

QJsonArray ClaudeToolCatalog();
QString ClaudeServerInstructions();
QJsonObject ClaudeInitializeResult(const QJsonObject &params,
                                   const QString &serverVersion);

}  // namespace forevertas::app

#endif
