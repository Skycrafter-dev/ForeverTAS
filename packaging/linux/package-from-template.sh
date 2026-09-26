#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${FOREVERTAS_BUILD_DIR:?FOREVERTAS_BUILD_DIR is required}"
appdir="${FOREVERTAS_APPDIR:-${build_dir}/AppDir}"
template="${FOREVERTAS_PACKAGE_TEMPLATE_APPDIR:?template AppDir is required}"
dist_dir="${FOREVERTAS_DIST_DIR:-${repo_root}/dist}"
tools_dir="${FOREVERTAS_TOOLS_DIR:-${repo_root}/build/package-tools}"
linuxdeploy="${LINUXDEPLOY:-${tools_dir}/linuxdeploy-x86_64.AppImage}"

test -d "${template}/usr/plugins/platforms"
test -x "${build_dir}/bin/ForeverTAS"
test -x "${build_dir}/bin/forevertas-simulation-debug-worker"
test -x "${linuxdeploy}"
update_asset_id="$(sed -n 's/^FOREVERTAS_UPDATE_ASSET_ID:INTERNAL=//p' \
    "${build_dir}/CMakeCache.txt")"
version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' \
    "${build_dir}/CMakeCache.txt")"
[[ "${update_asset_id}" =~ ^linux-nvidia-sm[0-9]+-x86_64$ ]] || {
    echo "Only matching NVIDIA compute builds can use the NVIDIA template" >&2
    exit 1
}
[[ "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || exit 1

rm -rf "${appdir}"
cp -a --reflink=auto "${template}" "${appdir}"
cp -a --reflink=auto "${build_dir}/bin/ForeverTAS" \
    "${appdir}/usr/bin/ForeverTAS"
cp -a --reflink=auto "${build_dir}/bin/forevertas-simulation-debug-worker" \
    "${appdir}/usr/bin/forevertas-simulation-debug-worker"

# A template may be reused only when every file outside the two executables
# is from the same source and toolchain. The release driver records both.
mkdir -p "${dist_dir}"
output="${dist_dir}/ForeverTAS-${version}-${update_asset_id}.AppImage"
rm -f "${output}" "${output}.sha256"
APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 LDAI_OUTPUT="${output}" \
LD_LIBRARY_PATH="${appdir}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${linuxdeploy}" --appdir "${appdir}" --output appimage

smoke_root="$(mktemp -d)"
trap 'rm -rf "${smoke_root}"' EXIT
(
    cd "${smoke_root}"
    "${output}" --appimage-extract >/dev/null
)
extracted="${smoke_root}/squashfs-root"
test -x "${extracted}/usr/bin/ForeverTAS"
test -f "${extracted}/usr/lib/libnvrtc-builtins.so.12.8"
test -f "${extracted}/usr/plugins/platforms/libqxcb.so"
if find "${extracted}" \( -type f -o -type l \) -name 'libcuda.so*' \
        -print -quit | grep -q .; then
    echo "AppImage must not bundle the NVIDIA driver" >&2
    exit 1
fi
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
APPIMAGE_EXTRACT_AND_RUN=1 "${output}" --qml-smoke-test
(
    cd "${dist_dir}"
    sha256sum "$(basename "${output}")" > "$(basename "${output}").sha256"
)
