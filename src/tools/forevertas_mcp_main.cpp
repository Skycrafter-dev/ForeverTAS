// forevertas-mcp: the stdio MCP server on Windows (see app/mcp_relay.h).
#include "app/mcp_relay.h"

int main(int argc, char **argv) {
    return forevertas::app::RunMcpRelay(argc, argv, QStringLiteral(FOREVERTAS_VERSION));
}
