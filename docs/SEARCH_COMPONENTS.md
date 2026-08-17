# Search Components Architecture

This document describes how ForeverTAS turns a Scratch-like block program
into a running bruteforce search. It is the primary reference for adding
search features without coupling the controller, the workspace UI, or the
search engine to individual implementations.

## Feature Model

Users compose one search script from small, single-purpose blocks:

```text
search: basic bruteforce
  scored by (evaluation goal)
  apply input mutations:
    [mutate inputs in window 1000→5990, seed 7]
        op: nudge steering
        op: shift existing events
        op: flip accelerate presses
    [mutate inputs in window 2000→4000, seed 9]
        op: insert brake presses
```

The workspace keeps a single script: one **search block** (a hat that binds
a registered search algorithm), one **evaluation goal** plugged into its
evaluation slot, and an ordered stack of **mutation windows**. Each window
is a container block owning the shared from/to times and RNG seed, and
holds an ordered substack of **mutation atoms** — blocks that each do one
input operation with one small configuration. Value reporters (`number`,
`+`, `−`, `×`, `÷`, `min`, `max`) may be plugged into number slots to
compute settings at compile time.

The application requires at least one non-empty window before a search can
start, matching the original composition rules.

Registered options are never blocks themselves. Every block lowers to the
registered options behind it at compile time (see "Lowering"), which is
what keeps each block trivial while the engine keeps its stable,
string-keyed configuration surface.

## Directory Organization

```text
src/
├── blocks/                      # Qt-free block core
│   ├── block_catalog.h/.cpp     # atomic block vocabulary (windows, ops,
│   │                            # evaluation goals, value primitives)
│   ├── block_program.h/.cpp     # placed-block representation and edits
│   ├── block_program_io.h/.cpp  # JSON persistence (v2, migrates v1) and
│   │                            # the text interchange (parser + printer)
│   ├── block_compiler.h/.cpp    # program → search components and
│   │                            # component validation
│   ├── block_lowering.h/.cpp    # atoms ↔ registered options, both
│   │                            # directions (compile + migration)
│   ├── block_expression.h/.cpp  # number-reporter evaluation
│   └── block_value.h/.cpp       # locale-independent number helpers
│
├── searches/
│   ├── algorithm_registry.h/.cpp  # option registries (internal ABI)
│   ├── option_configuration.h     # settings transport
│   ├── option_fields.h            # typed field schema
│   └── ...
│
└── app/
    ├── block_program_model.h/.cpp  # QML-facing editing model,
    │                               # persistence, legacy migration
    ├── option_settings_store.h     # legacy per-option settings loader
    ├── search_controller.h/.cpp    # application coordination
    └── search_worker.h/.cpp

qml/
├── Main.qml
└── blocks/
    ├── BlockWorkspace.qml       # palette + script column with windows
    ├── BlockView.qml            # generic block rendering
    ├── BlockSlot.qml            # typed value slots and reporter chips
    └── target-picker detail components for collection-backed goals
```

## Block Vocabulary

`src/blocks/block_catalog.*` declares one block per behavior. Every block
binds to the registered option it lowers to:

| Block          | Id                            | Shape     | Lowers to             |
|----------------|-------------------------------|-----------|-----------------------|
| search hat     | `search/basic-brute-force`    | Hat       | basic-brute-force     |
| window         | `mutate/window`               | Container | (owns window + seed)  |
| reroll         | `mutate/reroll-steering`      | Stack     | random-steering      |
| shift events   | `mutate/shift-events`         | Stack     | existing-event-perturbation |
| nudge steering | `mutate/nudge-steering`       | Stack     | existing-event-perturbation |
| set steering   | `mutate/set-steering`         | Stack     | existing-event-perturbation |
| flip accel     | `mutate/flip-accelerate`      | Stack     | existing-event-perturbation |
| flip brake     | `mutate/flip-brake`           | Stack     | existing-event-perturbation |
| insert steer   | `mutate/insert-steering-at`   | Stack     | input-insertion (absolute) |
| adjust steer   | `mutate/adjust-steering-by`   | Stack     | input-insertion (offset) |
| press accel    | `mutate/press-accelerate`     | Stack     | input-insertion       |
| press brake    | `mutate/press-brake`          | Stack     | input-insertion       |
| delete steer   | `mutate/delete-steering`      | Stack     | input-deletion        |
| delete accel   | `mutate/delete-accelerate`    | Stack     | input-deletion        |
| delete brake   | `mutate/delete-brake`         | Stack     | input-deletion        |
| deformations   | `mutate/smooth-steering`      | Stack     | smooth-steering       |
| finish time    | `evaluate/finish-time`        | Reporter  | precise-finish-time   |
| stunt points   | `evaluate/stunt-points`       | Reporter  | stunt-points          |
| speed          | `evaluate/speed`              | Reporter  | velocity (total)      |
| speed toward   | `evaluate/speed-toward`       | Reporter  | velocity (projected + alignment gate; a threshold of −100 disables the gate) |
| distance point | `evaluate/distance-to-point`  | Reporter  | point-target          |
| distance pose  | `evaluate/distance-to-pose`   | Reporter  | pose-target           |
| box entry      | `evaluate/box-entry-time`     | Reporter  | volume-entry-time     |
| prism entry    | `evaluate/prism-entry-time`   | Reporter  | custom-volume-entry-time |

Value primitives (`values/number`, `values/add`, `values/subtract`,
`values/multiply`, `values/divide`, `values/minimum`, `values/maximum`)
are declared directly in the catalog and output `number` values.

What used to be mode enums and per-channel toggles is now vocabulary:
`velocity`'s total/projected enum is the `speed` / `speed-toward` pair,
perturbation's delta/absolute mode is the `nudge` / `set` pair, insertion's
offset/absolute mode is the `adjust` / `insert-at` pair, and every channel
flag became its own press/flip/delete atom. Evaluation coordinates
(formerly hidden "mirrored" fields) are ordinary number slots; the target
collections act as pickers that fill them.

A placed program is a flat pool of `BlockNode`s (`blocks/block_program.h`):
each node stores its definition id, literal field values, reporter links
per field key, and — for hats and containers — the ordered substack (hats
additionally the evaluator id). Structural edits (attach, graft, detach,
move, remove) keep unreachable nodes garbage-collected, so persistence and
undo stay simple.

## Typed Field Schema

`src/searches/option_fields.h` defines `OptionField`, the typed
description of one settings key:

```text
key, label
kind: Number | Line | Enum | Boolean | Mirrored
defaultValue (exact settings string)
enumValues, minimum/maximum/decimals/step (numbers)
isSeed (participates in seed randomization)
group (display grouping)
mirrorAsset ("cuboid" | "custom-volume" | "pose")
```

Registrations declare their `fields` next to their `defaultSettings`; the
registry tests enforce coverage. Blocks reuse the same schema: each atom's
field list is a subset of its option's settings keys with byte-identical
defaults (enforced by the block tests), so lowering stays byte-stable.
`Mirrored` fields remain available to the registries but no block uses
them — goals own their values as ordinary fields.

## Compiler and Lowering

`blocks::CompileProgram(program)` walks the script and produces a
`SearchComponentConfiguration` (algorithm, ordered modifiers, evaluation
target as `OptionConfiguration`s):

- Field values come from literals or evaluated number reporters
  (arithmetic evaluates bottom-up; division by zero fails the compile).
- Each mutation window lowers through `blocks::LowerMutationGroup`:
  atoms bound to the same registered option merge into **one**
  configuration — which is what makes a migrated window behave exactly
  like the legacy multi-feature block it came from — while atoms of
  different options in one window lower to sequential passes sharing the
  window and seed (order: first appearance).
- Channel enablement and modes are derived from which atoms are present;
  keys for absent features are neutralized (shift 0, delta 0..0, toggles
  off). Two atoms of one window disagreeing on a shared key, or two
  conflicting steering modes (`nudge` + `set`, `adjust` + `insert-at`),
  produce an actionable compile error suggesting another window.
- Evaluation goals lower through `LowerEvaluationAtom`, pinning the
  fixed parts of their option (for example `speed-toward` pins
  `mode = projected` and derives `alignmentEnabled` from whether the
  alignment threshold exceeds −100).

`blocks::ValidateSearchComponents(...)` then applies the shared
validation: registry validation, mutation windows versus the Simulation
horizon (silently clamped), and the evaluation plan versus the horizon
and first mutation time. `RunSearch` consumes the compiled request
unchanged, so the search engine remains independent of the block system.

Within one window the op order in the UI does not change behavior (the
engine applies a pass's operations in its own fixed order); across
windows, order matters.

## Interchange Formats

- **JSON** (`blocks/program` in the platform settings store, version 2)
  is the full-fidelity persistence format, including block positions,
  loose canvas blocks, and remembered field values. Version 1 documents
  (option blocks) migrate on load by compiling to components and
  expanding through the lowering table; loose value blocks survive as
  literals at their positions, and legacy remembered values are dropped.
- **Text** is the human-editable interchange for the semantic program:

  ```text
  search basic-brute-force {
    autoPromoteBest = false
    evaluate speed {
      minTimeMs = 0
      maxTimeMs = 6000
    }
    mutate window {
      minTimeMs = (1000 + 500)
      maxTimeMs = min(5990, 7000)
      seed = 1179926867
      op smooth-steering {
        deformationCount = max(1, 2)
        radiusMs = 200
        amplitudeMin = -0.2
        amplitudeMax = 0.2
      }
    }
  }
  ```

  Parenthesized expressions rebuild the matching value-reporter blocks;
  printing a program always produces a stable, byte-identical document.
  The parser also accepts the previous option-id spelling
  (`mutate random-steering { ... }`, `evaluate velocity { ... }`) and
  expands it to the equivalent atoms; expressions in legacy entries must
  evaluate numerically.

## Legacy Migration

`BlockProgramModel` builds the program from the pre-block configuration
on first launch: `selection/searchAlgorithm` plus
`configuration/search/*`, the `composition/modifiers` JSON array, and
`selection/evaluationTarget` plus `configuration/evaluation/*`, including
legacy aliases (`finish-time` → `precise-finish-time`, `maximum-speed` →
`velocity`) and the single-modifier `selection/mutationAlgorithm` layout.
Both this path and JSON v1 load go through
`blocks::BuildProgramFromComponents`, the inverse of compilation, so a
migrated program recompiles byte-identically. One documented corner:
`velocity` configured for total speed *with* the alignment gate migrates
to `speed-toward` (direction and gate preserved, measure projected).

The result is persisted as `blocks/program`; legacy keys are left in
place but no longer read. Remembered field values seed a replaced
evaluator or hat with the last values used for that block definition.

## Controller Responsibilities

`SearchController` owns application coordination only:

- Worker-thread lifecycle, paths, base-script handling, status, progress.
- The target collections (cuboids, custom volumes, poses) as app-level
  editing aids; their detail components copy the selected target's
  geometry into the block's own fields and keep it in sync while edited.
- QML properties and change notifications that delegate to
  `BlockProgramModel`.

`evaluationTargetId` is derived from the script's evaluation block (it
reports the registered option id, e.g. `velocity` for both speed goals);
writing it replaces the evaluator block. Neither class contains a field,
property, or method named after a concrete target or modifier.

The QML-facing editing API is:

```text
blockPalette
blockScript                      (hat, evaluator, groups with atoms)
blockData(blockId)               (rendering data: fields, chips, detail)
addBlock(definitionId)
removeBlock(blockId)
setBlockField(blockId, key, value)
attachReporter(blockId, key, reporterDefinitionId)
detachReporter(blockId, key)
moveBlock(blockId, toIndex)      (atoms within a window, windows within
                                 the script)
setEvaluatorBlock(definitionId)
resetBlocks()
applyProgramText(text)
programText
```

Seed randomization on Start rewrites every `isSeed` field — the windows'
seeds, in substack order — preserving the legacy deterministic stream.

## QML Ownership

`Main.qml` places one `BlockWorkspace` inside the "Search blocks"
section. The workspace renders the palette (categories: Search,
Evaluate, Mutate, Values) and the script column:

- Clicking a search block replaces the script hat.
- Clicking an evaluation goal replaces the evaluator slot.
- Clicking a mutation window appends it to the script; clicking a
  mutation atom snaps it into the last window (creating one when none
  exists yet).
- Clicking a value block plugs it into the armed number slot (slots arm
  via their ⊕ button and highlight while armed).
- Windows and atoms reorder with ↑/↓ and leave the script with ×.

`BlockView` renders any block from its definition data, including
containers; `BlockSlot` renders the typed inline editors (number scrub
fields, combos, checkboxes, line edits) or the reporter chip plugged
into the slot. Field edits refresh block data in place, so delegates and
focus survive programmatic updates.

Collection-backed goals load a **detail component** under the block
(`VolumeEntryBlockDetail.qml` for box and prism goals,
`PoseTargetBlockDetail.qml` for poses). A detail component receives
`controller`, `viewer`, `viewport`, `blockId`, and `blockInformation`,
edits the target models, and writes the block's fields via
`controller.setBlockField` — the block owns its values, and the picker
keeps them in sync while its target stays selected.

Reusable field/layout components belong in `qml/settings/`; block
rendering belongs in `qml/blocks/`.

## Adding a Modifier

Assume a new modifier named **Steering Jitter** with ID `steering-jitter`.

1. Create `src/mutations/steering_jitter_mutator.h/.cpp`.
2. Implement `InputMutator`, including `EarliestMutationTimeMs()`.
3. Declare `SteeringJitterOptionFields()` next to the defaults: one
   `OptionField` per settings key, using `SeedField` for the seed and
   `AppendWindowFields` for the shared time window.
4. Reject unknown keys and parse every declared key.
5. Add one `ModifierRegistration` entry referencing the field list.
6. If the option is multi-feature, expose its features as atoms: declare
   the atom blocks in `block_catalog.cpp`, extend the assembler in
   `block_lowering.cpp`, and extend `ExpandModifierAtoms` so the option
   and its atoms remain mutual inverses (byte-identical round trips).
   Single-op options only need one atom definition and trivially map
   through the direct assembler.

No QML file, workspace change, controller change, or compiler change is
needed: the palette entries, typed slots, persistence, text interchange,
and validation are all derived from the catalog and lowering table.

## Adding an Evaluation Target

1. Create target files under `src/evaluators/`.
2. Implement `IterationEvaluator` and a per-iteration session.
3. Declare the option's `fields` (`MirroredField` entries are no longer
   used by blocks; prefer ordinary fields).
4. Return a clear target-owned `EvaluationSample::description`.
5. Add one `EvaluationTargetRegistration` entry.
6. Declare the goal's atom in `block_catalog.cpp` and wire it in
   `block_lowering.cpp` (`LowerEvaluationAtom`, `ExpandEvaluationAtom`,
   `EvaluationAtomDefinitionForOption`).
7. Only if the target manages a collection: create a detail component in
   `qml/blocks/` and reference it from `settingsComponent`; the
   component writes the block's fields.

## Adding a Search Algorithm

1. Implement `SearchAlgorithm` under `src/searches/`.
2. Keep only search-policy settings in its field schema.
3. Consume the generic mutator and evaluator contracts.
4. Add one `SearchAlgorithmRegistration` entry; the hat block derives
   from the registry automatically.

## Testing Checklist

Before submitting a new component:

1. Build with the strict warning flags.
2. Run all CTest targets, including the data-gated viewer QML smoke
   when a replay is available.
3. Extend `tests/block_program_tests.cpp` when adding core behavior —
   every new atom or option pair gets an entry in the lowering
   equivalence table (expand → compile must reproduce the configuration
   byte for byte).
4. Confirm field schemas cover `defaultSettings` exactly (enforced by
   the registry tests) and atom defaults match option defaults (enforced
   by the block tests).
5. Confirm IDs appear only in registry code, lowering/migration code,
   and registry assertions.
6. Confirm no ID switch or option-specific controller field was
   introduced.
7. Confirm repeated windows preserve independent settings and order.
8. Confirm invalid settings and atom conflicts produce actionable
   messages.
