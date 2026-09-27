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
Concurrent AppImage compression jobs receive disjoint CPU sets, preventing each
`mksquashfs` process from spawning threads across the whole runner.
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
| Full 19-flavor Linux release driver, routine warm build, package, smoke, verify | 741.09 s | 274.24 s |

The original release's 19 Linux artifact timestamps span 41 minutes 24 seconds,
but that included first-time compilation and is **not** a comparable warm
baseline. Instead, the original driver was run twice in a fresh isolated
worktree seeded from its 19 warm build directories: 829.40 s for the first
rehydrated run, then 741.09 s for the routine warm run. The redesigned driver
likewise ran twice with warm directories: 312.78 s before CPU partitioning and
274.24 s with partitioning. Both routine runs used the same host, pinned
toolchain images, four Ninja jobs, and all 19 package flavors; neither invoked
a GPU compiler. The redesigned routine run was 2.70x faster. The NVIDIA
matrix-only timing excludes building the three template/root packages, while
both full-driver times include all 19 packages and startup smoke tests.

All 17 pre-existing NVIDIA AppDirs were byte-identical outside their two
executables. The assembled matrix passed all 17 packaged QML smoke tests and
Linux asset verification for all 19 flavors. An SM50 package assembled twice
with the same `SOURCE_DATE_EPOCH` had identical SHA-256 hashes. The QML-only
SM75 rebuild changed only the module; executable and CUDA/HIP object hashes
were identical before and after. Packaged universal, SM50, SM75, and AMD
AppImages passed QML smoke tests with the dynamic module. The module installed
in all 19 AppDirs has the same SHA-256 hash. Across all NVIDIA and AMD warm
builds, 420 CUDA/HIP object files were byte-identical to the original release.
On the local RTX 5060, the SM120 CUDA search parity/performance test passed
before and after. Its test executable and both hot-path core archives were
byte-identical, so this packaging change adds no compute dispatch or device
code to the measured search path.

Windows staging follows the same two-executable overlay. It builds the QML
module once in its universal root; SM75, AMD, and all other NVIDIA flavors
consume that module. `windeployqt` scans QML sources to deploy Qt imports
because the application executable no longer owns a QML module. A clean,
pinned previous Windows build can be seeded into a new isolated checkout with
`seed-windows-warm-builds.ps1`; the script copies and relocates CMake caches,
compares source content before restoring timestamps, and records the SHA-256
of each copied Validator static archive. The release driver rechecks those
hashes and the pinned Validator and toolchain manifest before importing the
archives, so moving worktrees or changing UI source does not recompile GPU
code. A changed Validator checkout cannot pass the pin check and must build
its compute component normally. The unchanged QML and Qt runtime are deployed
once in the SM75 template. Other NVIDIA ZIPs copy that archive's already
compressed entries and replace only their two executables; they are byte
reproducible under the same inputs. The SM75 installer staging tree is kept
for the release run; variant installers copy its verified common runtime and
replace the two executables instead of decompressing the ZIP again. The ZIP
overlay's executable hashes are checked against those same build outputs.
Independent NVIDIA variants run as bounded
PowerShell jobs (`FOREVERTAS_RELEASE_VARIANT_JOBS`, default 6). Each ZIP is
smoke-tested and each executable's embedded CUDA architecture is inspected
before its Inno Setup installer is accepted. Installer compression uses the
faster LZMA2 preset, trading some artifact size for release latency without
changing installation or updater asset identities. The portable startup check
and installer compilation consume one extracted ZIP tree rather than unpacking
the same runtime twice.
