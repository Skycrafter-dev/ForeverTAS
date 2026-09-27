# Changelog

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
