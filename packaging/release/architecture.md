# Warm release assembly

Linux and Windows releases retain one package per supported hardware flavor and
the existing updater asset names. The QML interface, visual resources, and
shaders live in a dynamically loaded module instead of each executable. Linux
builds that module once in the universal flavor and installs the identical
module into NVIDIA and AMD packages. A NVIDIA package is assembled from its own
SM-specific application and debug-worker executables plus one SM75 Qt/runtime
deployment. The release drivers build the universal flavor first, then the
NVIDIA template and AMD flavor, then assemble the remaining NVIDIA packages.
Linux runs independent builds in parallel
(`FOREVERTAS_RELEASE_VARIANT_JOBS`, default 4); CMake/Ninja build directories and
the shared sccache directory remain warm between runs. No fat CUDA/HIP binary or
new runtime dispatch path is introduced.
`FOREVERTAS_RELEASE_CACHE` defaults to a sibling directory outside worktrees,
so compiler and packaging-tool caches survive a new isolated checkout.
For a fresh checkout, set `FOREVERTAS_WARM_BUILD_SOURCE` and
`FOREVERTAS_WARM_VALIDATOR_SOURCE` to clean prior worktrees. The driver clones
the requested warm CMake build directories without writing to either donor,
checks the pinned toolchain, and restores timestamps only for tracked source
files whose contents are byte-identical. Changed files retain a fresh mtime,
so Ninja rebuilds their dependents. It refuses missing warm build directories
unless `FOREVERTAS_ALLOW_COLD_BUILD=1` is deliberately set.

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
| QML-only change, warm SM75 build | 32.35 s executable relink | 4.45 s shared module; SM50 executable had no work (1.06 s check) |

The original release's 19 Linux artifact timestamps span 41 minutes 24 seconds,
but that included first-time compilation and is **not** a comparable warm
baseline. The matrix timing excludes building the three template/root packages.

All 17 pre-existing NVIDIA AppDirs were byte-identical outside their two
executables. The assembled matrix passed all 17 packaged QML smoke tests and
Linux asset verification for all 19 flavors. An SM50 package assembled twice
with the same `SOURCE_DATE_EPOCH` had identical SHA-256 hashes. The QML-only
SM75 rebuild changed only the module; executable and CUDA/HIP object hashes
were identical before and after. Packaged universal, SM50, SM75, and AMD
AppImages passed QML smoke tests with the dynamic module. The module installed
in universal and SM75 AppDirs had the same SHA-256 hash.

Windows staging follows the same two-executable overlay. It builds the QML
module in its universal, SM75, and AMD roots; other NVIDIA flavors consume
the SM75 module. Its full installer run still requires verification on the
Windows builder before it can be used for a release. This branch must not be
merged or published on that evidence alone.
