# Changelog

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
