#include "app/claude_tool_catalog.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

#include <stdexcept>

namespace forevertas::app {
namespace {

// One JSON object per tool keeps every literal far below compiler limits.
const char *const kToolDefinitions[] = {
R"json({
 "name": "get_status",
 "description": "Snapshot of the ForeverTAS session: Packs directory, loaded replay/challenge, the current search configuration (backend, horizon, algorithm, modifier passes, evaluation target, conditions), whether a search is running, live metrics (attempts, attempts/s, elapsed), the current best summary, the last base evaluation and GPU availability. Call this first, and whenever you are unsure what state the application is in.",
 "inputSchema": {"type": "object", "properties": {
   "include_inputs": {"type": "boolean", "description": "Also return the full base input script and the best input script (can be long). Default false."}
 }}
})json",
R"json({
 "name": "list_components",
 "description": "Lists every search algorithm, input modifier and evaluation target that configure_search accepts, with ids, display names, GPU support and the complete default settings map of each (settings are string key/value pairs; times are milliseconds). Optionally includes the condition/custom-target property reference used by condition_script and the scripted-target 'script' setting.",
 "inputSchema": {"type": "object", "properties": {
   "include_condition_reference": {"type": "boolean", "description": "Include the full list of condition properties and functions. Default false."}
 }}
})json",
R"json({
 "name": "get_documentation",
 "description": "Returns ForeverTAS reference documentation bundled with the application. Topics: readme, search_components, conditions, gates, checkpoint_time, input_insertion, gpu_backends, gpu_safety. Use 'gates' before using the gate-entry target or simulate's gates; use 'conditions' before writing condition scripts or custom (scripted) targets.",
 "inputSchema": {"type": "object", "properties": {
   "topic": {"type": "string", "enum": ["readme", "search_components", "conditions", "gates", "checkpoint_time", "input_insertion", "gpu_backends", "gpu_safety"]}
 }, "required": ["topic"]}
})json",
R"json({
 "name": "load_scenario",
 "description": "Selects the map to work on: a replay (.Replay.Gbx) or a standalone challenge (.Challenge.Gbx). Optionally sets the TrackMania Nations/United Forever 'Packs' directory (auto-detected when omitted) and extracts the replay's inputs into the base input script. Returns map name, checkpoint count, laps, environment, the car's start position and, when inputs were extracted, the script plus its simulated result.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "Absolute path to a .Replay.Gbx or .Challenge.Gbx file."},
   "packs_directory": {"type": "string", "description": "Absolute path to the game's Packs directory. Omit to keep the current or auto-detected one."},
   "extract_inputs": {"type": "boolean", "description": "Replace the base input script with the replay's own inputs. Default false."}
 }, "required": ["path"]}
})json",
R"json({
 "name": "get_map_layout",
 "description": "Describes the loaded map geometry for route planning: start position/orientation and the key blocks (start, checkpoints, finish, turbo pads) as world-space axis-aligned boxes (center, size, yaw), optionally every placed block. World axes: Y is up; units are metres (a Stadium block is 32 x 8 x 32).",
 "inputSchema": {"type": "object", "properties": {
   "include_blocks": {"type": "boolean", "description": "Also list every placed block. Default false."},
   "max_blocks": {"type": "integer", "description": "Upper bound on listed blocks. Default 400."}
 }}
})json",
R"json({
 "name": "get_input_script",
 "description": "Returns the current base input script (the baseline the search mutates) and any parse error. Script format (TMInterface style), exactly one '<time> <command> <value>' per line: '<time> press|rel up|down|left|right', '<time> press enter' (respawn), '<time> steer <-65536..65536>' (negative = left), '<time> gas <-65536..65536>'. Times are seconds ('12.34'), mm:ss.mmm or hh:mm:ss.mmm, non-negative and aligned to 10 ms; 0 is the first input tick of the race. A state holds until changed. '#' and '//' start comments.",
 "inputSchema": {"type": "object", "properties": {}}
})json",
R"json({
 "name": "set_input_script",
 "description": "Replaces the base input script shown in the application (the search baseline). Returns the parse result. Set preview=true to also render the run in the Race Viewer for the user. Use simulate first if you only want to test a script without changing the application state.",
 "inputSchema": {"type": "object", "properties": {
   "script": {"type": "string"},
   "preview": {"type": "boolean", "description": "Show the base run in the Race Viewer. Default false."}
 }, "required": ["script"]}
})json",
R"json({
 "name": "use_best_as_base",
 "description": "Copies the current best result (from the running/finished search, or a cycle selected with get_search_history) into the base input script, so the next search refines it. This is the core loop of iterative TAS optimisation.",
 "inputSchema": {"type": "object", "properties": {
   "source": {"type": "string", "enum": ["best", "selected_cycle"], "description": "Default 'best'."}
 }}
})json",
R"json({
 "name": "simulate",
 "description": "Deterministically simulates input scripts on the optimized CPU engine (independent of the GUI, does not change application state) and reports the outcome: finish time (incl. sub-tick precise estimate), checkpoint times, respawns, max speed, airborne/sliding/wall-contact time, and sampled telemetry (position, velocity, km/h, yaw/pitch/roll, steer/gas/brake, gear, rpm, wheel contact, sliding, turbo, lateral_contact). Checkpoint entries give 'number' (1-based, = checkpoint-time target's checkpointIndex), 'race_time_ms' (the game's tick time) and 't_ms' (input timeline). Telemetry t_ms is on the input-script clock. Give 'script' for one run with telemetry, or 'scripts' (up to 256) to run a batch in parallel on cloned simulators. 'gates' reports honest gate crossings per run and the score the gate-entry target would give (see get_documentation gates), so a batch can be ranked without pulling telemetry. Snapshots: 'snapshot_at_ms' captures the physics state at that time of a single run and returns a snapshot_id; 'from_snapshot' continues any later script (or batch) from it, provided its inputs at or before that time are identical, so a controller or sweep only re-simulates the part that changes. Changing horizon_ms never reloads the map. A full per-tick CSV can be written for detailed analysis.",
 "inputSchema": {"type": "object", "properties": {
   "script": {"type": "string", "description": "Input script to simulate. Defaults to the current base input script."},
   "scripts": {"type": "array", "items": {"type": "string"}, "description": "Batch of scripts run in parallel (max 256). Summaries only unless include_samples is true."},
   "threads": {"type": "integer", "description": "Parallel simulators for 'scripts' (1-32). Default: CPU threads - 1."},
   "include_samples": {"type": "boolean", "description": "Also return telemetry samples for every script of a batch. Default false."},
   "gates": {"description": "Gates to evaluate on every run: an array of {x,y,z,dir[,hw|lmin,lmax][,ymin,ymax][,vmin,vmax][,yawmin,yawmax][,wheels]} objects or the compact 'x=..,y=..,z=..,dir=..|...' string. Last gate scored, earlier gates are ordered waypoints."},
   "gate_from_ms": {"type": "integer", "description": "Ignore gate crossings before this script-clock time (mirror the search's earliest modifier time). Default 0."},
   "gate_soft": {"type": "boolean", "description": "Soft (graded) gate scoring for gate_target. Default true."},
   "yaw_weight": {"type": "number", "description": "Soft-score penalty metres per degree outside a gate's heading window. Default 0.5."},
   "speed_weight": {"type": "number", "description": "Soft-score penalty metres per km/h outside a gate's speed window. Default 0.5."},
   "snapshot_at_ms": {"type": "integer", "description": "Single script only: capture the state at this script-clock time (multiple of 10) and return snapshot_id. Up to 64 snapshots are kept per scenario."},
   "from_snapshot": {"type": "string", "description": "snapshot_id to continue from. Inputs at or before the snapshot time must equal those of the run that took it; checkpoint_times include the earlier ones."},
   "horizon_ms": {"type": "integer", "description": "Stop after this much simulation time if the race has not finished. Default 600000."},
   "sample_interval_ms": {"type": "integer", "description": "Telemetry sampling period (multiple of 10). Default 250. Use 0 to omit samples."},
   "from_ms": {"type": "integer", "description": "First sampled input-timeline time. Default 0."},
   "to_ms": {"type": "integer", "description": "Last sampled input-timeline time. Default: end of run."},
   "csv_path": {"type": "string", "description": "Write every tick to this CSV file (absolute path). Use 'auto' for Documents/ForeverTAS/Claude/."},
   "replay_path": {"type": "string", "description": "Scenario override. Defaults to the loaded replay/challenge."}
 }}
})json",
R"json({
 "name": "configure_search",
 "description": "Configures the search exactly as the GUI would (every change is visible to the user and persisted). Only the provided fields change. Setting 'modifiers' replaces the whole ordered pass list. Settings maps take string or numeric values; omitted keys keep their current value (start from list_components defaults). Input-modifier times (minTimeMs/maxTimeMs/...TimeMs) are on the input timeline (same clock as the script); evaluation-target times are simulation times. Returns the resulting configuration, the validation message and whether a search can start.",
 "inputSchema": {"type": "object", "properties": {
   "backend": {"type": "string", "description": "reference | optimized-cpu | multi-threaded-cpu | cuda | vulkan | hip (see get_status for availability)."},
   "simulation_horizon_ms": {"type": "integer", "description": "Search end: simulation time bound for every attempt (multiple of 10)."},
   "extend_horizon": {"type": "boolean", "description": "Raise the search end to cover every configured window."},
   "condition_script": {"type": "string", "description": "Conditions filtering which ticks are eligible for evaluation, one ANDed comparison per line (see get_documentation conditions). Empty string clears."},
   "algorithm": {"type": "object", "properties": {"id": {"type": "string"}, "settings": {"type": "object"}}},
   "modifiers": {"type": "array", "items": {"type": "object", "properties": {"id": {"type": "string"}, "enabled": {"type": "boolean"}, "settings": {"type": "object"}}, "required": ["id"]}},
   "target": {"type": "object", "description": "Evaluation target. 'gate-entry' scores the honest, interpolated crossing of the last of an ordered list of gates (earlier gates are waypoints with their own limits) and, with soft=true, ranks candidates that miss by how close they came, so a search progresses even when nothing reaches the gate yet; its 'gates' setting accepts an array of gate objects (see get_documentation gates).", "properties": {"id": {"type": "string"}, "settings": {"type": "object"}}},
   "cpu_workers": {"type": "integer"},
   "gpu_batch_size": {"type": "integer", "description": "Parallel samples per GPU batch (CUDA/Vulkan/HIP)."},
   "gpu_calibration": {"type": "boolean", "description": "Let the GPU backend calibrate its batch size."},
   "cuda_fast_mode": {"type": "boolean", "description": "Map-specialised CUDA kernels (compute capability 7.5+ only)."},
   "randomize_seeds": {"type": "boolean", "description": "Randomise modifier seeds on every start."},
   "autorestart": {"type": "object", "properties": {"mode": {"type": "string", "enum": ["off", "duration", "attempts"]}, "duration": {"type": "string", "description": "HH:MM:SS"}, "attempts": {"type": "integer"}}}
 }}
})json",
R"json({
 "name": "evaluate_base",
 "description": "Evaluates the current base input script against the configured evaluation target and conditions (no mutation), exactly like the GUI's Evaluate button. Returns score, evaluation time and description, or that the target is never reached.",
 "inputSchema": {"type": "object", "properties": {
   "timeout_seconds": {"type": "number", "description": "Default 60."}
 }}
})json",
R"json({
 "name": "start_search",
 "description": "Starts the configured search in the application (visible live in the GUI and Race Viewer). Returns immediately; poll with wait_for_search. Fails with the validation message when the configuration is not startable.",
 "inputSchema": {"type": "object", "properties": {}}
})json",
R"json({
 "name": "wait_for_search",
 "description": "Blocks until the running search finds an improvement, finishes, or the timeout elapses, then returns the status (attempts, rate, elapsed, best summary). Keep timeouts under ~50 s per call and call repeatedly for long searches.",
 "inputSchema": {"type": "object", "properties": {
   "timeout_seconds": {"type": "number", "description": "Default 30, max 290."},
   "until": {"type": "string", "enum": ["improvement", "finished"], "description": "Default 'improvement' (also returns when the search finishes)."}
 }}
})json",
R"json({
 "name": "stop_search",
 "description": "Stops the running search. Graceful stop finishes the current attempt, restores the best, samples it and saves the cycle; abort=true cancels at the next simulation checkpoint and keeps the last completed best. Waits up to wait_seconds for the search to end.",
 "inputSchema": {"type": "object", "properties": {
   "abort": {"type": "boolean"},
   "wait_seconds": {"type": "number", "description": "Default 30."}
 }}
})json",
R"json({
 "name": "get_best_result",
 "description": "Returns the best result of the current/last search: summary text, the full best input script, attempts and elapsed time, and (by default) full-run simulations of both the best script and the current base script so you can check that the improvement did not desync the rest of the run (finished, finish_time_precise_ms, checkpoint_times).",
 "inputSchema": {"type": "object", "properties": {
   "simulate": {"type": "boolean", "description": "Also simulate the best script and report finish/checkpoint times. Default true."}
 }}
})json",
R"json({
 "name": "get_search_history",
 "description": "Lists saved search sessions for the loaded map and their cycles (each autorestart cycle or stopped search is saved). Selecting a session_index and/or cycle_index loads that cycle's inputs (then use_best_as_base with source 'selected_cycle').",
 "inputSchema": {"type": "object", "properties": {
   "session_index": {"type": "integer"},
   "cycle_index": {"type": "integer"}
 }}
})json",
R"json({
 "name": "save_input_script",
 "description": "Writes an input script to a text file (e.g. for TMInterface). Source: 'best', 'base', 'selected_cycle', or explicit 'script' text.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "Absolute destination path (.txt)."},
   "source": {"type": "string", "enum": ["best", "base", "selected_cycle"]},
   "script": {"type": "string"}
 }, "required": ["path"]}
})json",
R"json({
 "name": "capture_window",
 "description": "Saves a PNG screenshot of the ForeverTAS window (configuration panels and 3D Race Viewer) and returns its path.",
 "inputSchema": {"type": "object", "properties": {
   "path": {"type": "string", "description": "Absolute PNG path. Default Documents/ForeverTAS/Claude/window-<time>.png"}
 }}
})json",
};

QJsonObject ParseObject(const char *text) {
    QJsonParseError error{};
    const QJsonDocument document =
            QJsonDocument::fromJson(QByteArray(text), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::logic_error("invalid built-in MCP tool definition");
    }
    return document.object();
}

}  // namespace

QJsonArray ClaudeToolCatalog() {
    static const QJsonArray catalog = [] {
        QJsonArray tools;
        for (const char *const definition : kToolDefinitions) {
            tools.append(ParseObject(definition));
        }
        return tools;
    }();
    return catalog;
}

QString ClaudeServerInstructions() {
    return QStringLiteral(
            "ForeverTAS drives deterministic TrackMania Nations Forever "
            "physics (ForeverValidator) to build tool-assisted runs. Typical "
            "loop: get_status -> load_scenario (extract_inputs for a replay) "
            "-> simulate to understand the run -> configure_search (target "
            "precise-finish-time or checkpoint-time, modifier windows around "
            "the section being optimised) -> start_search -> wait_for_search "
            "-> stop_search -> use_best_as_base -> repeat with new windows. "
            "For section-by-section work, score each section with the "
            "gate-entry target (honest gate crossings with heading, speed and "
            "lateral limits, ordered waypoints, soft scoring for targets "
            "nothing reaches yet; get_documentation gates), and use simulate "
            "batches with gates and snapshots to build seeds quickly. "
            "Optimise section by section from the start, keep windows short "
            "(1-5 s) and use the GPU backend when available for throughput. "
            "Every action is visible in the user's GUI.");
}

QJsonObject ClaudeInitializeResult(const QJsonObject &params,
                                   const QString &serverVersion) {
    QString protocol = params.value(QStringLiteral("protocolVersion")).toString();
    static const QStringList supported{
            QStringLiteral("2024-11-05"), QStringLiteral("2025-03-26"),
            QStringLiteral("2025-06-18")};
    if (!supported.contains(protocol)) {
        protocol = QString::fromLatin1(kClaudeBridgeDefaultProtocolVersion);
    }
    return QJsonObject{
            {QStringLiteral("protocolVersion"), protocol},
            {QStringLiteral("capabilities"),
             QJsonObject{{QStringLiteral("tools"),
                          QJsonObject{{QStringLiteral("listChanged"), false}}}}},
            {QStringLiteral("serverInfo"),
             QJsonObject{{QStringLiteral("name"),
                          QString::fromLatin1(kClaudeBridgeServerName)},
                         {QStringLiteral("title"), QStringLiteral("ForeverTAS")},
                         {QStringLiteral("version"), serverVersion}}},
            {QStringLiteral("instructions"), ClaudeServerInstructions()}};
}

}  // namespace forevertas::app
