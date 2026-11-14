# Making a release

```sh
# once per release: the version in CMakeLists.txt, CHANGELOG.md, commit, tag
packaging/release/release.py build      # everything that changed, Linux and Windows
packaging/release/release.py publish    # GitHub, as a draft (asks first)
```

`build` checks what it built before it says it is done (`release.py check`
runs the checks alone). Options: `--os linux` or `--os windows`, `--parts
app,cuda,hip` (any of them), `--packs DIR --replay FILE` (also run a search on
this computer's GPU with its module, and compare with the CPU), `--validator
PATH` (a ForeverValidator repository with the pinned commit; default: next to
this one), `--without-render-module` (a test build without In-game; `publish`
refuses it).

## The In-game render mode

Every release ships it: a private module (ForeverViewer's renderer) beside the
app, never in this repository. `release.py` builds it from its own repository,
at the commit this computer's `packaging/release/release.local.json` pins
(untracked):

```json
{"render_module": {"repository": "~/path/to/ForeverViewer", "commit": "<full commit>"}}
```

It is built once per pinned commit (`release.py render-module` builds it
alone), into the release cache (`render-module/`), each system in its own
image: Linux in `render-module-linux.Dockerfile` (the app's Ubuntu 22.04, so
no newer glibc; the module brings the libraries the AppImage does not have),
Windows in `render-module-windows.Dockerfile` (MinGW GCC 13). Its build also
writes the licenses of everything it includes (`licenses/`); the app installs
them with its own, under `licenses/render-module`. A computer without Docker
or Podman (the Windows half run elsewhere) takes the module from its cache:
copy `render-module/` from the computer that built it. `check` has the
shipped app load it (`--check-render-module`) in the same clean systems as
the GPU modules, and looks for its licenses.

## What gets built

| | Linux (container, `linux.Dockerfile`) | Windows (builder VM, over ssh) |
| --- | --- | --- |
| app | AppImage | Inno Setup installer |
| CUDA | 12 modules (`cuda-sm50` ... `cuda-sm120`, PTX in 120) | the same |
| HIP | 1 module (gfx1100-gfx1201) | the same |

The app has no GPU code. A module is the search core built with CUDA or HIP
(`FOREVERTAS_COMPUTE_MODULE=cuda|hip`, src/compute); the app loads it at run
time (src/app/compute_modules.cpp). See [UPDATES.md](UPDATES.md) for how users
get them.

## Why it is fast

- **The GPU code is compiled once for every generation.** One CUDA build
  (`CMAKE_CUDA_ARCHITECTURES` with all 12, `nvcc --threads 0`), then each
  module is that build relinked with its archives pruned to one generation
  (`nvprune`, `compute_modules.py`): seconds per module, no compiling.
- **Nothing is compiled twice.** The build folders stay between releases
  (`~/.cache/forevertas-release`, `C:/forevertas-release` on the builder),
  sources are synced into them by content (an unchanged file keeps its time,
  so ninja rebuilds only what changed), and ccache (Linux) / sccache (Windows)
  keep every object, CUDA's included. A release that changes no GPU code
  compiles no GPU code: the modules are relinked from cached objects.
- **Unchanged modules are not uploaded.** A module's file name is the hash of
  its contents. `publish` uploads only modules the `compute-modules` release
  does not have yet.
- Linux and Windows build at the same time.
- **It fits the computer's memory.** ptxas takes up to ~3.7 GB per
  generation on the twin's kernels: nvcc compiles as many generations side by
  side as there are 5 GB of memory (all cores when it can), one CUDA file at
  a time (`cmake/compile_slots.py`), while C++ files build next to it.
  Override with `-DFOREVERTAS_CUDA_GENERATIONS_AT_ONCE=` /
  `-DFOREVERTAS_CUDA_FILES_AT_ONCE=` (on Linux, changing it costs no
  rebuild: ccache ignores it).

Measured on the lab (6 cores; Linux 24 GB, Windows VM 16 GB, both at once):

| Release | Linux | Windows |
| --- | --- | --- |
| nothing changed | 2 min | 2.5 min |
| the app changed | 3 min | 3 min |
| the GPU search code changed | 17 min | 23 min |

A build with empty caches compiles every CUDA file for 12 generations, once.
Unchanged modules keep their file names (checked: two builds of the same
sources give the same 26 names), so a release that changes no GPU code
uploads no module.

## What is checked

- every file against the hash in its listing;
- every module of a platform carries its app's key (they share code);
- every module unpacked and loaded **by the shipped app**: Linux, the AppImage
  in a clean Ubuntu 22.04 (`linux-check.Dockerfile`: what a desktop has, no
  CUDA, no ROCm); Windows, the installed app with only Windows on `PATH`.
  A module missing a library fails here, not on a user's computer;
- every CUDA module carries exactly its generation (and PTX only in 120);
- the HIP module carries device code for every AMD target
  (`verify_hip_targets.py`);
- the app starts without a GPU driver (Linux: the AppImage's own checks;
  Windows: `--qml-smoke-test` with only Windows on `PATH`);
- with `--packs/--replay`: a search on this computer's GPU through its module
  is the same as the same search on Optimized CPU, attempt for attempt;
- the Linux app builds with the Wayland pointer lock.

The builders have no AMD GPU: on a computer with one, the same check is

```sh
ForeverTAS-*.AppImage --appimage-extract
squashfs-root/AppRun --check-compute-module forevertas-compute-linux-hip-*.tar.zst PACKS REPLAY
```

(exit 0: the search on that GPU is the same as on Optimized CPU).

## What it needs

- Linux: Docker or Podman, git, rsync, Python 3. The toolchain image
  (`linux.Dockerfile`: Ubuntu 22.04, Qt 6.9.3, CUDA 12.8, ROCm 7.2.4, ccache) is
  built on first use and again whenever the file changes. About 30 GB free
  for the image, build folders and compiler cache. (Podman: `--userns=keep-id`
  may copy the image once more.)
- Windows builder (`release.json`: `windows.host`, ssh with a key, PowerShell):
  Visual Studio 2022 Build Tools, LLVM (clang-cl), Qt 6.11.1 msvc2022_64, the
  HIP SDK, Inno Setup 6, Python 3 with `zstandard` and `cmake` (CMake 4: Visual Studio's
  CMake 3.31 compiles HIP files as C++), sccache, git. CUDA 12.8 and
  vcpkg (at `windows.vcpkg_commit`) are set up by the scripts themselves.

## Files

- `release.py`: the driver (build, check, publish).
- `release.json`: repository, GPU generations, AMD targets, Windows builder.
- `build-linux.sh`, `linux.Dockerfile`, `linux-check.Dockerfile`: Linux.
- `build-windows.ps1`, `ensure-windows-cuda.ps1`: Windows.
- `compute_modules.py`: modules from a GPU build (split, bundle, name).
- `verify_hip_targets.py`: HIP device code per target.
