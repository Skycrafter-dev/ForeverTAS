# Warm release assembly

Linux and Windows releases retain one package per supported hardware flavor and
the existing updater asset names. A NVIDIA package is assembled from its own
SM-specific application and debug-worker executables plus one SM75 Qt/runtime
deployment. The universal and AMD packages continue to deploy their own
runtimes. The release drivers build the NVIDIA template first, then assemble
the remaining NVIDIA packages from it. Linux runs independent builds in parallel
(`FOREVERTAS_RELEASE_VARIANT_JOBS`, default 4); CMake/Ninja build directories and
the shared sccache directory remain warm between runs. No fat CUDA/HIP binary or
new runtime dispatch path is introduced.
`FOREVERTAS_RELEASE_CACHE` defaults to a sibling directory outside worktrees,
so compiler and packaging-tool caches survive a new isolated checkout.

The release driver builds the template from the same clean ForeverTAS commit and
pinned ForeverValidator commit as its variant executables. It checks each
variant's embedded CUDA architecture, runs the packaged startup smoke test,
and writes per-platform source records. `prepare` checks all 38 package hashes
and source records, then emits `build-provenance.json`; `draft` and `publish`
require its source commit to match the release tag. `SOURCE_DATE_EPOCH` is set
from the source commit for Linux packages.

## Warm measurements (2026-09-26)

All measurements used existing warm build outputs and no GPU recompilation.

| Operation | Before | After |
| --- | ---: | ---: |
| One NVIDIA AppImage, warm build plus old packaging vs template assembly and smoke | 44.34 s (SM75) | 20.62 s (SM75) |
| Full NVIDIA matrix, template assembly and smoke, four concurrent jobs | not measured warm | 183.84 s for 17 |
| QML-only change, warm SM75 rebuild | 32.35 s | GPU objects unchanged |

The original release's 19 Linux artifact timestamps span 41 minutes 24 seconds,
but that included first-time compilation and is **not** a comparable warm
baseline. The matrix timing excludes building the three template/root packages.

All 17 pre-existing NVIDIA AppDirs were byte-identical outside their two
executables. The assembled matrix passed all 17 packaged QML smoke tests and
Linux asset verification for all 19 flavors. An SM50 package assembled twice
with the same `SOURCE_DATE_EPOCH` had identical SHA-256 hashes. The QML-only
SM75 rebuild scheduled three resource/link steps and no CUDA or HIP compile.

Windows staging follows the same two-executable overlay, but its full installer
run still requires verification on the Windows builder before it can be used
for a release. This branch must not be merged or published on that evidence
alone.
