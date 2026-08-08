# ForeverTAS

ForeverTAS is the TAS client for
[ForeverValidator](https://github.com/Skycrafter-dev/ForeverValidator).
ForeverValidator remains the source of truth for deterministic physics
and replay validation, used through the PhysicsSandbox API.

```text
ForeverTAS -> PhysicsSandbox -> ForeverValidator physics
```

## Dependency

CMake `FetchContent` pins ForeverValidator to the exact commit
`688071fa82c3d93b3617ac1529a24915c0fe3364`. The embedded build disables the
ForeverValidator CLI and tests and links its native asset adapter and core
simulation library.

## Build

The pinned preset provides a reproducible build from the exact public
ForeverValidator commit:

```sh
cmake --preset pinned
cmake --build --preset pinned
ctest --preset pinned
```

For day-to-day development, create `CMakeUserPresets.json` from
`CMakeUserPresets.json.example` once and set
`FETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR` to the local ForeverValidator
checkout. This file is ignored because its path is machine-specific. The local
preset has its own build tree and always uses the current Validator worktree,
including uncommitted changes:

```sh
cmake --preset local-validator
cmake --build --preset local-validator
ctest --preset local-validator
```

The committed dependency hash only needs to change when ForeverTAS deliberately
adopts a tested ForeverValidator revision. Use the pinned preset as the final
pre-push check.

## Desktop application

Build and launch the Qt 6 Quick application:

```sh
./build/local/bin/ForeverTAS
```

Select an installed TMUF `Packs` directory and either a replay or standalone
`Challenge.Gbx`, enter a base input script, choose an evaluation target,
assemble an ordered list of input modifier passes, then start the basic search.
The Browse buttons always open the operating system's file picker rather than
a Qt-provided dialog.
The selected file supplies the map and scenario; only the editable script
supplies the player-control baseline. **Extract inputs to script** is available
for replays and imports their controls when that is the desired starting point.
The application persists paths, the script draft, selections, pass
order, the user-owned **Simulation horizon**, and every option-owned
configuration with the platform-native Qt settings store. That horizon alone
bounds search, preview, validation, and CPU/CUDA simulation; commands after it
remain editable but unexecuted. Modifier time windows that extend past it are
silently limited to the last executable input tick. Modifier seeds are
randomized and persisted on each Start by default; disabling that option
preserves the entered seeds for reproducible reruns. Search, map loading,
validation, and physics
stay in C++; QML presents the controls and Race Viewer.
The base-input and Conditions editors also provide **Save file** and **Load file**
libraries for reusable, custom-named `.txt` scripts; the two libraries are
kept separate and replacing an existing name requires confirmation.
Condition symbols expose parser-backed definition/type hovers, and recognized
function calls auto-pair parentheses while keeping the cursor inside.
Surface, turbo, boolean, and gear comparisons offer named constants on their
right-hand side instead of requiring raw runtime IDs.

The search runs indefinitely on a worker thread after Start is pressed. Each
iteration applies the configured modifier passes in order, preserves the
script-derived input prefix before the mutation branch exactly, normalizes only
the mutable suffix, and evaluates it with the selected target. Whenever a new
global best is found, its copy-ready input script is shown immediately.
The optional **Promote each best result to baseline** mode turns the search
into iterative refinement: after an improvement, later mutations start from
that best input sequence instead of the original script-derived baseline.
Iteration count, iterations per second, elapsed time, and time since the last
improvement continue refreshing while the search runs. Pressing Stop
finishes the current
iteration, restores the global best, then performs one fresh canonical
simulation and records one viewer sample per physics tick. The completed Best
run is added to the Race Viewer only after that Stop-triggered sampling pass.
CUDA searches reconstruct changed winners and every viewer trajectory with the
Reference backend; unchanged device incumbents do not trigger redundant CPU
reconstruction. Viewable input-backed runs keep physics snapshots at one-second
intervals, so edits and Simulation-horizon changes resume from the latest valid
snapshot rather than replaying the whole run from zero.
The optional Conditions script filters which simulated ticks are eligible for
the selected evaluation target. Each enabled line is a comparison, combined
with the selected AND/OR mode; line numbers toggle individual gates;
it supports the BfV2 car, previous-car, wheel, and search-state variables,
arithmetic, `kmh`, `deg`, `distance`, `time_since`, and external target values.
The built-in Tick Gates editor provides one-level-at-a-time hierarchical
completion, IDE-style type badges, signature help, and red line diagnostics.
See the complete
[condition language reference](docs/CONDITIONS.md) for every object, alias,
function, and Point Target example.
If the baseline never satisfies the script, its evaluation is intentionally
empty and the first satisfying mutation outranks it regardless of target
score. Conditions never change simulation, inputs, or the target's ranking
rule.
Analog script and iteration inputs use the exact signed integer state range
`[-65536, 65536]`; normalized decimal UI settings are quantized once when
parsed. User-facing input timeline settings are zero-based: `0 ms` selects the
first actionable input, which is simulation time `10 ms` at 100 Hz. Absolute
setting keys ending in `TimeMs` are translated by one physics tick exactly once
when a registry creates a simulation component; stored values and relative
durations remain user-facing.
Built-in targets cover precise finish time, stunt points by a chosen deadline,
cuboid entry time, velocity, point distance, and weighted pose error. The stunt
target observes only the chosen deadline because the score is monotonic.
Volume-entry targets are managed as a persistent named cuboid collection. The
selected cuboid is the active brute-force target; the evaluation panel can add,
duplicate, remove, rename, and directly edit every cuboid or focus the viewer
camera on it. Camera and car placement buttons move the selected target to the
current rendered camera or simulated car position. Every cuboid is visible in
both viewer renderers, and the selected one exposes color-coded 3D axis bars
for movement plus endpoint handles for resizing. Targets are occluded by map
geometry by default; the global draw-through toggle makes them visible through
blocks when desired.
Custom polygon volumes share the same shape-target menu. A target stores a
drawing plane, editable 2D vertices, and an independent extrusion depth. Users
can select an axis plane in the viewer, redraw the polygon directly against
that plane, drag its vertex and depth handles, edit the same values in the
settings list, and focus the camera on the finished prism. The selected custom
volume has the same camera and car placement actions and is evaluated exactly
by the Reference and optimized CPU backends; the UI
reports that CUDA is unavailable rather than substituting an inexact cuboid.
Pose targets are managed as a persistent named collection of complete car
positions and orientations. The selected pose is the weighted pose-error goal
for every brute-force backend. Users can add a pose from the viewer car,
duplicate, remove, rename, and edit it in the evaluation panel, or translate
and rotate it with color-coded handles on its visible car model in either
viewer renderer. Camera and car placement copy both position and orientation;
the Focus action frames the selected pose in the camera.
Precise finish search ranks the inclusive upper bound of ForeverValidator's
one-nanosecond transition bracket and displays all nine fractional digits.
Built-in modifiers cover existing-event perturbation, smooth steering
deformation, input insertion, input deletion, and random steering.

The multi-threaded CPU backend assigns a disjoint mutation sequence to each
worker. Every worker owns an independent optimized-CPU simulation, while live
metrics and best results are reduced into one deterministic aggregate. The
worker count is configurable and persists between sessions.

The complete visible settings pane owns vertical wheel scrolling, including
areas occupied by sliders, dropdowns, and the best-input preview. Nested
controls do not capture wheel input from the pane.

Map loads are serialized and transactional. The currently rendered scene stays
attached while a replacement replay's geometry and car shape are read, then the
scene is swapped only after the new map is complete. Loading a map does not
advance or publish the replay timeline. Timeline controls remain disabled until
a completed search adds the `Best` run.

The Race Viewer stores named search-result runs. A header selector switches the
active timeline and camera focus between `Best` and future run types, while
every run remains visible as a separate car in the 3D preview. Car colors are
baked into separate flat-shaded vertex-color meshes. Its telemetry overlay is
an editable persisted template. The field picker inserts camera, car position,
velocity, speed, input, race progress, timeline, tick, and run-name fields;
numeric fields accept an optional zero-to-six digit precision suffix.

After a map is loaded, **Drive** starts a live 100 Hz physics run in the viewer.
Arrow keys and QWERTY `WASD` control full acceleration, braking, and steering;
`ZQSD` provides the equivalent bindings on AZERTY layouts. Simultaneous
digital inputs retain ForeverValidator's in-game priority rules, including
left steering over right. Losing keyboard focus releases held controls, and a
completed manual session remains available as the `Manual` viewer run.

**Copy current race** in the base-input section replaces the search input with
the selected viewer run through its current timeline position. Events after
that position are deliberately excluded, so a partial manual or scripted run
can become the exact starting point for the next search.

**Save trajectory** (or `Ctrl+S`) simulates the current base-input script and
adds its exact path to the 3D viewer as a persistent reference for the loaded
map. Semantically identical scripts are deduplicated, so repeated saves do not
stack duplicate paths.

While a search is running, every published best-run improvement is sampled
through the full replay and added to the viewer as an amber trajectory. The
newest path is emphasized while older improvement paths remain visible at
reduced opacity.

The default viewport is the textured Qt Quick 3D renderer. On Qt 6.7 or newer
with ShaderTools, the `Textured (RT)` render mode enables the real-time QRhi
compute renderer with GPU BVH traversal, ray-traced shadows and reflections,
and immediate noise-free output. Qt 6.5 and 6.6 keep the full raster renderer
and omit only that optional mode.

The viewer's **Whiteboard** mode draws directly over either renderer without
replacing the map, cars, targets, or trajectories. Pen strokes, lines,
rectangles, ellipses, and editable vector text are independent movable and
resizable items. Drawing color and stroke size are adjustable, and the eraser
removes pixels only from the selected item. Leaving whiteboard mode restores
normal 3D camera interaction while keeping the overlay visible.

**Place** turns the current overlay into a static whiteboard plane at the
current camera angle. The Drawings list restores each saved viewpoint, toggles
that plane for the current map without deleting it, and keeps multiple
map-specific drawings across application sessions. Complete drawing sets can
also be exported to, or imported from, a location selected with the native
file picker.

## Portable bundles

ForeverTAS can be packaged natively as a Linux AppImage or Windows portable
ZIP. Both artifacts use the same CMake installation definition and include the
required Qt and QML runtime files. macOS is not supported.

See [docs/PACKAGING.md](docs/PACKAGING.md) for local packaging commands,
artifact layouts, signing notes, and clean-machine release checks.

See `docs/SEARCH_COMPONENTS.md` for the registry, persistence, composition, and
extension contracts.

See `docs/RENDERER.md` for visual-scene extraction, replacement materials,
fallbacks, caching, render modes, and asset ownership.
