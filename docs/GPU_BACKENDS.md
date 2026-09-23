# GPU backends

CUDA and Vulkan are independent build options and selectable engines in one
application. CPU modes remain available. There is no HIP implementation yet.

## Combined developer build

Use the matching local ForeverValidator checkout until the pinned Validator
commit has been published:

```sh
cmake -S . -B build/gpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFOREVERTAS_ENABLE_CUDA=ON \
  -DFOREVERTAS_ENABLE_VULKAN=ON \
  -DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=/path/to/ForeverValidator
cmake --build build/gpu
```

CUDA needs its toolkit; Vulkan needs Vulkan development headers and the loader.
Set either option to `OFF` independently, or both to `OFF` for a CPU-only build.
The Vulkan shader binaries are embedded by ForeverValidator; Slang is not
required for an ordinary application build.

## Runtime selection

Choose **CUDA** or **Vulkan** in the physics-backend selector. Each has separate
availability diagnostics, saved batch size and throughput-calibration settings.
The CUDA fast-mode preference remains CUDA-only and is not changed by selecting
Vulkan. An unavailable engine displays its own diagnostic and does not silently
run the search on another backend. GPU search winners are sampled with the
existing reference path for presentation.

The shared experimental search API retains legacy `Cuda`-named types and fields
for source compatibility. Dispatch is determined by the selected sandbox backend,
not by those names. Vulkan calibration never queries CUDA device information;
CUDA retains its existing register, occupancy and memory safety checks.

## Verification

Enable `BUILD_TESTING` and supply `FOREVERTAS_TEST_PACK_DIRECTORY` and
`FOREVERTAS_TEST_REPLAY` to register both resident-search parity suites. Tests
cover explicit routing, independent preferences, QML controls, search winners,
precise finish times and calibration. Hardware availability is required to run
GPU parity tests; successful compilation alone is not device certification.

## Packaging

The release recipes enable both engines while retaining CUDA runtime packaging
and architecture verification. Linux build dependencies include `libvulkan-dev`;
the AppImage explicitly deploys the Vulkan loader. Windows dependencies include
vcpkg's `vulkan` package, and installation copies `vulkan-1.dll` alongside the
application. Vendor GPU drivers are not bundled. The release manifest pins the
same Validator revision as CMake and invalidates CUDA search-object caches when
that revision's shared physics headers change.

Release builders must validate the final bundle on each supported OS and GPU
and include the applicable loader/library notices before publication.

A prebuilt CUDA search object must match all shared CUDA sources and headers.
Clear `FOREVERVALIDATOR_CUDA_SEARCH_PREBUILT_OBJECT` or regenerate the object
after changing them; an old object must not bypass rebuilding changed code.
