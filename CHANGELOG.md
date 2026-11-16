# Changelog

## 0.3.1

### New and improved

- **Searching on other computers:** Edit > Preferences > Remote. Your own
  machines (a Google Colab or Kaggle notebook, a rented GPU, another computer)
  join with a code and search for every search started here, beside this
  computer: **Open in Colab** opens the notebook, which asks only for the code
  and your Packs folder. Every search feature works remotely, exact and
  approximate, on their GPUs and CPU threads. See docs/REMOTE.md.
- **ForeverTAS@home:** lend your computer to a group, or search with the
  computers its members lend. Make a group and share its invite, or join the
  public pool (anyone lends, anyone searches). Lent computers search in a
  sandbox, pause while you search on your own GPU, and every best they find
  is checked here before it counts. One machine can lend to several groups
  and the public pool at once, one search at a time.
- **This computer** in the Remote tab: its graphics cards, its processor and
  thread count, and what it lends (its GPU, how many CPU threads).
- On a CPU backend, this computer's CPU searches beside the remote machines.

## 0.3.0

### New and improved

- **Approximate physics:** Search > Physics: Approximate searches on a fast
  approximation of the game's physics, many times more attempts a second, and
  rebuilds every new best on the exact physics before it counts. On CUDA, HIP
  and the multi-threaded CPU, for every target and condition.
- **Faster promoted approximate searches on the GPU:** a promotion no longer
  throws away the batch the GPU already computed: about 11% more attempts a
  second in a promoting search (RTX 5060, 0.90M to 1.01M), same results run
  to run.
- **In-game look:** Graphics > Render > In-game draws the race with the
  game's own textures, lighting and car, with ForeverTAS's targets and
  trajectories on top. File > Export to MP4 records the race that way.
- **New targets:** Distance to checkpoint/finish (straight, or around walls:
  it goes for the part of the trigger the car can reach), velocity towards a
  point, fastest entry into a box or shape, and pose targets with a weight per
  axis.
- **Gates target:** an ordered list of gates (position, heading, and limits
  across, up, on speed, heading and wheels on the ground): the car must cross
  each earlier one honestly and in order, and the search lowers the time it
  crosses the last. Misses can be ranked by how close they came, so a search
  climbs towards a gate nothing reaches yet. CPU backends with exact physics.
  See docs/GATES.md. (From a contribution by thirkle.)
- **Window chains:** link the time windows of passes and targets with a
  color; changing one moves the others, as in TICK.
- **Driving:** any driving key or the gamepad takes over the race that
  plays, from where it is. Each takeover is an attempt (Attempts panel) you
  can watch again or keep as the base inputs (Undo keep goes back). Pause,
  speed, rewind, single ticks and savestates (F1-F12, Shift to save) work
  while driving.
- **Game cameras:** keys 1, 2 and 3 are the car's camera slots as the game
  has them (behind, close, inside).
- **Edit > Preferences:** every race key and gamepad button can be changed;
  the gamepad's device and steering deadzone; MCP. The first launch picks the
  race keys for your keyboard layout (AZERTY too).
- **Go to:** type where to go and press Enter: a time (30, 1:02.35, 1500ms),
  from where you are (+2, -0.5), a checkpoint (cp 3, next, prev, finish), a
  share of the race (50%) or a tick (tick 120). In the Playback panel when it
  is wide enough, and from the race view with Ctrl+G. Bindable in Controls:
  Go to..., Go to again (the last place typed: type +5 once for a five-second
  skip key), Next checkpoint (Page Down) and Previous checkpoint (Page Up).
- **MCP (AI assistants):** Claude or any MCP client can drive ForeverTAS: load
  maps, test input scripts headlessly (single runs, batches, snapshots), set
  up and run searches and read the results, all visible in the window. Off
  until turned on in Edit > Preferences > MCP, which shows the client
  configuration to copy; with ForeverTAS closed, a client opens it. See
  docs/MCP.md. (From a contribution by thirkle.)
- **Base inputs:** TMInterface time ranges: `0.50-1.20 press up` presses at
  0.50 and releases at 1.20.
- **Inputs timeline:** across or down, with a cursor you drag (it keeps
  going past the window's edge, held like a game holds the pointer).
- **New look:** every panel is now a sheet of glass over the race view, which
  fills the whole window. Light and dark mode are gone; there is one look.
  View > Glass look... adjusts how dark, blurred and bent the glass is.
- **Panels you arrange yourself:** Inputs, Graphics, Bruteforce, Code, the race
  viewer header, Camera, Scripted telemetry, Splits, Playback, Manual driving,
  Whiteboard and Drawings are all panels. Drag a panel by its title to move it.
  While dragging, drop targets appear: drop on a panel's side to split its
  space, on its middle to stack the two as tabs, or on a window edge to dock
  along that edge. Drop anywhere else and the panel floats; floating panels
  stick to each other's edges and never leave the window. Drag the gaps between
  docked panels to resize them, or the edges of a floating panel.
- **Closable panels:** every panel has a close button and comes back in the same
  place from the View menu. Splits, Scripted telemetry and Manual driving still
  appear only when they have something to show.
- **Panel headers** can be hidden: click a panel's top edge, or View >
  Panel headers. Base, Target, Passes, Search and Results are panels of their
  own.
- **Menu bar:** File: Open map or replay (loads it right away), Extract
  inputs from replay, Export to MP4, Set packs folder. Edit: Undo, Redo, Cut,
  Copy, Paste, Select all, Preferences. View: the panels, Panel headers, Hide
  all panels (Tab), Show viewer, Full screen (F11), zoom (Ctrl++ / Ctrl+- /
  Ctrl+0), Glass look and Reset layout. Help: Check the car, Open log folder.
- The panel layout, zoom, glass look, window size and full screen are saved
  between sessions.
- **Faster GPU setup:** batch calibration takes under ten seconds and
  matches tuning by hand. Stop ends a search at once. History and Current best
  show every decimal.
- **One download per system:** a single ForeverTAS for Linux and one for
  Windows, for every GPU. ForeverTAS downloads the GPU support for your
  graphics card by itself (8 to 15 MB, checked before use), and again when you
  change cards. Earlier versions update into it as usual.
- **Two graphics cards at once:** with an NVIDIA and an AMD card, the backend
  list shows **GPU** with a box per card. Tick one or both: ticked cards search
  together, each its own attempts, and share their best runs. Each card keeps
  its own batch settings. ForeverTAS downloads the GPU support for both.
- **Logs:** each session writes a log, with a crash report if ForeverTAS
  crashes or freezes. Help > Open log folder; please send it with bug
  reports.

### Fixes

- **Plain error messages:** when a search can't start, the message says where
  (Search, Target, or Pass 2 with its kind) and what, in plain words; a setting
  that can't be read is named as the window shows it, with its value and what
  it takes ("Minimum steering insertions" can't be "3.5": it takes a whole
  number, 0 or more), instead of "input insertion settings are invalid".
- Editing the base inputs (typing, pasting, Copy current race, Keep) now
  shows the base inputs' run at the same moment, whatever you were watching:
  an attempt, the best, or a drive (which ends, its attempt kept). Before, the
  edit changed nothing on screen until you picked the run yourself.
- Number fields store what they show: a number typed out of a field's range
  or with decimals where whole numbers go was kept as typed (and refused by
  the search, "settings are invalid") while the field showed it corrected. A
  value saved that way now shows as it is stored.
- **The vanishing car:** a car (the best, a drive after a takeover, any
  car) could stop being drawn while everything else showed, until another
  run was chosen. Qt Quick 3D gives a car shape that comes back into the view
  (no car of that color was shown for a while: a search's best cleared, a
  drive ended) an empty copy and never fills it. ForeverTAS now fills it
  again. The same held for every 3D shape it draws (trajectories, targets).
- **Gamepad:** a connected gamepad now drives and can be bound straight away
  (Preferences > Gamepad, Device: Automatic, the first connected), where before
  nothing happened until one was picked by hand. The tab shows the last button
  pressed and names controllers seen without a gamepad layout; the log lists
  every controller and which one is in use. A chosen gamepad plugged into
  another port is found again.
- Pressing Enter in some settings froze or closed ForeverTAS.
- A hidden panel header could not be brought back.
- Box, pose and other gizmos could not be dragged in the race view; drags
  are lighter.
- Pose targets: copy the position and the rotation separately, from the
  camera or the car; copying the camera's rotation gave a flipped car.
- Scripted telemetry can be edited again (pencil button).
- `car.localvel` read 0 on every backend.
- Windows: GPU searches could crash when starting. AMD: an RX 7600 XT had
  every batch size refused; Linux AMD searches failed with "invalid argument".
- The best of an autorestart was shown in the next cycle; entry dot, camera
  buttons and long lists fixed; memory is given back after loading a map.

### Removed

- **CUDA fast mode** and the **Vulkan** physics backend. CUDA, HIP and the CPU
  backends remain. A search saved with Vulkan selected opens on Reference.

## 0.2.7

### Fixes

- Session rows show finish times in seconds under "Finish time", including
  previously saved sessions.
- Autorestart continues after cycles where no iteration meets the conditions.
  Stop and Abort still end the search; unsuccessful cycles do not save a fake result.
- Fixed CUDA/HIP Tree search and Adaptive escalation sometimes stopping with
  "sandbox input timeline is invalid" after finding improvements. Inputs just
  beyond the simulation horizon now stay in the correct order when the best
  result becomes the new baseline. CPU simulation and scoring are unchanged.

## 0.2.6

### Fixes

- Removed ForeverTAS's GPU step time limit. Long GPU steps no longer stop a search or prevent a batch from running just because they take too long, in both manual and calibrated searches. Memory checks and the Stop button still work. System and graphics-driver timeouts are unchanged.

## 0.2.5

### New and improved

- **Tree search (CPU):** a new search that improves your run with small, local changes. By default each attempt changes one of four segments of the mutation window and keeps the rest at the current best, and every CPU worker follows its own improvements for five seconds before adopting the shared best ("islands"). In our tests it beat basic bruteforce in 48 of 56 paired runs, and with islands it escaped a slower outcome every time where basic bruteforce never did. Change the segments per attempt, what the other segments do, and the island migration time in the search settings; advanced options cover leaves per tree and mixing in basic bruteforce workers.
- **Adaptive escalation (CPU):** a new search that starts with one changed segment per attempt and doubles the number of changed segments each time it stalls, returning to one after any improvement.
- **Tree search and adaptive escalation on CUDA/HIP:** each GPU candidate changes randomly selected segments and keeps the remaining controls at the current best. Adaptive escalation doubles the changed-segment count after stalled batches and resets it after an improvement. CPU prefix sharing, islands, and other CPU search behavior are unchanged.
- **More GPU targets:** custom polygon-prism volume entry targets now run on CUDA/HIP, and checkpoint time targets are enabled on HIP.
- **Faster finish and entry searches:** attempts that can no longer beat the current best finish or volume-entry time now stop early on CPU backends, with identical results.

## 0.2.4

Changes since **0.2.2**.

### New and improved

- **More ways to search on your GPU:** Vulkan and HIP join CUDA, with dedicated NVIDIA builds and AMD RX 7000/9000 packages.
- **Simpler installation and updates:** a Windows installer, in-app updates on Windows and Linux, automatic GPU-package recommendations, and package switching without losing settings.
- **Saved search sessions:** automatically restart after a duration or attempt count, save each cycle, and browse, sort, preview, or copy previous results. Best-run previews belong to the current session.
- **Custom objectives:** write expressions to minimize, maximize, or reach a chosen value; combine several goals in one search on CPU or GPU.
- **Reorganized search controls:** dedicated Base, Target, Passes, and Search tabs, visual target selection, and grouped controls for adding, reordering, and deleting input passes.
- **Edit time ranges in the 3D view:** drag input-pass and evaluation windows, the simulation horizon, and stunt deadlines directly along the trajectory. Capture the current timeline time into a field, or select the full run or the remaining run in one click.
- **Easier target placement:** pick a point directly in the scene or copy the car/camera position. Set velocity direction from the car's travel, heading, or an axis, with a visible direction arrow and alignment cone.
- **See what the search evaluates:** visual markers and live measurements for targets, entry/finish events, stunt scores, and satisfied conditions. Telemetry text now supports calculations and these evaluation fields.
- **Control the viewer's appearance:** a Runs/Overlays/Targets panel with visibility, colors, opacity, line widths, dashed lines, and draw-through options for individual items.
- **Undo and redo across the editor:** search settings, input passes, target edits, drawings, telemetry, and appearance changes, not just the base input script.

### Fixes from 0.2.2

- Corrected yaw, pitch, and roll in pose targets and condition scripts so they match the car's orientation in the viewer.
- Camera shortcuts **1, 2, 3, and 7** work after clicking the race preview, rather than requiring focus on the camera controls.
- Fixed CUDA compatibility on older NVIDIA GPUs. Regular CUDA supports compute capability 5.0+; unavailable Fast CUDA automatically falls back to regular CUDA.
- Loading maps and viewing trajectories no longer depend on the selected GPU search backend being available.
- Made CPU input-mutation sampling consistent between Windows and Linux for reproducible seeded searches.
