#!/usr/bin/env bash
# The Linux half of a release, inside the toolchain image (linux.Dockerfile);
# release.py runs it. Mounts: /src (ForeverTAS), /validator (ForeverValidator
# at its pin), /build (build folders, kept between releases), /cache (ccache,
# AppImage tools), /out (results). Builds only what changed since the last
# release: the folders and the compiler cache persist.
#
# Environment: CUDA_SM (e.g. 50,52,...,120), CUDA_PTX (120), HIP_GFX
# (gfx1100;...), PARTS (any of: app cuda hip), JOBS.
set -euo pipefail

parts=" ${PARTS:-app cuda hip} "
jobs="${JOBS:-$(nproc)}"
out=/out/linux
mkdir -p "${out}/compute"
export CCACHE_DIR=/cache/ccache CCACHE_BASEDIR=/ CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}" CCACHE_COMPRESS=1
# the same file at the same path in every release container: hits across releases
export CCACHE_NOHASHDIR=1
# how many generations nvcc compiles side by side changes nothing it makes
export CCACHE_IGNOREOPTIONS="--threads=*"

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release
        -DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=/validator
        -DFOREVERTAS_ENABLE_TWIN=ON
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)

step() { printf '\n=== %s (%s)\n' "$1" "$(date +%T)"; }

if [[ "${parts}" == *" app "* ]]; then
    step "app"
    # FOREVERTAS_EXTRA_LICENSES: those of the libraries release.py ships beside the app
    cmake -S /src -B /build/app "${common[@]}" -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr \
        "-DFOREVERTAS_EXTRA_LICENSE_FILES=${FOREVERTAS_EXTRA_LICENSES:-}" >/dev/null
    cmake --build /build/app --parallel "${jobs}"
    # what releases must have, not just what this image happens to allow
    grep -q "FOREVERTAS_WAYLAND_POINTER_LOCK=1" /build/app/build.ninja ||
        { echo "the release app has no Wayland pointer lock" >&2; exit 1; }
    rm -f "${out}"/ForeverTAS-*.AppImage
    # linuxdeploy finds the app's Qt libraries through the loader path
    LD_LIBRARY_PATH="${QT_ROOT_DIR}/lib" \
    FOREVERTAS_SKIP_BUILD=1 FOREVERTAS_BUILD_DIR=/build/app FOREVERTAS_DIST_DIR="${out}" \
    FOREVERTAS_TOOLS_DIR=/cache/appimage-tools FOREVERTAS_RELEASE_JOBS="${jobs}" \
        bash /src/packaging/linux/build-appimage.sh
fi

if [[ "${parts}" == *" cuda "* ]]; then
    step "CUDA module (sm ${CUDA_SM}, PTX ${CUDA_PTX})"
    architectures="$(echo "${CUDA_SM}" | sed 's/,/-real;/g')-real;${CUDA_PTX}-virtual"
    cmake -S /src -B /build/cuda "${common[@]}" -DFOREVERTAS_COMPUTE_MODULE=cuda \
        "-DCMAKE_CUDA_ARCHITECTURES=${architectures}" \
        -DCMAKE_CUDA_COMPILER_LAUNCHER=ccache >/dev/null
    cmake --build /build/cuda --parallel "${jobs}"
    rm -rf "${out}/compute/cuda" && mkdir -p "${out}/compute/cuda"
    # the CUDA EULA (cudart's package carries it), shipped with cudart
    cp /usr/share/doc/cuda-cudart-12-8/copyright /tmp/CUDA-EULA.txt
    python3 /src/packaging/release/compute_modules.py pack --build /build/cuda \
        --backend cuda --os linux --sm "${CUDA_SM}" --ptx "${CUDA_PTX}" \
        --runtime "${CUDA_PATH}/lib64/libcudart.so.12" \
        --runtime /tmp/CUDA-EULA.txt \
        --out "${out}/compute/cuda" --jobs "${jobs}"
fi

if [[ "${parts}" == *" hip "* ]]; then
    step "HIP module (${HIP_GFX})"
    cmake -S /src -B /build/hip "${common[@]}" -DFOREVERTAS_COMPUTE_MODULE=hip \
        -DCMAKE_HIP_PLATFORM=amd "-DCMAKE_HIP_ARCHITECTURES=${HIP_GFX}" \
        -DCMAKE_HIP_COMPILER_LAUNCHER=ccache >/dev/null
    cmake --build /build/hip --parallel "${jobs}"
    module=/build/hip/forevertas-compute-hip.so
    # every target's device code is really there
    python3 /src/packaging/release/verify_hip_targets.py "${module}" --expect "${HIP_GFX//;/,}"
    # its runtime: what it needs from ROCm (and ROCm's own needs that are not
    # part of every system); the GPU driver's libdrm stays the system's
    runtime=()
    while read -r library; do
        runtime+=(--runtime "${library}")
    done < <(ldd "${module}" | awk '$3 ~ /^\// { print $3 }' |
             grep -E '^/opt/rocm|/lib(numa|elf|tinfo)\.so' | grep -v 'libdrm' | sort -u)
    for license in hip/LICENSE.md hsa-rocr/LICENSE.md rocprofiler-register/LICENSE.md; do
        name="AMD-$(dirname "${license}")-LICENSE.md"
        cp "/opt/rocm/share/doc/${license}" "/tmp/${name}"
        runtime+=(--runtime "/tmp/${name}")
    done
    rm -rf "${out}/compute/hip" && mkdir -p "${out}/compute/hip"
    python3 /src/packaging/release/compute_modules.py pack --build /build/hip \
        --backend hip --os linux "${runtime[@]}" --out "${out}/compute/hip"
fi

step "ccache"
ccache --show-stats | grep -E "Hits|Misses|Cache size" || true
