# GPU backends

CUDA (NVIDIA) and HIP (AMD, or NVIDIA through HIP) are independent build
options and selectable engines in one application. CPU modes remain available.
ForeverValidator's Vulkan backend is not built into ForeverTAS.

## Combined developer build

With a local ForeverValidator checkout:

```sh
cmake -S . -B build/gpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFOREVERTAS_ENABLE_CUDA=ON \
  -DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=/path/to/ForeverValidator
cmake --build build/gpu
```

CUDA needs its toolkit; HIP needs ROCm (or HIP on NVIDIA). Set either option to
`OFF` independently, or both to `OFF` for a CPU-only build.

## Runtime selection

Choose **CUDA** or **HIP** in the physics-backend selector. Each has separate
availability diagnostics, saved batch size and throughput-calibration settings.
An unavailable engine displays its own diagnostic and does not silently run the
search on another backend. GPU search winners are sampled with the existing
reference path for presentation.

The shared experimental search API retains legacy `Cuda`-named types and fields
for source compatibility. Dispatch is determined by the selected sandbox backend,
not by those names. CUDA retains its register, occupancy and memory safety
checks; HIP leaves launch validation to its runtime.

## Verification

Enable `BUILD_TESTING` and supply `FOREVERTAS_TEST_PACK_DIRECTORY` and
`FOREVERTAS_TEST_REPLAY` to register the resident-search parity suites. Hardware
availability is required to run GPU parity tests; successful compilation alone
is not device certification.

## Packaging

The release recipes keep CUDA runtime packaging and architecture verification.
Vendor GPU drivers are not bundled. The release manifest pins the same
Validator revision as CMake and invalidates CUDA search-object caches when that
revision's shared physics headers change.

Release builders must validate the final bundle on each supported OS and GPU
and include the applicable library notices before publication.

A prebuilt CUDA search object must match all shared CUDA sources and headers.
Clear `FOREVERVALIDATOR_CUDA_SEARCH_PREBUILT_OBJECT` or regenerate the object
after changing them; an old object must not bypass rebuilding changed code.
