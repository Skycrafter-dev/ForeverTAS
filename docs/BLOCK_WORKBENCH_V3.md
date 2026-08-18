# Block Workbench v3 architecture

Status: implemented and validated on the `scratch-blocks` worktree. The v3
block-workbench scope described by this document is complete; normal product
release packaging/QA remains part of the wider ForeverTAS release process.

## Current implementation checkpoint

The architecture is now running in the application rather than existing only
as a plan:

- the production Blocks page embeds a locally bundled Blockly editor through
  Qt WebEngine/WebChannel; the old hand-built QML canvas is no longer part of
  the production QML module or active test suite;
- the version-3 C++ graph owns arbitrary named value inputs and statement
  stacks, persists independently of Blockly, and compiles before runtime state
  is changed;
- Blockly value sockets use the catalog's semantic type checks, and statement
  connections are also typed (`root-command`, `iteration-command`, and
  `mutation-command`) so invalid stack placement is prevented in the editor
  and rejected again by native validation;
- the visual search is process-shaped: `bruteforce` owns an `each iteration`
  stack whose current canonical order is `mutate -> simulate -> keep best`;
  simulation horizon and its per-tick predicate are parameters of the
  simulation step rather than hidden setup state;
- visual simulation predicates compile directly to the shared CPU/CUDA
  condition bytecode. Existing text condition scripts migrate losslessly via
  a hidden compatibility node that is not offered in the toolbox;
- current native evaluator shapes still lower to their existing optimized CPU/
  CUDA implementations where possible;
- a generic expression runtime handles compositional scalar/vector/rotation
  math, comparisons, Boolean logic, state predicates, objective filters,
  first-time predicates, box membership and prism membership when no native
  evaluator matches;
- the same supported generic expression programs lower to ForeverValidator's
  typed CUDA expression bytecode and run on regular CUDA and Fast CUDA. Prism
  geometry is flattened once per evaluator and referenced from bytecode rather
  than copied into every thread. Precise finish time remains a native-only CUDA
  objective and is rejected explicitly if embedded inside a generic expression;
- runtime-only v3 configurations remain authoritative without being forced
  through the lossy v2 model, while a subsequent legacy edit deliberately
  takes authority back;
- one visual mutation window retains one seed identity even when it lowers to
  several native modifier configurations;
- viewer-backed point/rotation/box/prism pickers populate ordinary typed value
  blocks, and paired mutation min/max values use compact typed range pills;
- Blockly follows ForeverTAS's Appearance setting live: workspace, toolbox,
  flyout and editor chrome switch between the existing light and dark palette
  without reloading or rebuilding the semantic program;
- multi-parameter process/objective blocks use catalog-driven wrapped input
  rows so composition stays compact instead of growing into one horizontal
  form-like chain;
- the obsolete native-QML block renderer is removed from production and its
  unreachable historical smoke-test body has been deleted;
- a 500-block bridge/parse/validate/compile/apply/persist stress regression is
  part of the native test suite;
- final verification after the prism/layout changes is green on the 13-test
  non-CUDA matrix, the 24-test CUDA-enabled ForeverTAS matrix, the 22-test
  ForeverValidator CUDA matrix, and Reference/regular-CUDA/Fast-CUDA generic
  expression parity including prism membership.

## Goal

ForeverTAS should expose the search system as a real visual programming
language, not as a settings form drawn with blocks. Blocks must be small,
orthogonal, strongly typed, freely composable, and pleasant to edit at the
scale of a serious TAS search program.

The concept image is the minimum interaction/design target. The semantic target
is deliberately more modular: phrases such as “minimum distance to target” are
not catalog entries. They are programs assembled from independent blocks such
as `minimize`, `distance`, `car position`, `point`, and `during`.

### Composition rule

A block may hide configuration, but never hidden sequential work. If an action
technically happens inside a larger process, it is either a visible statement
inside that process or a true parameter of it. Mutation, simulation and result
selection are therefore separate steps inside each bruteforce iteration. A
mutation seed is a parameter of the mutation scope. A simulation predicate is
a parameter of simulation because it filters ticks while that simulation is
running. This rule takes precedence over mirroring legacy option registries.

The left timeline and Race Viewer are outside this work. The editor lives in the
right-panel Blocks workbench and may widen that panel while active.

## Library decision

Use upstream Google Blockly for the block-editor interaction layer and Qt
WebEngine + Qt WebChannel for embedding/bridging it into the Qt Quick app.
ForeverTAS remains responsible for the language and semantics.

Blockly owns:

- rendered block geometry and typed connection affordances;
- statement/value nesting, drag/snap, insertion markers and flyouts;
- zoom/pan, keyboard navigation, copy/duplicate/delete and undo/redo;
- toolbox/category interaction and block selection;
- renderer/theme mechanics and workspace serialization mechanics.

ForeverTAS owns:

- block type definitions and category taxonomy;
- the normalized C++ program model and versioned persistence;
- validation/type checking and diagnostic messages;
- lowering/compilation into search algorithms, mutators and evaluators;
- TrackMania-specific target editors, viewer pickers and domain values;
- C++/CUDA execution and parity.

Do not fork Blockly. Vendor a pinned production bundle and its license; keep the
source package/version metadata used to reproduce the bundle.

## Language model

### Value types

The editor uses semantic types rather than “anything fits anywhere”. Initial
closed set:

- `Number`: dimensionless scalar.
- `Milliseconds`: simulation timestamp or duration in ms.
- `Meters`: world distance.
- `MetersPerSecond`: linear speed.
- `Percent`: normalized user-facing percentage.
- `Integer`: discrete count/seed/index.
- `NumberRange`: compact constant numeric min/max pair.
- `IntegerRange`: compact constant count min/max pair.
- `Boolean`: predicate.
- `Vector3`: world/local vector.
- `Position3`: world position; structurally vector-like but not implicitly
  interchangeable with velocity/direction.
- `Direction3`: normalized-or-normalizable direction.
- `Rotation3`: yaw/pitch/roll target.
- `Pose3`: position + rotation.
- `Volume`: geometric membership target.
- `InputChannel`: steer / accelerate / brake.
- `Score`: top-level objective expression, not a raw number.
- `TimeRange`: evaluation or mutation observation range.

Compatible numeric units can use explicit conversion blocks. Do not silently
connect `Milliseconds` to `Meters`, or a raw `Vector3` to a `Position3` socket.

### Shapes

- **Hat**: program root (`when search starts`). Exactly one active root.
- **Command**: sequential operation with previous/next statement connections.
- **Control**: command with one or more statement substacks (`repeat`, `if`,
  mutation scope).
- **Reporter**: typed value output with rounded shape.
- **Predicate**: Boolean reporter with hexagonal shape.
- **Objective**: typed `Score` reporter. Objective blocks compose ordering,
  observation range, filters and a measured reporter.

The C++ model must not special-case an `evaluator` child. Any named input may
own a child expression and any control block may own named statement lists.

### Node representation

Version 3 normalized node:

```
Node {
  id
  definitionId
  fields: { key -> literal }
  inputs: { key -> childNodeId }
  statements: { key -> [childNodeId...] }
  x, y               // only meaningful for top-level nodes
}
Program {
  version: 3
  root: nodeId
  topLevel: [nodeId...]
  nodes: { id -> Node }
}
```

`inputs` replaces the old numeric-only `reporters` map and the special
`evaluator` slot. `statements` replaces the single unnamed `substack` vector.
Version-2 data is migrated on load and is never written again after a
successful migration.

### Definition representation

A block definition declares:

- stable id, display label, category, color role and visual shape;
- optional output type;
- ordered literal fields;
- ordered typed value inputs, each with a default shadow block where useful;
- ordered statement inputs with allowed command families;
- lowering opcode / semantic role, not a UI-specific implementation name;
- optional domain editor metadata for special values.

A definition is valid independently of Blockly. Blockly definitions are
generated from this catalog so the UI cannot drift from compiler semantics.

## Initial catalog

The first professional catalog intentionally favors reusable primitives over
one-off evaluators.

### Flow

- `when search starts` (Hat)
- `mutate during [TimeRange] seed [Integer]` (Control, inside an iteration)
- `keep best by [Score]` (Command, after simulation)
- `repeat N` and conditional execution are deliberately outside the current v3
  language until the search runtime has explicit semantics for them

### Search

- `bruteforce [policy parameters]` (Control) with an `each iteration` stack
- `passes [Integer]` where this is an actual independent semantic knob

Canonical shape:

```
when search starts
  bruteforce promote best [false]
    each iteration
      mutate during [time range] seed [integer]
        <physical mutation operations>
      simulate until [milliseconds] where [predicate]
      keep best by [score]
```

### Simulation / state reporters

- `car position` -> Position3
- `car velocity` -> Vector3
- `car local velocity` -> Vector3
- `car speed` -> MetersPerSecond
- `car yaw`, `pitch`, `roll` -> Number/Angle when angle type is added
- `checkpoint count` -> Integer
- `stunt points` -> Number
- `simulation time` -> Milliseconds
- state flags such as `sliding`, `freewheeling`, wheel contact -> Boolean
- `simulate until [Milliseconds] where [Boolean]` is the iteration process
  step; the predicate is evaluated per simulation tick.

### Targets / geometry

- `point x y z` -> Position3
- `direction x y z` -> Direction3
- `pose [position] [rotation]` -> Pose3
- `box center size` -> Volume
- `prism ...` -> Volume
- viewer picker actions populate the same point/rotation/box/prism value
  blocks rather than defining separate evaluator types.

### Math / vectors

- number, integer, compact `[min ... max]` ranges and typed-unit literals/shadows
- +, -, ×, ÷, min, max, abs, clamp
- vector x/y/z, magnitude, normalize, dot, distance
- explicit `km/h` conversion for speed display/threshold composition
- comparisons `< <= = >= >`, `and`, `or`, `not`

### Time

- `from [Milliseconds] to [Milliseconds]` -> TimeRange
- `at [Milliseconds]` -> TimeRange (single sample)

### Objectives

- `minimize [Number-like reporter] during [TimeRange]` -> Score
- `maximize [Number-like reporter] during [TimeRange]` -> Score
- `first time [Predicate] during [TimeRange]` -> Score
- `only when [Predicate]` objective filter

This yields the requested point target as, conceptually:

```
minimize
  distance
    car position
    point (x y z)
  during (from 3000 ms to 5500 ms)
```

No `minimum distance to point` block exists.

### Mutation primitives

Keep mutation operations specialized by the physical edit they perform, but
split independent parameters into typed reporter inputs instead of large forms.
For example:

```
shift events
  count [1 .. 3]
  max shift [100 ms]
```

and:

```
insert steering
  count [0 .. 2]
  value [-1 .. 1]
  max hold [200 ms]
```

Shared window/seed belongs to the surrounding mutation-window control block.
Do not reproduce registry implementation artifacts as user-visible blocks.

## Evaluation lowering strategy

The language must stay more general than today’s engine without regressing
CUDA.

### Stage A: structural composition + native lowering

Compile objective trees into the existing registered evaluator configurations
when they match a native evaluator capability. Examples:

- `minimize(distance(car position, point)) during range` -> point evaluator;
- `minimize(pose error(...)) during range` -> pose evaluator;
- `maximize(magnitude(car velocity)) during range` -> velocity evaluator;
- `maximize(dot(car velocity, direction)) during range` -> projected velocity;
- finish/stunt/volume constructs -> their current specialized evaluators.

This keeps all existing optimized CPU/CUDA behavior while removing monolithic
blocks from the language.

### Stage B: generic expression evaluator

The typed runtime expression program handles combinations that do not map to a
native specialization. The compiler keeps the native fast path when a known
objective shape matches and uses the generic evaluator otherwise. CPU and CUDA
receive the same visual-program semantics rather than backend-specific objective
rewrites.

### Stage C: generic CUDA expression program

ForeverValidator exposes a typed CUDA expression evaluator with scalar, vector,
rotation and Boolean stack values. ForeverTAS lowers the supported visual
expression operations into that program, validates stack shape/depth before
launch, and runs the same objective on regular and session-specialized CUDA.
The parity suite covers ordinary score expressions, first-time predicates and
generic prism-membership predicates.
Generic expressions that require precise finish-time sampling are rejected
before Start and users must use the native precise-finish objective instead.

## Blockly bridge

### Embedding

- `WebEngineView` fills the Blocks workbench host.
- Local `qrc:`/resource HTML only. No remote CDN/runtime network dependency.
- `QWebChannel` exposes one narrow `BlockEditorBridge` QObject.
- The page has a strict content-security policy appropriate for bundled
  assets; external navigation is disabled.

### Bridge API

C++ -> editor:

- `catalogJson()` — generated semantic block definitions/categories.
- `workspaceJson()` — normalized program snapshot converted to editor state.
- `workspaceRevision()` — accepted native/editor workspace revision.
- `diagnosticsJson()` — native parse/type/compiler diagnostics.
- `editable()` — whether the editor may currently submit changes.

Editor -> C++:

- `applyWorkspace(json, revision)` — debounced structural snapshot after a
  Blockly transaction; C++ parses/type-checks atomically before accepting.
- `editorReady()` / `requestNativeWorkspace()` — editor synchronization.
- `selectedViewerTargetJson(kind)` — current viewer-backed target state.
- `requestViewerPointPick(blockId)` / `completeViewerPointPick(...)` — point
  picker handoff without giving JavaScript direct viewer authority.

C++ publishes property-change signals for workspace JSON/revision, diagnostics
and editability, plus explicit viewer-point-pick request/completion signals.
Revision numbers prevent a local edit echo from replacing the same in-flight
workspace.

Never execute generated JavaScript as search logic. Blockly is an editor;
C++ compilation remains authoritative.

## Editor UX target

- dark Scratch-like canvas with subtle dot/grid texture;
- persistent left category rail, colored category icons/labels, flyout next to
  it, large canvas to the right;
- category palette stays compact; search filters blocks by label/aliases;
- Geras/Thrasos-style rounded Blockly renderer, customized spacing/radius only
  through public renderer/theme APIs;
- inline shadow values for small numeric literals; no separate inspector for
  ordinary scalar parameters;
- context menus for duplicate/delete/collapse/help;
- Ctrl/Cmd+Z/Y, delete/backspace, copy/paste, keyboard navigation;
- zoom controls + Ctrl-wheel, pan, center-on-program, zoom-to-fit;
- clear invalid-connection feedback from types;
- block warnings for semantic/compiler errors, plus one concise panel status;
- no permanent zoom badge or hand-built drag ghost once Blockly is active;
- preserve user viewport and selection across non-structural C++ updates.

Target visual density: ordinary command blocks ~30–38 px high, reporters as
small as their inline content, and no card-with-form treatment.

## Persistence and compatibility

- Persist normalized C++ program JSON version 3.
- Store Blockly workspace UI metadata only when it has no semantic equivalent
  (collapsed state, optional comments); semantic tree always round-trips from
  C++.
- On first v2 load, migrate old evaluator blocks into v3 objective trees and
  old `reporters`/`substack` into named `inputs`/`statements`.
- Preserve a corrupt/legacy backup before destructive migration.
- Migration is deterministic and covered by golden fixtures for every existing
  evaluator/modifier configuration.

## Build and packaging

- Add Qt WebEngineQuick and WebChannel as required GUI dependencies.
- Initialize Qt WebEngine before QApplication construction.
- Bundle Blockly locally and add its Apache-2.0 notice/source-version metadata.
- Extend Linux aqt module installation with `qtwebengine` and required runtime
  dependencies.
- Extend Windows deployment smoke tests to assert WebEngine/WebChannel DLLs,
  resources/locales and the QtWebEngineProcess helper are present.
- Keep a `BLOCKLY_VERSION`/lockfile so release builds are reproducible.

## Test strategy

### Core language

- definition schema/type compatibility tests;
- cycle/reachability/ownership tests for arbitrary named inputs/statements;
- v2 -> v3 migration goldens;
- JSON round-trip and deterministic serialization;
- malformed/untrusted workspace rejection and size/depth limits.

### Compiler

- one golden lowering test per supported objective composition;
- equivalent differently-parenthesized math trees where semantics permit;
- clear errors for wrong units/types and the intentionally rejected CUDA
  composition (generic precise finish time);
- parity against each old monolithic evaluator configuration during migration.

### Editor bridge

- JS unit tests for generated definitions, workspace normalization and event
  debounce/revision handling;
- bridge tests with malformed JSON and out-of-order revisions;
- no-network test: editor must work with outbound network disabled.

### GUI

- QML smoke test: WebEngine loads local editor and bridge reaches ready state;
- scripted browser-side workspace tests for connect/disconnect/duplicate/delete,
  nested reporters, statement insertion and serialization;
- screenshot/reference checks at 100%, 125%, 150% scaling and light/dark app
  modes where maintained;
- manual Linux Wayland/X11 and Windows portable verification before release.

### Performance

- 500-block editor interaction target with no multi-frame model rebuild for a
  scalar edit;
- bridge snapshots debounced/coalesced; no persistence write per mousemove;
- compiler linear-ish in reachable nodes with explicit maximum node/depth
  guards;
- search hot loop receives compiled native/generic evaluator once at Start;
  Blockly/WebEngine has zero participation in simulation iterations.

## Original delivery sequence

The language core, production Blockly editor, generic CPU evaluator and generic
CUDA evaluator described through the CUDA-expansion stage are implemented in
the current worktree. The sequence below remains as architectural history and
as a checklist for release hardening rather than a statement of current missing
features.

### Weeks 1–2 — architecture and safety rails

- finalize v3 schema, typed definition catalog and migration fixtures;
- pin Blockly; prove local WebEngine/WebChannel embedding on Linux + Windows
  toolchains;
- establish bridge contract and editor CSP/no-network policy.

Exit: empty Blockly workbench loads in ForeverTAS from bundled resources and a
C++ test round-trips a v3 program.

### Weeks 3–6 — language core

- implement generic `inputs`/`statements` model and type checker;
- implement v2 migration;
- add primitive values, vectors, time ranges, math, comparisons and state
  reporters;
- implement objective expression AST and native-evaluator pattern lowering.

Exit: every current evaluator can be represented by a multi-block v3 tree and
lowers to an equivalent existing runtime configuration.

### Weeks 7–10 — production editor

- generate Blockly block definitions/toolbox from catalog;
- implement theme/renderer tokens matching ForeverTAS concept direction;
- bridge live edits, undo/redo, selection, diagnostics and persistence;
- remove old hand-built canvas from normal runtime path.

Exit: users can build/edit/save/reload all existing search semantics entirely
inside Blockly with typed sockets.

### Weeks 11–14 — mutation decomposition and domain UX

- move mutation scalar parameters into inline typed sockets/shadows;
- add channel/range/seed primitives where useful;
- viewer pickers for point/pose/volume values;
- category search, keyboard flow, context actions, copy/paste polish.

Exit: no current mutation/evaluation option is exposed as a giant settings
card.

### Weeks 15–18 — generic evaluator

- generic typed CPU expression bytecode for supported state/math/predicate ops;
- compiler fast-path selection to specialized evaluators;
- clear backend capability diagnostics and reference/optimized parity tests.

Exit: useful objective compositions beyond the old evaluator registry execute
on CPU without new evaluator classes.

### Weeks 19–21 — CUDA expansion

- generic CUDA score/predicate opcodes for the supported visual expression set;
- regular/Fast CUDA parity suite and capability checks;
- preserved specialized kernels for common fast-path objectives.

Exit: supported generic objectives have explicit, tested CUDA behavior; every
unsupported combination is rejected before search.

### Weeks 22–24 — release hardening

- 500+ block stress tests, memory/perf pass, accessibility/keyboard pass;
- packaging on Linux AppImage and Windows portable; offline launch tests;
- migration fuzzing/corruption recovery; docs/tutorial examples;
- remove obsolete QML block renderer and dead compatibility code after the
  migration boundary is proven.

Exit: release candidate with no editor-specific simulation regressions, clean
portable packaging and deterministic migration.

## Initial implementation slice

The branch began with the slice that unlocked the architecture rather than
cosmetically polishing the obsolete editor:

1. introduce v3 type/slot definition structures alongside v2;
2. add compositional objective definitions (`minimize`, `maximize`, `distance`,
   `car position`, `point`, `time range`) and compiler lowering for point/speed
   native evaluators;
3. add migration tests proving the old point-target evaluator becomes the
   multi-block objective tree;
4. embed a locally bundled pinned Blockly editor through WebEngine/WebChannel;
5. generate those initial definitions/toolbox from the C++ catalog;
6. replace the Blocks workbench host with the embedded editor and validate the
   real app, while leaving Setup/Run and the viewer untouched.
