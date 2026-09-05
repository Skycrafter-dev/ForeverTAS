# Blocks workbench guide

Choose **Program… → New program**, then connect commands beneath **when run starts**. **Run** executes that visible graph; **Debug** starts paused. Loops, variables, procedures and individual input/state operations are ordinary language constructs, not settings for a predefined search.

## Start from editable sequences

Open **Macroblocks** in the toolbox, or search for a template by name. **Inputs** contains existing-event mutation, deletion, insertion/holds, steering rerolls and smooth steering. **Conditions** contains common tests and per-tick rejection. **Targets** contains scoring and first-entry/finish loops. **Search** contains a brute-force example assembled from primitives.

Inserting a template creates a normal stack of blocks, with fresh temporary names. Attach it to your program and edit any part. There is no wrapper to expand and no native macro implementation running behind it. Undo removes the inserted sequence as one edit. To reuse your modified sequence, move it inside a **My Blocks** definition and replace its initial values with procedure parameters.

## Fine control

**Advance one tick** steps exactly once. Input reporters return a new sequence after changing or deleting one specific event; use the resulting sequence explicitly. Timestamp edits do not silently sort, merge, clamp or randomize other records. Construct targets from points, vectors, rotations, boxes and polygon point lists. Compare values and store a snapshot yourself when it qualifies.

**Keep this result** and **keep saved result** publish unconditionally. Your visible comparisons decide whether a candidate is better and whether to replace the baseline.

## Inspect and save

Use the debugger toolbar and block context-menu breakpoints to inspect the source, program variables, local variables and simulation state. **Program…** also provides save/open, reusable library import and feedback-control/parallel-branch examples.

See [Blocks simulation sandbox](BLOCKS_SANDBOX.md) for exact input units, macro semantics, events, parallel execution, persistence and runtime limits. Legacy visual-to-native search compilation and its opaque mutation/evaluation blocks have been removed.
