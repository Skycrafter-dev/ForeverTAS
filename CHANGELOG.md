# Changelog

## 0.2.4

Changes since 0.2.2, including the unreleased 0.2.3 improvements.

### New and improved

- **Install once, update in the app.** Windows installers and Linux AppImages can download updates and recommend the right compute package for your GPU. Packages can be switched without losing settings.
- **More GPU choices.** Added Vulkan and HIP search, dedicated NVIDIA builds, and AMD RX 7000/9000 packages. Regular CUDA now supports older GPUs starting with compute capability 5.0; Fast CUDA requires 7.5 and falls back automatically on older cards.
- **Automatic search restarts and saved sessions.** Restart after a chosen duration or number of attempts, then browse, sort, preview, and copy saved results.
- **Custom search objectives.** Combine expressions to minimize, maximize, or match desired values on CPU and supported GPU backends.
- **Direct trajectory editing.** Drag input-pass windows, evaluation windows, the simulation horizon, and stunt deadlines in the viewer, with visual feedback for evaluation results.
- **Viewer appearance controls.** Manage runs, overlays, and targets with per-item visibility, color, opacity, line width, and draw-through settings.
- **Broader undo and redo.** Undo changes to search settings, targets, drawings, and viewer appearance.
- **Simpler input-pass management.** Stable Base, Target, Passes, and Search tabs keep navigation predictable; add, reorder, and delete controls stay together. Removed redundant target labels.

### Fixes

- Fixed multi-second freezes when orbiting the 3D view by avoiding repeated trajectory and telemetry rebuilds for every mouse event.
- Camera shortcuts **1, 2, 3, and 7** now work after clicking the race preview.
- Starting a new search session clears the previous session's Best run and improvement previews automatically. The Clear previews button is no longer needed.
- Stopping a search no longer replaces the Inputs preview automatically. Base inputs stay unchanged unless explicitly edited or replaced.
- Targets and time-window handles remain visible and editable when the search horizon is too short or a window has zero length.
- Corrected pose-target yaw, pitch, and roll to match the viewer's coordinate system.
- Search previews no longer depend on the chosen GPU backend; improved results are checked on CPU before being accepted.
- GPU calibration is bounded so it cannot consume the whole configured search duration. Fixed CPU input-mutation sampling differences between Windows and Linux.
- Fixed packaged update connections and leftover GPU libraries when switching Windows compute packages. The viewer can run without an NVIDIA driver.
