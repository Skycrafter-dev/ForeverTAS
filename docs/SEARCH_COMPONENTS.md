# Search Components Architecture

This document describes how ForeverTAS organizes configurable search
algorithms, ordered input modifiers, and evaluation targets. It is the primary
reference for adding search features without coupling the controller or main
QML file to individual implementations.

## Feature Model

A search request contains five parts:

1. The Packs directory and replay-or-challenge scenario path.
2. Parsed base-input commands.
3. One selected search algorithm.
4. An ordered list of configured modifier passes.
5. One selected evaluation target.

The application currently requires at least one modifier pass before a search
can start.

Each selectable implementation owns:

- A stable ID and display name.
- Its complete default settings map.
- Typed parsing and validation.
- Its runtime factory.
- Its QML settings component.
- Optional aliases and legacy persistence mappings.

The registry is the only source that connects these pieces. `Main.qml`,
`SearchController`, and `RunSearch` do not switch on implementation IDs.

## Directory Organization

```text
ForeverTAS/
├── src/
│   ├── searches/
│   │   ├── algorithm_registry.h/.cpp
│   │   ├── option_configuration.h
│   │   ├── option_settings_utils.h
│   │   ├── search_algorithm.h
│   │   ├── search_runner.h/.cpp
│   │   └── basic_brute_force_search.h/.cpp
│   │
│   ├── mutations/
│   │   ├── input_mutator.h
│   │   ├── input_event_utils.h/.cpp
│   │   ├── input_event_formatter.h/.cpp
│   │   ├── modifier_utils.h
│   │   ├── composite_input_mutator.h/.cpp
│   │   ├── random_steering_mutator.h/.cpp
│   │   ├── existing_event_perturbation_mutator.h/.cpp
│   │   ├── smooth_steering_mutator.h/.cpp
│   │   ├── input_insertion_mutator.h/.cpp
│   │   └── input_deletion_mutator.h/.cpp
│   │
│   ├── evaluators/
│   │   ├── iteration_evaluator.h
│   │   ├── evaluator_utils.h
│   │   ├── precise_finish_time_evaluator.h/.cpp
│   │   ├── volume_entry_evaluator.h/.cpp
│   │   ├── velocity_evaluator.h/.cpp
│   │   ├── point_target_evaluator.h/.cpp
│   │   └── pose_target_evaluator.h/.cpp
│   │
│   └── app/
│       ├── search_completion.h
│       ├── search_configuration_model.h/.cpp
│       ├── search_controller.h/.cpp
│       └── search_worker.h/.cpp
│
├── qml/
│   ├── Main.qml
│   └── settings/
│       ├── AlgorithmSelector.qml
│       ├── ModifierComposition.qml
│       ├── SettingTextField.qml
│       ├── SettingSwitch.qml
│       ├── SettingCombo.qml
│       ├── TimeWindowSettings.qml
│       ├── Vector3Settings.qml
│       └── one owned component per selectable implementation
│
├── tests/
│   ├── search_component_tests.cpp
│   ├── search_controller_tests.cpp
│   ├── search_smoke.cpp
│   ├── viewer_smoke.cpp
│   └── viewer_qml_smoke.cpp
│
└── CMakeLists.txt
```

## Generic Configuration Transport

`src/searches/option_configuration.h` defines the category-neutral transport
format:

```cpp
using OptionSettings = std::map<std::string, std::string>;

struct OptionConfiguration {
    std::string id;
    OptionSettings settings;
};
```

Values remain strings while they are edited and persisted. The implementation
that owns the option parses them into a typed structure during validation and
construction.

`SearchRequest` contains:

```text
SearchRequest
├── packDirectory
├── replayPath
├── baseInputCommands: vector<ParsedInputCommand>
├── searchAlgorithm: OptionConfiguration
├── modifiers: vector<OptionConfiguration>
└── evaluationTarget: OptionConfiguration
```

Repeated modifier IDs are allowed. Each vector entry is an independent pass
with its own settings.

## Registry Contract

`src/searches/algorithm_registry.*` contains three registries:

- `SearchAlgorithmRegistry()`
- `ModifierRegistry()`
- `EvaluationTargetRegistry()`

Every registration contains:

```text
id
legacyIds
displayName
settingsComponent
defaultSettings
legacyPersistenceKeys
validateSettings callback
create callback
```

Stable IDs use lowercase hyphen-separated names. Released IDs must not be
silently reused for a different behavior. Renames require an alias in
`legacyIds`; the controller canonicalizes persisted aliases back to the current
ID.

`defaultSettings` is also the allowed key set. The controller ignores update
requests for unknown keys, while implementation validation rejects incomplete
or extra maps received outside the controller.

## Search Algorithm Contract

`SearchAlgorithm` receives a `SearchExecutionContext` containing:

- A loaded `PhysicsSandbox`.
- The physics tick duration.
- The composed `InputMutator` pipeline.
- The selected `IterationEvaluator`.
- Stop, hard-abort, progress, and live-best callbacks.

The Basic bruteforce implementation owns continuous iteration scheduling and
global winner selection. It does not own modifier windows, seeds, evaluation
windows, or comparison direction.

It asks:

- The modifier pipeline for its earliest affected input time.
- The evaluation target for its observation plan.
- The evaluation target whether a sample is better than the incumbent.

This keeps search orchestration independent from every target and modifier ID.

### Winner retention and final sampling

`SearchLiveUpdate` publishes the current winning state, normalized input
timeline, iteration count, throughput, elapsed time, and last-improvement time.
It is emitted periodically while the loop runs and immediately after each new
global best. The worker refreshes the summary live without simulating a complete
viewer timeline.

Pressing Stop finishes the current iteration and returns `SearchResult`. Only
then does `RunSearch` perform the separate final-sampling stage:

1. Open a fresh Reference-backend sandbox with the configured Simulation horizon.
2. Reload the map into a canonical timeline from tick zero.
3. Replace its inputs with the winning timeline.
4. Advance exactly one physics tick at a time until genuine completion or the
   Simulation horizon.
5. Record position, rotation, and input state for every tick.

This Stop-triggered pass is intentionally separate from iteration evaluation.
Search algorithms remain free to branch, restore snapshots, and observe only
the target-required window without retaining complete iteration traces. The
worker reports this pass as `SearchProgressStage::FinalSampling`. A private
hard-abort callback exists only for application shutdown and skips completion
sampling.

`input_event_formatter.*` converts the retained timeline into invariant,
copy-ready input script syntax and parses that same input-only command subset.
Timestamps always use `.` decimals and do not depend on `LC_NUMERIC`; analog
states are already canonical integers and are serialized verbatim.

Parsed commands retain user-relative milliseconds and their source line.
Loading a map creates a canonical `RaceRunning` origin at zero, after which the
runner applies the existing one-tick user-timeline offset. Recorded controls,
finish markers, outcomes, and timing are not imported. Input commands may extend
beyond the user-configured Simulation horizon; they remain in the script but are
not executed or previewed past that horizon. Modifier windows may also extend
beyond it in the saved configuration; execution silently limits them to the
last input tick inside the horizon. Cached sandboxes always restore the
canonical map snapshot before applying the current request's script.

### Canonical analog input representation

All replay, sandbox, mutation, winner-retention, and script-export layers use
`AnalogInputState`, a signed integer constrained to `[-65536, 65536]`. Its sign
convention is explicit: negative steering is left, positive steering is right;
analog gas uses negative values for accelerate and positive values for brake. Replay decoding converts the game's signed-24
storage representation directly into this canonical form.

Before a search starts, keyboard left/right events are collapsed into canonical
analog steering events. The conversion mirrors the engine's timestamp
arbitration, same-tick analog dead zone, and left-over-right priority, so mixed
keyboard and analog scripts produce the same physics while modifiers see one
steering channel.

Modifier settings remain normalized decimal strings in `[-1, 1]` for UI and
persistence compatibility. `ParseNormalizedAnalogInput` quantizes each setting
once to an integer state. Mutators subsequently use integer sampling, addition,
comparison, and saturation only; iteration timelines never carry arbitrary
floating-point analog values.

The only integer-to-float conversion occurs when ForeverValidator builds the
normalized vehicle-control state consumed by physics. Physics state snapshots,
search samples, and Race Viewer channels intentionally remain floats because
they describe applied simulation controls rather than editable input events.

## Modifier Contract

`InputMutator::Mutate` receives:

- The current input timeline entering that pass.
- The iteration index.
- The pass index.
- The physics tick duration.

Each modifier instance owns its own active window, seed, channel selection,
and modification parameters. `EarliestMutationTimeMs()` reports the earliest
input tick that the pass may change. `CompositeInputMutator` uses the minimum
across all passes.

### User timeline origin

All UI and persisted input timeline values are zero-based. The simulation's
first actionable input occurs one physics tick later, so user `0 ms` maps to
simulation `10 ms` at the current 100 Hz rate. This translation is centralized
in `input_timeline_time.h` and applied exactly once by the public modifier
registry validation and factory methods before their simulation-native
implementation hooks are called.

The naming contract is deliberate: every absolute input timeline setting key
ends in `TimeMs` and is shifted by one tick. Relative durations use a more
specific suffix such as `HoldMs`, `ShiftMs`, or `RadiusMs` and are never shifted.
Only modifier settings pass through this conversion; evaluation frames and
search-policy settings use the entered simulation time directly. Registry
coverage tests enforce this boundary for every current option, while
input-script serialization uses the same inverse conversion. Components must
not add local time offsets.

### Composition

Modifier passes run in displayed order. Each pass receives the previous pass's
output. The search also supplies the earliest mutable input time: the first tick
after the restored branch state. Every pass and the final composite
normalization preserve baseline events before that boundary byte-for-byte and
in their original order. Only the mutable suffix is normalized:

- Saturate every analog state to the exact integer range `[-65536, 65536]`.
- Align event times to whole simulation ticks.
- Sort events chronologically with stable ordering.
- For multiple events with the same action and tick, keep the last pass value.
- Reattach the exact immutable baseline prefix.
- Count effective differences from the original baseline.

This split is required because `PhysicsSandbox::ReplaceInputs` rejects any
change to replay history before the restored branch. If no effective change
remains after normalization, the iteration is still counted but the unchanged
simulation is not repeated.

Deterministic random streams are derived from:

```text
configured seed + iteration index + pass index
```

By default, Start first replaces and persists every displayed modifier seed so
successive searches explore new streams. Disabling **Randomize modifier seeds
on Start** leaves those values untouched for reproducible reruns. Within a run,
the configured seed, iteration index, and pass index keep streams deterministic
while allowing repeated instances of the same modifier to remain independent.

## Evaluation Target Contract

### Conditions

`search/conditionScript` is an optional persisted tick-eligibility program,
with `search/conditionGateMode` selecting AND or OR combination. Every enabled
line is a comparison; lines prefixed with `//` are disabled. The language
matches BfV2 condition scripts: scalar and vector current/previous car state,
wheel contact/sliding/surface values, search timestamps and iteration count,
`+ - * /`, `> < >= <= =`, grouping, `kmh`, `deg`, `distance`, `time_since`,
and `variable`/`var`. The active point target is available as the vector
`bf_target_point`.

The complete user-facing grammar, object/alias list, builder controls, horizon
semantics, and Point Target examples live in
[Condition Language Reference](CONDITIONS.md). The shared C++ catalogue is the
source for both that language implementation and context-aware completion. A
single lexer and recoverable AST also own incomplete member and argument sites;
the UI accepts revision-checked, whole-document completion edits so source and
cursor updates cannot race.
Condition and base-input scripts can be stored in separate custom-named `.txt`
libraries through the shared `ScriptFileStore`; atomic writes prevent partial
files and an existing name is never replaced without confirmation.

The parser emits one bounded postfix program used by both host and CUDA
interpreters. AND uses the native logical opcode; OR reduces boolean gate
results with addition and a final greater-than-zero comparison, keeping the
same CUDA ABI. The search checks that program immediately before calling the
target session. A false condition therefore removes only that tick from
evaluation; it does not stop simulation or reset target state. A run with at
least one eligible target sample always outranks a baseline with none, while
two eligible runs remain ordered exclusively by `IterationEvaluator::IsBetter`.

Evaluation is timeline-based rather than a single stateless score function.

`IterationEvaluator` owns:

- `Plan(...)`: the closed observation window for a replay and modifier branch.
- `CreateSession()`: per-iteration timeline state.
- `IsBetter(...)`: maximize or minimize semantics.

Each observed result is an `EvaluationSample`:

```text
score
timeMs
description
```

The description is displayed directly in the result summary, so targets own
their metric wording.

Timeline sessions receive the previous and current sandbox states. This lets
transition targets, such as entering a volume, interpolate crossing time
between ticks without adding target-specific logic to the search algorithm.

## Controller Responsibilities

`SearchConfigurationModel` owns the generic component configuration state:

- Selected search ID and search settings map.
- Ordered modifier-pass list.
- Selected evaluation ID and evaluation settings map.
- Generic add/remove/move/type/setting methods for modifier passes.
- Registry-driven validation.
- Generic persistence and request construction.

`SearchController` owns application coordination:

- Worker-thread lifecycle, paths, base-script validation and persistence,
  replay-input extraction, status, and progress.
- Completed-search transport: summary text, copy-ready winning inputs, replay
  identity, and the fully sampled winning timeline.
- QML properties and change notifications that delegate to the configuration
  model.

Neither class may gain a field, property, or method named after a concrete
target or modifier.

The QML-facing composition API is:

```text
modifierOptions
modifierPasses
addModifierPass(id)
removeModifierPass(index)
moveModifierPass(fromIndex, toIndex)
setModifierPassId(index, id)
setModifierPassSetting(index, key, value)
```

## Viewer runs

`RaceViewerController` stores a vector of named `RaceViewerRun` entries rather
than one global frame vector. Each run owns its sampled frames and current
interpolated pose.

`loadMap` reads the selected scenario's scene, render geometry, and vehicle
shape without advancing the simulation or creating a run. A loaded map
therefore has zero runs and disabled timeline controls. A completed search
upserts `Best`; the same run
container supports additional result types later without adding more controller
fields.

Scenario loading is serialized and transactional. If another file is requested
while the active worker is still finishing, the latest request is queued and
starts as soon as the worker exits. A monotonically increasing load serial
prevents a late result from an older worker from replacing the newer scene.
The current scene remains published while a replacement is loading; publishing
an intermediate empty run or ellipsoid model would detach nested Qt Quick 3D
render nodes. QML mirrors ellipsoid transforms into a stable `ListModel`, updates
roles in place, and retains inactive delegates when vehicle shape counts shrink.
The controller and QML regressions perform three real first-second-first loads;
the QML test invokes the actual **Load map** button for every load and checks
current shape transforms and map rendering on a capable graphics backend.

The settings pane uses one window-level wheel redirector over its entire visible
rectangle. Mouse-wheel and touchpad vertical deltas update only the outer
settings flickable, even over sliders, dropdowns, or the best-input preview.
Nested scroll areas remain usable through direct dragging and their scrollbars,
but do not steal wheel input from the pane.

The selected run owns the active timeline, duration, input channels, playback,
and camera focus. All runs are still interpolated at the active time and exposed
to QML through `runPoses`, so the preview renders one car hierarchy per run.
`runOptions` and `selectedRunId` drive the centered run selector.

QML mirrors `runPoses` into a stable `ListModel` and updates roles in place.
Each run pose also carries its prebuilt car geometry. Best and future runs use
separate baked palettes with the same flat-shading formula. Filled materials
stay white with vertex colors enabled, avoiding color multiplication that would
darken or distort the baked shading. Binding `Repeater3D` directly to a rebuilt
`QVariantList` would destroy and
recreate every car model whenever the time changes.

## Persistence

Search and evaluation selections use:

```text
selection/searchAlgorithm
selection/evaluationTarget
```

Their settings remain namespaced by category and option ID:

```text
configuration/search/<id>/<key>
configuration/evaluation/<id>/<key>
```

The ordered modifier composition is stored as compact JSON under:

```text
composition/modifiers
```

Each JSON entry contains its modifier ID and complete settings object. This
preserves order, repeated modifier types, and independent values.

Older single-modifier settings are migrated only when no composition JSON is
present. Legacy mappings stay in registry metadata or narrowly named migration
constants; they must not appear as selectable options.

## QML Ownership

`Main.qml` places one generic search selector, one generic modifier composition
editor, and one generic evaluation selector.

`AlgorithmSelector` loads the selected option's `settingsComponent` directly
from registry metadata.

`ModifierComposition` renders the persisted pass order, provides add/remove/
move controls, and loads each pass's settings component. It supplies a pass
component with:

```qml
property var settings
property var updateSetting
property bool running
```

A modifier QML file reads only its provided settings map and writes only via
`updateSetting(key, value)`.

Search and evaluation components receive `property var controller` and use only
their corresponding generic settings map and update method.

Reusable field/layout components belong in `qml/settings/`; implementation
logic and implementation-specific field lists belong in the owned component.

## Adding a Modifier

Assume a new modifier named **Steering Jitter** with ID `steering-jitter`.

1. Create `src/mutations/steering_jitter_mutator.h/.cpp`.
2. Implement `InputMutator`, including `EarliestMutationTimeMs()`.
3. Define a typed settings structure owned by the modifier.
4. Provide defaults, validation, and a factory matching
   `ModifierRegistration`.
5. Reject unknown keys and parse every default key.
6. Use the shared deterministic RNG helpers when randomness is involved.
7. Add one `ModifierRegistration` entry.
8. Create `qml/settings/SteeringJitterSettings.qml` using `settings`,
   `updateSetting`, and `running`.
9. Add source and QML files to CMake.
10. Test validation, deterministic output, boundaries, normalization,
    registry construction, persistence, and QML loading.

No change to `Main.qml`, `SearchController`, `SearchRequest`, or
`BasicBruteForceSearch` should be needed.

## Adding an Evaluation Target

1. Create target files under `src/evaluators/`.
2. Implement `IterationEvaluator` and a per-iteration session.
3. Define the target's observation plan and comparison direction.
4. Provide defaults, validation, and a factory.
5. Return a clear target-owned `EvaluationSample::description`.
6. Add one `EvaluationTargetRegistration` entry.
7. Create and register the owned QML component.
8. Test the metric with synthetic state sequences, including transition and
   interpolation cases where relevant.

No search-loop or controller branch should be added for the target.

## Adding a Search Algorithm

1. Implement `SearchAlgorithm` under `src/searches/`.
2. Keep only search-policy settings in its typed structure.
3. Consume the generic mutator and evaluator contracts.
4. Report progress, publish improvements, and check Stop regularly.
5. Provide defaults, validation, factory, registry entry, and QML component.
6. Test default construction and algorithm-specific scheduling behavior.

## Current Built-In Components

### Search algorithms

- `basic-brute-force`: baseline plus independent deterministic iterations,
  continuing until Stop is requested.

### Modifiers

- `random-steering`: replaces existing steering values in a window.
- `existing-event-perturbation`: perturbs selected existing event values and
  times.
- `smooth-steering`: adds raised-cosine steering deformations.
- `input-insertion`: inserts steering, accelerate, or brake segments.
- `input-deletion`: deletes eligible events per channel.

### Evaluation targets

- `velocity`: total or projected velocity with optional alignment threshold.
- `precise-finish-time`: minimizes the inclusive nanosecond upper bound of
  the simulated finish transition. The legacy `finish-time` ID migrates to
  this target. Results use the compact `h:mm:ss.nnnnnnnnn` form, omitting
  zero-valued hour and minute components while retaining all nine fractional
  digits. CUDA searches use the ordinary CUDA timeline for this target because
  the resident evaluator only exposes tick-rounded finish time.
- `volume-entry-time`: minimizes interpolated entry time into a cuboid.
- `point-target`: minimizes distance to a target point over a window.
- `pose-target`: minimizes weighted position and orientation error.

Spatial target models expose atomic absolute-placement operations. Their QML
editors receive the viewport's rendered camera pose and the viewer's simulated
car pose, allowing the selected cuboid, polygon volume, or full pose target to
be moved directly to either source without incremental coordinate edits.

## Testing Checklist

Before submitting a new component:

1. Build with the strict warning flags.
2. Run all CTest targets.
3. Run the real Wayland/GPU viewer smoke test when available.
4. Run a real replay search smoke test.
5. Run `git diff --check`.
6. Confirm IDs appear only in registry code, migration tests, and registry
   assertions.
7. Confirm no ID switch or option-specific controller field was introduced.
8. Confirm every option owns defaults, validation, factory, persistence
   metadata, and QML.
9. Confirm repeated modifier instances preserve independent settings and order.
10. Confirm invalid settings produce actionable messages.
