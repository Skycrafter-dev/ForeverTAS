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

Base-input timestamps accept decimal seconds, `mm:ss.mmm`, and `hh:mm:ss.mmm`
(for example `1:23.450 press up`). Fractions have at most three digits and
must align to the 10 ms input tick. Non-leading clock components are in
`0..59`. Export remains decimal seconds for compatibility.

Volume entry times use six fractional second digits in results and stored
result descriptions; preview overlays keep the same precision in the chosen
time unit. These are interpolated crossing estimates,
not six-digit physical accuracy. Numeric scores retain their original double
precision; formatting does not affect comparison or ranking.

Units are global preferences, chosen once under Preferences on the Base tab.
**Steering units** switches every steering field between normalized values and
native integer units `[-65536, 65536]`, matching exported scripts. **Time
units** switches every simulation-time field (windows, search end, hold times,
shifts, radius, deadlines) and time overlay between milliseconds, seconds and
clock time; each field accepts any of these forms. Switching units does not
rewrite settings: times are stored as whole-tick milliseconds, and native
steering edits as exact normalized fractions, with native display using the
same half-away-from-zero quantization as the simulator.

Velocity results and target overlays show km/h with the underlying m/s value
alongside it. Scores, `car.speed`, velocity vectors and saved settings remain
in m/s; `car.speedKph` remains the explicit custom-telemetry conversion.

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
│   │   ├── basic_brute_force_search.h/.cpp
│   │   └── tree_search.h/.cpp
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
supportsGpuBackends (search algorithms only)
supportsMultiThreadedCpu (search algorithms only)
```

The two search-algorithm capability flags replace ID checks: the runner and
controller reject a GPU backend or the multi-threaded CPU backend for an
algorithm that does not declare support, and the controller reports
"<display name> currently requires a CPU physics backend." before Start.

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

### Tree search

`tree-search` shares simulated prefixes between candidates. It splits the
mutable range, from the earliest modifier time to the earlier of the last
affected input and the evaluation end, into `segmentCount` whole-tick
segments. Each tree then:

1. Draws one ordinary candidate with anchor reporting enabled. Each applied
   item reports the earliest input tick it changes, its pass, and its count
   slot. Counting anchors per segment fixes how many items every pass places
   in every segment, so the user's count ranges are drawn once per tree.
   Items anchored after the last observed tick are dropped because they
   cannot change any attempt.
2. Branches only at segments that received items. Branch counts adapt to the
   number of active segments so every tree targets `2^segmentCount` leaves,
   and later segments receive any extra branch.
3. Walks the tree depth first. At a branching segment it captures the
   physics state and clones the evaluation session, then each child restores
   them, redraws only that segment's items through a segment draw, replaces
   the input window from the segment start, and simulates to the segment end.
   Siblings never repeat a draw. A child matches its parent's inputs
   until its first changed input, so it resumes from the latest state an
   earlier sibling captured on that shared timeline before the evaluation
   window, replacing only the inputs after it. Segments without items are
   simulated once for every branch below them.
4. Finishes each leaf by simulating to the evaluation end. Every leaf is one
   attempt: it increments `iterations`, receives the next interleaved
   attempt index, and is reported through `attemptCompleted`. Attempts per
   second therefore count leaves.

Every leaf replays exactly like a flat attempt with the same inputs; the
search smoke test re-simulates every reported leaf from tick zero and
requires identical scores. Improvements found inside the shared prefix use
the inputs applied so far. A race that completes ends its path as one
attempt. A tree whose drawn candidate changes nothing observable is redrawn
without counting an attempt, and a search whose evaluation window ends
before the first mutable input is rejected.

Stop and iteration limits apply after the current leaf. With
`autoPromoteBest`, an improvement, or a different promoted baseline shared by
multi-threaded workers, abandons the rest of the tree so the next tree grows
from the new best. Workers run independent trees. Tree search runs on CPU
physics backends only.

Leaves keep ordinary candidate statistics individually, but siblings share
their early segments: per tree, segment `k` of `segmentCount` sees about
`2^(k+1)` distinct variants. Compare improvements over time, not attempts per
second, when choosing between algorithms.

Settings reshape each tree. Their defaults are the local search that won
the strategy experiments below: four segments, one changed per attempt
(`branchedSegmentCount=1`), the others kept at the current best
(`unbranchedSegments=keep`), and five-second islands (`migrationSeconds=5`).
Setting `branchedSegmentCount=0` and `unbranchedSegments=draw` restores the
full tree described above.

- `leafCount` (default `0`, meaning `2^segmentCount`, at most `2^20`) is the
  leaf target the branch counts aim for, independent of the segment count.
- `branchedSegmentCount` (default `0`, all active segments; at most
  `segmentCount`) branches only that many active segments per tree, chosen at
  random from a hash of the drawn anchors so the choice follows the modifier
  seeds. With `varyBranchedSegmentCount=true` each tree instead branches a
  random number of segments from one up to that limit.
- `unbranchedSegments` decides what the other active segments do: `draw`
  (default) applies one segment draw shared by every leaf, so each leaf still
  changes every anchored item; `keep` drops their items so the leaves keep the
  tree's base inputs there and change only the branched segments. `keep` is a
  local search: with one branched segment of four, a leaf changes about a
  quarter of the items the user's count ranges would otherwise draw.
- `flatWorkerCount` (default `0`) is a portfolio for the multi-threaded CPU
  backend: workers with an index below it run `basic-brute-force` with the
  same `autoPromoteBest`, sharing the promoted best with the tree workers.
  Single-worker backends ignore it.
- `branchedSegmentDistribution` (`uniform` or `geometric`) draws a varied
  branched-segment count uniformly or with halving odds per extra segment.
- `itemKeepPercent` (default `100`) keeps each drawn item with that chance,
  shrinking every candidate further.
- `segmentChoice` picks branched segments `random`ly (default), by an upper
  confidence bound on each segment's improvement rate (`bandit`), or weighted
  toward later, cheaper segments (`late`).
- `repeatImprovedSegments` (default `0`) makes that many trees after an
  improvement branch the improving segments again.
- `migrationSeconds` (default `5`) runs islands on the multi-threaded CPU
  backend: between adoptions of the shared best, each worker promotes only its
  own improvements, so workers keep separate lineages. `0` adopts every
  shared best at once. Single-worker backends ignore it.

The UI shows the segment count, `branchedSegmentCount`,
`unbranchedSegments`, `migrationSeconds`, and promotion directly, and the
leaf count, varied counts, and basic workers under advanced options. The
remaining settings are engine options for experiments.

### Hopeless attempts end early

Targets whose samples only get worse with time report
`IterationEvaluator::LatestImprovingTimeMs`: precise finish time, both
volume-entry targets, checkpoint time, and the earliest Time goal return the
incumbent's sample time. Once an attempt has
observed the tick containing that time without beating the incumbent, no
later sample can, so the CPU loops of basic bruteforce and the tree searches
stop it there. This is always on and changes no result: single-worker runs
with fixed seeds and attempt counts produce identical improvements and final
scores with and without it. GPU sessions are unaffected.

### Adaptive escalation

`adaptive-escalation` is a search option that runs the tree engine with
`unbranchedSegments=keep`. It changes one of `segmentCount` segments per
attempt (default 8), doubles the number of changed segments after
`escalateAfterTrees` trees without improvement (default 8), up to all of
them, and returns to one after any improvement, so it polishes while
progress comes and widens when it stalls. `migrationSeconds` (default `0`)
adds islands. It runs on CPU physics backends, including multi-threaded CPU.

The settings were compared with `forevertas-search-strategy-comparison`
(6 workers, auto-promote, seed trials of 30 s, pose and velocity targets over
the end of the mutation window). `unbranchedSegments=keep` with one branched
segment, or with `varyBranchedSegmentCount=true`, found five to eight times
as many improvements per minute as basic bruteforce and the default tree and
reached a lower pose error. Most of the gain comes from changing fewer inputs
per attempt rather than from shared prefixes: one leaf per tree did about as
well. The velocity target has two basins far apart. Basic bruteforce
reached the faster one in 6 of 8 seeds and `varyBranchedSegmentCount=true`
with `keep` in 3 of 8, which is within noise but suggests that large changes
still escape basins better. Within the slower basin the `keep` variants
refined further.

A larger study with `forevertas-search-lab` (498 runs over 14 scenarios on
seven maps; pose, speed, finish-time, and point targets; dense, sparse, and
smooth modifier presets; 6 workers and auto-promote, compared on paired
seeds) changed that picture. One branched segment with `keep` beat basic
bruteforce in 48 of 56 paired runs and the default ten-segment tree lost to
basic in most of them. Adding five-second islands beat plain `keep` in 32 of
46 paired runs and reached the faster velocity basin in all 16 runs, where
basic bruteforce never did (0 of 11) and islands on basic bruteforce did not
help. With sparse or smooth presets, which already change little, basic
bruteforce did as well. These results set the current defaults.

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
not executed past that horizon, and the viewer previews them only up to the
separate Preview extent. Modifier windows may also extend
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

### Tree-search anchors and segment draws

`MutationRequest` carries two optional tree-search fields, forwarded by the
composite to every pass:

- `anchors`: when set, an ordinary draw appends one `MutationAnchor` per
  applied item: the earliest input time the item changes, the pass index,
  and a modifier-defined count slot. Recording must not consume random
  numbers, so ordinary draws stay identical to the CUDA, Vulkan, and HIP
  samplers.
- `segment`: when set, the pass draws only `slotCounts[passIndex][slot]`
  items whose anchors fall inside `anchorRange`, applying each item in full
  even where its effect extends past the segment. Count-driven passes with
  no items in the segment return their normalized input without seeding a
  random engine. Draws use the same value
  distributions as ordinary draws conditioned on the anchor: uniform starts
  and event picks restricted to the segment, or rejection for items whose
  anchor depends on a drawn offset.

Current slots and anchors:

| Modifier | Slots | Anchor |
| --- | --- | --- |
| `random-steering` | 0 | each rewritten event; segment draws rewrite every eligible event in the segment |
| `existing-event-perturbation` | 0 | earlier of the original and shifted event time |
| `smooth-steering` | 0 | first tick the deformation writes |
| `input-insertion` | 0 steer, 1 accelerate, 2 brake | inserted start |
| `input-deletion` | 0 steer, 1 accelerate, 2 brake | deleted event time |

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

The complete [symbol and function reference](CONDITIONS.md) is generated from
the parser catalog; the catalog test checks every spelling, context and example.

`search/conditionScript` is an optional persisted tick-eligibility program.
Every non-empty line is a comparison and all lines are ANDed. The language
matches BfV2 condition scripts: scalar and vector current/previous car state,
wheel contact/sliding/surface values, search timestamps and iteration count,
`+ - * /`, `> < >= <= =`, grouping, `kmh`, `deg`, `distance`, `time_since`,
and `variable`/`var`. The active point target is available as the vector
`bf_target_point`.

The parser emits one bounded postfix program used by both host and CUDA
interpreters. The search checks that program immediately before calling the
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

`IterationEvaluationSession::Clone()` copies the complete timeline state.
Tree search clones a session at every branching segment, so a clone must
produce exactly the samples the original would for the same later states.

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
2. Implement `InputMutator`, including `EarliestMutationTimeMs()`, anchor
   reporting, and segment draws for tree search.
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
2. Implement `IterationEvaluator` and a per-iteration session, including
   `Clone()`.
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
- `tree-search`: CPU-only trees of candidates that share simulated prefixes;
  every leaf is one attempt. It defaults to the winning local search: one of
  four segments changes per attempt, the others keep the current best, and
  multi-threaded workers run five-second islands.
- `adaptive-escalation`: CPU-only local search that widens from one changed
  segment to all of them while it stalls and resets on improvement.

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
- `time`: finds the first tick in its window on which every condition holds
  and makes it happen as early (`earliest`) or as late (`latest`) as possible.
  Its value comes only from the Conditions, so the app requires at least one.
  The run stops at that first match. It observes ticks before the first
  mutation, because a later first match would misreport that run. CPU, CUDA,
  HIP and Vulkan all score the same first-match tick.

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
