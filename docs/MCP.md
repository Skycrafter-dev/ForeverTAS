# MCP: AI assistants driving ForeverTAS

ForeverTAS speaks the [Model Context Protocol](https://modelcontextprotocol.io):
an MCP client (Claude Desktop, Claude Code, or any other) can load maps, test
input scripts, set up and run searches, and read the results. It drives the
same controls you see, so everything it changes shows in the window.

## Turning it on

MCP is off by default. Off, ForeverTAS has no socket, thread or timer for it,
and costs nothing. Turn it on in **Edit > Preferences > MCP**; the choice is
kept. The same page shows the client configuration to copy:

- **Linux (AppImage):** the command is the AppImage with `--mcp-stdio`:

  ```json
  {"mcpServers": {"forevertas": {"command": "/path/to/ForeverTAS.AppImage",
                                  "args": ["--mcp-stdio"]}}}
  ```

- **Windows:** the command is `forevertas-mcp.exe` in the ForeverTAS folder:

  ```json
  {"mcpServers": {"forevertas": {"command": "C:\\...\\ForeverTAS\\forevertas-mcp.exe"}}}
  ```

The client starts that program (the relay) and talks to it over standard
input and output; the relay talks to the ForeverTAS window over a local socket
that only your user account can reach.

- With **ForeverTAS closed**, the first tool call opens it with MCP on for that
  session (your saved choice stays as it was).
- With **ForeverTAS open and MCP off**, the call fails with a message saying
  so; the relay never opens a second window.
- If ForeverTAS closes while a call runs, that call fails; the next one opens
  it again.

Set `FOREVERTAS_MCP_NO_LAUNCH=1` in the client's environment for the relay to
only ever use a window that is already open.

## What it can do

`tools/list` describes every tool. In short: `get_status`, `list_components`,
`get_documentation`, `load_scenario`, `get_map_layout`,
`get_input_script`/`set_input_script`, `use_best_as_base`, `simulate`,
`configure_search`, `evaluate_base`, `start_search`/`wait_for_search`/
`stop_search`, `get_best_result`, `get_search_history`, `save_input_script`
and `capture_window`.

`simulate` runs scripts on its own copy of the physics (Optimized CPU), never
changing what the window shows: one script with telemetry and an optional CSV,
or a batch of up to 256 in parallel; snapshots let later runs continue from a
point instead of the start; gates score each run as the Gates target would
(see [GATES.md](GATES.md)).

## Clocks

- **Script times** (input timeline): 0 is the race's first input tick.
  Telemetry `t_ms`, modifier windows (`minTimeMs`, `maxTimeMs`, ...) and
  `snapshot_at_ms` use it.
- **Simulation times**: evaluation-target settings use these, as in the
  window.

## Limits

These keep the window responsive whatever a client asks:

- A `simulate` call computes for at most 120 s (a batch, 300 s), then fails
  with a message; a request the client cancels stops at the next tick.
- Simulations run one at a time on their own thread, never the window's.
- At most 8 clients at once; a message over 64 MB closes its connection.
- Copies of the physics kept for batches are dropped two minutes after the
  last simulation; up to 64 snapshots are kept per map.
- Waiting tools (`wait_for_search`, `stop_search`, `evaluate_base`) wait at
  most 290 s per call.

## Files

`csv_path`, `save_input_script`'s `path` and `capture_window`'s `path` take an
absolute path as given; a bare file name or a relative path goes to
`Documents/ForeverTAS/MCP/` (also where `"auto"` and the defaults go).

## Safety

Only your user account reaches the socket. A client can do what you can do in
the window, including writing files where you can; give MCP access only to
clients you trust. Turning MCP off disconnects every client at once.

## Troubleshooting

- **"ForeverTAS is open with MCP off":** turn it on in Edit > Preferences >
  MCP, or close ForeverTAS and call again.
- **"Another ForeverTAS window answers MCP clients":** a second ForeverTAS of
  yours has MCP on too; one answers, the other takes over when it closes.
- The log (Help > Open log folder) has a line for MCP turned on or off,
  clients connecting and leaving, and every failed tool call.
