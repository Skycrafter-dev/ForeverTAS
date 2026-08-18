# Blocks workbench guide

The **Blocks** tab is the visual search-program editor. Blockly handles block
editing and navigation; ForeverTAS converts the workspace into a typed native
program, validates it, and only then applies it to the search runtime.

## Basic search shape

A normal program has this structure:

```text
when search starts
  bruteforce
    each iteration
      mutate during ...
        ... mutation blocks ...
      simulate until ... where ...
      keep best by ...
```

The order is meaningful. Mutation changes candidate inputs, `simulate` runs
that candidate, and `keep best` evaluates eligible simulated states.

## Example: maximize speed

For a minimal speed search:

1. Put a **mutation window** inside `bruteforce / each iteration`.
2. Add **reroll steering** inside the mutation window.
3. Set the mutation time range and seed.
4. Add **simulate** after the mutation window and choose the simulation horizon.
5. Leave `where` as `true` to make every simulated tick eligible.
6. Add **keep best by**, then compose `maximize -> car speed -> during`.

The objective range is independent from the mutation window and simulation
horizon. It controls which eligible states are compared, not which inputs are
mutated or how long the candidate is simulated.

## Example: approach a point only while moving toward it

Compose the score from small reporter blocks instead of selecting a monolithic
target:

```text
only when
  score
    minimize
      distance
        car position
        point (x, y, z)
      during (from ... to ...)
  condition
    dot
      normalize(car velocity)
      direction (x, y, z)
    >= 0
```

Point, direction, scalar and time blocks are ordinary typed values. Incompatible
sockets do not connect, and native validation repeats the type check before the
runtime configuration changes.

## Example: filter simulation with geometry

`simulate ... where` accepts the same composable Boolean predicate language on
CPU, regular CUDA and Fast CUDA. For example:

```text
simulate until 6000 ms where
  car position inside
    prism
      plane XZ
      origin (0, 0, 0)
      depth 5 m
      polygon ...
```

An axis-aligned **box** can be used instead of a prism. Geometry predicates can
also be combined with `and`, `or`, `not`, comparisons, vector math and state
reporters. The simulation predicate filters eligible ticks; it does not change
the physics simulation itself.

## Mutation windows

A mutation window owns one time range and one seed. Every mutation statement
nested inside that window shares that seed identity even if the compiler lowers
the visual window into several native modifier configurations. Put separate
mutation windows in the iteration stack when the edits need independent ranges
or seeds.

Mutation blocks are deliberately single-purpose: steering edits, event shifts,
button presses/releases and deletions are separate statements rather than one
large settings block.

## Viewer-assisted targets

Point, rotation, box and prism values can be populated from the Race Viewer.
Select the relevant target block and use its viewer action. The selected viewer
geometry is copied into normal typed block values; the search does not depend
on live viewer state afterward.

## Keyboard and search

- `Ctrl+F` / `Cmd+F` focuses block search.
- `Escape` clears and closes search results.
- Tab can move focus into the embedded editor.
- Blockly provides its standard keyboard navigation, copy/paste, delete,
  duplicate and undo/redo behavior.
- The editor becomes read-only while a search is running; stop the search to
  edit the program again.

## Backend notes

Supported generic objective and predicate compositions have matching Reference,
regular CUDA and Fast CUDA behavior. Precise finish time is the deliberate
exception: use the dedicated native precise-finish objective rather than
embedding precise finish time in an arbitrary generic CUDA expression.

The Blockly page is bundled with ForeverTAS and has no network dependency.
Semantic version-3 program JSON is the authoritative persisted representation;
Blockly workspace data is only editor state and can be regenerated if needed.
