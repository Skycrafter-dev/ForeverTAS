# Adversarial audit — ForeverTAS-scratch-system (2026-08-17)

Complete issue list from the adversarial audit (3 parallel deep-audit passes over
`src/app`, `src/blocks|searches|evaluators|mutations|conditions`, the QML layer,
plus a full manual pass over `Main.qml`). Issues are fixed one by one; each
section records what was done.

## Resolution log (fix commits on top of the audited baseline)

- `d7fecf7` — **A1, A2, A5, A6, C5, C6, D7, dead file**: the whole
  binding-severance/stale-state family; whiteboard text-tool data loss and
  stale-edit-index; combo popups flip upward and widen to full labels; slot
  editors widen; cuboid model emits `targetsChanged`; `SettingSwitch.qml`
  removed.
- `a8d6f4d` — **A3, A4, C7, D8, D12 (partial)**: debugger click hazards
  (breakpoint only from gutter, Return no longer inserts surprise lines),
  palette feedback hints, loading banner as info, status/glyph/tooltip polish,
  themed scrollbars, deduplicated error display.
- `a61ce8e` — **B1, B2, B3, B8 (diagnostics), D16, D17**: smooth steering
  cumulative-bump fix with restore + regression test and window-patch parity;
  expression overflow rejected; evaluation/mutation window conflict validated
  at configuration time; actionable no-satisfied-iteration message; condition
  stack-depth rejected at compile; reserved words quoted in the text
  interchange.
- `abb8345` — **B4, B5, B6, B7, D3, D4, D6, D14, m9, m10**: persist() OOB
  guards; corrupt program backup; CUDA preference kept across non-CUDA builds;
  extraction no longer clobbers concurrent edits; stale text-apply error;
  packs-hint early return; focus payload validation; numeric seed comparison;
  message fixes.
- `845efa5` — **C1, C2, C3, C4 (controller-owned), C9, D1, D13**: wheel
  chaining to the outer pane at extents + 1:1 trackpad deltas + topmost-first
  hit test; keyboard tooltips; persisted render mode; severity as state
  instead of translated-string matching; running-search edit hint; green
  focus color; dock width clamp.
- `13eb9ac` — **D12, D15 (partial), D19, D20, volume-entry UX nits**: legacy
  key migration persists forward; dropped persisted targets warn; moveBlock
  rejects negative indices; draw-polygon instructions; menu checkmarks; dead
  code removed.

## A. Critical UI state bugs (wrong data shown / silent data loss)

- **A1** `WhiteboardOverlay.qml:40-56,142-150` — Text tool: clicking a second
  spot while editing discards the in-progress annotation silently; toggling
  `active` mid-edit also cancels without committing.
- **A2** `PoseTargetBlockDetail.qml:187-195` and `VolumeEntryBlockDetail.qml:308-317`
  — target name field shows/edits the wrong target after the first rename
  (imperative write severs `text` binding; stale name then written onto newly
  selected target).
- **A3** `SimulationDebuggerPanel.qml:534-546` — single click on any code line
  toggles a breakpoint (TapHandler armed for x >= 23, i.e. whole line);
  accidental breakpoints while trying to click/scroll code.
- **A4** `BlockWorkspace.qml:312-320` — clicking a reporter block in the palette
  with no armed slot silently does nothing; core plug-a-value interaction gives
  zero feedback.
- **A5** Checked/`currentIndex`/`text` binding-severance family (imperative
  writes break `checked:`/`currentIndex:`/`text:` bindings; backing value no
  longer re-syncs): `Main.qml` packs/replay TextFields + base-input TextArea +
  trajectory toggle + take-over checkbox + dark-mode toggle; `WhiteboardOverlay`
  tool buttons/mode toggle/visibility checkbox; `BlockWorkspace` palette tabs;
  `PoseTargetBlockDetail` combo; `SettingSwitch`/`SettingCombo`; debugger
  `liveEdit.text`.
- **A6** `WhiteboardOverlay.qml:15,63-66` — stale `editingIndex` can edit the
  wrong drawing item if the list changes mid-edit.

## B. Major logic bugs (search correctness / silent misbehavior)

- **B1** `smooth_steering_mutator.cpp:75-84` — smooth steering deformation is
  cumulative per tick (each tick adds `SteeringStateAt(inputs)+delta` on top of
  previously pushed events), saturating to full lock; effective magnitude scales
  with radius/tick instead of configured amplitude.
- **B2** `block_value.cpp:36-48` + `block_expression.cpp:53-66` — arithmetic
  overflow (±inf) formats as `"0"` silently; search runs against a value the
  user never entered.
- **B3** `block_compiler.cpp:194-203` — component validation misses
  `evaluation end < earliest mutation start`; passes validate, then throws
  mid-launch in `basic_brute_force_search.cpp:1023-1038`.
- **B4** `custom_volume_target_model.cpp:949` / `pose_target_model.cpp:518` —
  unguarded `targets_[selectedIndex_]` in `persist()` (latent OOB; cuboid model
  guards, these two don't).
- **B5** `block_program_model.cpp:158-175` — corrupt stored program JSON is
  silently overwritten with defaults (no backup, no message).
- **B6** `search_controller.cpp:284-291` — non-CUDA build silently rewrites a
  stored `cuda` backend preference to `reference` (preference lost when
  switching binaries).
- **B7** `search_controller.cpp:847-875` — replay extraction clobbers base-input
  edits made while extraction ran (completion checks paths only).
- **B8** `volume_entry_evaluator.cpp:86` / `custom_volume_entry_evaluator.cpp:320`
  — baseline starting inside the volume never yields a sample (`!previous`
  branch is dead in the search loop) → whole search dies with unrelated error.
- **B9** `input_event_formatter.cpp:170-173` — script time formatting rounds up
  to the next tick; unaligned origin would shift every exported script later by
  up to 10 ms on save/reload (latent; origins aligned today).
- **B10** `block_program_model.cpp:416-420` — value-reporter palette click
  "places block on canvas" but loose top-level blocks are invisible/undeletable
  in the UI and persist forever as dead data.
- **B11** `search_controller.cpp:1364-1388` — destructor blocks unbounded on
  worker wait; packs auto-detection thread has no cancellation (exit freeze).
- **B12** `system_file_dialog.cpp:117,152` — portal file dialog: blocking D-Bus
  call + unbounded nested event loop → UI freeze if portal hangs; portal
  unavailable → silent no-op treated as "user cancelled".

## C. Major UX/usability

- **C1** `panel_wheel_redirector.cpp:104-122` — pixel vs angle delta scaling
  (trackpads ~2.5px/pixel vs wheel 120px/notch); inner list always consumes
  wheel even at extent → outer pane unscrollable once inner bottoms out (README
  promises the opposite).
- **C2** Keyboard controls are undiscoverable: manual-drive keys, give-up
  (Delete), respawn (Enter/Backspace), free camera (7), E/C vertical movement —
  nothing in the UI documents them.
- **C3** Render mode selection is not persisted across sessions.
- **C4** Severity communicated by translated-string matching:
  `Main.qml:3983-3987` (indexOf "failed"/"discarded"), `:4621-4624`
  (`statusText === qsTr("Search failed")`), `:3754-3755` (`statusText !== qsTr("No map loaded")`)
  — breaks under translation; no state enum from controller.
- **C5** `BlockSlot.qml:53-58` — fixed 64/96/140px inline editors clip real
  values; `Vector3Settings` three-column squeeze; enum combos elide with no way
  to see full text.
- **C6** `StyledComboBox.qml:113-121` — popup only opens downward; near window
  bottom it collapses to near-zero height.
- **C7** `SimulationDebuggerPanel.qml:228-255` — loading state rendered as red
  error banner; `:669-675` Enter always inserts a new line after commit.
- **C8** Whiteboard: no rename affordance for auto-named boards; error/status
  only as 10px text; drawings list not closable via Escape/outside click.
- **C9** Search edits allowed while running with no indication that the running
  search uses the start-time snapshot (misleading).

## D. Minor / polish (selection)

- **D1** `AppTheme.qml:32` — focus color is blue while accent is green.
- **D2** Misleading/hardcoded messages: horizon error hardcodes "between 10
  and" (`search_controller.cpp:1073-1077`); point-target catch blames the
  condition script (`:1043-1105`).
- **D3** Stale `programTextError` after structural edits fix the program
  (`search_controller.cpp:731-737`).
- **D4** `setPacksDirectory` clears auto-detect hint before the no-change
  early-return (`search_controller.cpp:783-793`).
- **D5** `cudaBatchSizeChanged` persists calibrated batch size over the user's
  explicit value (`search_controller.cpp:956-962`).
- **D6** `focusSelectedCustomVolume`/`focusSelectedPoseTarget` no
  canConvert-validation; pose focus size hardcoded `(4,2.5,7)`
  (`search_controller.cpp:750-781`).
- **D7** Cuboid `notifyTargetChanged()` doesn't emit `targetsChanged` (pose and
  custom-volume models do) → stale QML consumers.
- **D8** Debugger: rainbow red/green filenames, lowercase status strings,
  misleading Reset tooltip, ASCII ">"/"v" glyphs, unthemed ScrollBars, 22x22
  hover-only insert/delete buttons, duplicated error display.
- **D9** Whiteboard: hardcoded border colors, near-invisible first swatch,
  unvalidated color text field, stale editing index (see A6), 13px resize
  handle, disabled rows painted solid gray, no busy indicator during export.
- **D10** `ThemedTabButton` no elide; `ThemedButton` focus border 1→2 jitter;
  dead icon support; `ThemedMenuItem` no checkmark for checked items.
- **D11** `VolumeEntryBlockDetail` "x" vs "×" glyph inconsistency; "%1 of 3
  minimum vertices" grammar; no draw-mode instruction.
- **D12** Dead code/stale files: `settings/SettingSwitch.qml` unused,
  `qml/qmldir` stale, `cameraFocusToolbar` unused layout params, dead identical
  conditional string in `search_worker.cpp:159-162`, unused `lastCompletion_`.
- **D13** Playback dock fixed 430px width vs ~427px minimum viewport width at
  default splitter sizes (slight clip at min window).
- **D14** `randomizeSeeds` compares seeds as strings ("0042" ≠ "42" defeats
  dedupe) (`block_program_model.cpp:607-643`).
- **D15** `moveBlock` negative index cast; `setProgramText` leaves remembered
  values from discarded program; duplicate `programTextChanged` emissions.
- **D16** `block_program_io.cpp:735` — bareword values colliding with
  `min`/`max`/`evaluate`/`mutate`/`op` break the text interchange (quoted
  escape needed).
- **D17** Conditions: deep stack (>32) silently evaluates false; div-by-zero
  yields 0 while block expressions error (inconsistent subsystems).
- **D18** `EffectiveInputChangeCount` positional diff overstates mutation
  counts when insertions occur (reported statistics only).
- **D19** `option_settings_store.h` legacy keys never migrated forward (value
  lost when old key disappears); load() rewrites settings unconditionally.
- **D20** Target models silently drop invalid persisted targets and rewrite
  storage (silent data loss, no report); future version treated as invalid.

## Explicitly deferred (documented, not fixed in this pass)

- One-tick evaluation/mutation timeline asymmetry (intended, codified in tests;
  improved error messaging instead of semantic change).
- **B9** script time ceil-rounding on unaligned origins (latent — needs an
  unaligned-origin replay fixture to validate a change).
- **B10** loose reporter blocks: the palette no longer creates them silently
  (hint instead); the underlying canvas affordance is future work.
- **B11** unbounded destructor wait / packs-detection cancellation plumbing.
- **B12** portal dialog timeout/parenting and silent no-op when the portal is
  unavailable (platform work).
- Snap-Steam autodetection, Wayland portal window parenting, Windows dialog
  owner HWND (platform feature work).
- Unbounded sandbox cache growth, multi-worker improvement double-count
  (statistics-only), CUDA batch-index bookkeeping (needs dedicated parity
  testing).
- Pose score mixing meters/radians (semantic change to ranking; needs
  validation strategy).
- Whiteboard board renaming, drawings list as a real Popup, export busy
  indicator (feature work).
- `Vector3Settings` three-column squeeze beyond the popup-width fix.
- `qml/qmldir` kept: potentially load-bearing for source-directory loads.
- Base-input undo semantics (per-keystroke entries, 100-cap) — controller undo
  now refreshes the editor correctly; coalescing is a design change.
