#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${FOREVERTAS_BUILD_DIR:-${repo_root}/build/package-linux}"
dist_dir="${FOREVERTAS_DIST_DIR:-${repo_root}/dist}"
tools_dir="${FOREVERTAS_TOOLS_DIR:-${repo_root}/build/package-tools}"
appdir="${FOREVERTAS_APPDIR:-${build_dir}/AppDir}"
linuxdeploy_version="${LINUXDEPLOY_VERSION:-1-alpha-20251107-1}"
qt_plugin_version="${LINUXDEPLOY_PLUGIN_QT_VERSION:-1-alpha-20250213-1}"

case "$(uname -m)" in
    x86_64|amd64)
        appimage_arch="x86_64"
        linuxdeploy_sha256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
        qt_plugin_sha256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"
        ;;
    aarch64|arm64)
        appimage_arch="aarch64"
        linuxdeploy_sha256="620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff"
        qt_plugin_sha256="bf1c24aff6d749b5cf423afad6f15abd4440f81dec1aab95706b25f6667cdcf1"
        ;;
    *)
        echo "Unsupported AppImage architecture: $(uname -m)" >&2
        exit 2
        ;;
esac

mkdir -p "${dist_dir}" "${tools_dir}"

linuxdeploy="${LINUXDEPLOY:-${tools_dir}/linuxdeploy-${appimage_arch}.AppImage}"
qt_plugin="${LINUXDEPLOY_PLUGIN_QT:-${tools_dir}/linuxdeploy-plugin-qt-${appimage_arch}.AppImage}"

download_tool() {
    local destination="$1"
    local url="$2"
    local expected_sha256="$3"
    local temporary
    if [[ -f "${destination}" ]] &&
            ! echo "${expected_sha256}  ${destination}" | sha256sum -c - >/dev/null 2>&1; then
        echo "Discarding invalid cached tool $(basename "${destination}")" >&2
        rm -f "${destination}"
    fi
    if [[ ! -f "${destination}" ]]; then
        echo "Downloading $(basename "${destination}")"
        temporary="${destination}.tmp.$$"
        rm -f "${temporary}"
        curl --fail --location --retry 3 --output "${temporary}" "${url}"
        echo "${expected_sha256}  ${temporary}" | sha256sum -c -
        chmod +x "${temporary}"
        mv "${temporary}" "${destination}"
    fi
    test -x "${destination}"
}

download_tool "${linuxdeploy}" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/${linuxdeploy_version}/linuxdeploy-${appimage_arch}.AppImage" \
    "${linuxdeploy_sha256}"
download_tool "${qt_plugin}" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${qt_plugin_version}/linuxdeploy-plugin-qt-${appimage_arch}.AppImage" \
    "${qt_plugin_sha256}"

cmake_args=(
    -S "${repo_root}"
    -B "${build_dir}"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=/usr
    -DBUILD_TESTING=OFF
)
if [[ -n "${FOREVERTAS_ENABLE_CUDA:-}" ]]; then
    cmake_args+=(
        "-DFOREVERTAS_ENABLE_CUDA=${FOREVERTAS_ENABLE_CUDA}"
    )
fi
if [[ -n "${FOREVERTAS_CUDA_ARCHITECTURES:-}" ]]; then
    cmake_args+=(
        "-DCMAKE_CUDA_ARCHITECTURES=${FOREVERTAS_CUDA_ARCHITECTURES}"
    )
fi
if [[ -n "${FOREVERTAS_VALIDATOR_SOURCE:-}" ]]; then
    cmake_args+=(
        "-DFETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR=${FOREVERTAS_VALIDATOR_SOURCE}"
    )
fi
if [[ "${FOREVERTAS_SKIP_BUILD:-0}" == "1" ]]; then
    test -f "${build_dir}/CMakeCache.txt"
    test -x "${build_dir}/bin/ForeverTAS"
else
    cmake "${cmake_args[@]}"
    cmake --build "${build_dir}" --parallel
fi

rm -rf "${appdir}"
DESTDIR="${appdir}" cmake --install "${build_dir}"

desktop-file-validate \
    "${appdir}/usr/share/applications/dev.skycrafter.forevertas.desktop"
appstreamcli validate --no-net \
    "${appdir}/usr/share/metainfo/dev.skycrafter.forevertas.appdata.xml"

if [[ "${FOREVERTAS_ENABLE_CUDA:-OFF}" == "ON" ]]; then
    nvrtc_builtins="${CUDA_PATH:?CUDA_PATH is required for a CUDA AppImage}/targets/${appimage_arch}-linux/lib/libnvrtc-builtins.so.12.8"
    if [[ ! -e "${nvrtc_builtins}" ]]; then
        echo "CUDA NVRTC builtins library does not exist: ${nvrtc_builtins}" >&2
        exit 1
    fi
    nvrtc_builtins_real="$(readlink -f "${nvrtc_builtins}")"
    mkdir -p "${appdir}/usr/lib"
    cp -L "${nvrtc_builtins_real}" \
        "${appdir}/usr/lib/$(basename "${nvrtc_builtins_real}")"
    ln -sfn "$(basename "${nvrtc_builtins_real}")" \
        "${appdir}/usr/lib/libnvrtc-builtins.so.12.8"
fi

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "${build_dir}/CMakeCache.txt")"
if [[ -z "${version}" ]]; then
    version="0.0.0"
fi

find_qt6_qmake() {
    local candidate resolved candidate_version
    local candidates=()

    if [[ -n "${QMAKE:-}" ]]; then
        candidates+=("${QMAKE}")
    fi
    candidates+=(qmake6 qmake-qt6 qmake)

    for candidate in "${candidates[@]}"; do
        if [[ "${candidate}" == */* ]]; then
            resolved="${candidate}"
            [[ -x "${resolved}" ]] || continue
        else
            resolved="$(command -v "${candidate}" 2>/dev/null || true)"
            [[ -n "${resolved}" ]] || continue
        fi

        candidate_version="$("${resolved}" -query QT_VERSION 2>/dev/null || true)"
        if [[ "${candidate_version}" == 6.* ]]; then
            printf '%s\n' "${resolved}"
            return 0
        fi
    done

    echo "Could not find a Qt 6 qmake executable. Set QMAKE explicitly." >&2
    return 1
}

real_qmake="$(find_qt6_qmake)"
real_qt_plugins="$("${real_qmake}" -query QT_INSTALL_PLUGINS)"
if [[ ! -d "${real_qt_plugins}" ]]; then
    echo "Qt plugin directory does not exist: ${real_qt_plugins}" >&2
    exit 1
fi

# Distribution Qt installations can contain third-party plugin packs unrelated
# to ForeverTAS. Present a filtered plugin tree to linuxdeploy so optional KDE
# image format plugins cannot pull in unavailable codecs or inflate the bundle.
filtered_qt_plugins="${build_dir}/qt-plugins-forevertas"
rm -rf "${filtered_qt_plugins}"
mkdir -p "${filtered_qt_plugins}"
cp -a "${real_qt_plugins}/." "${filtered_qt_plugins}/"

filter_plugin_directory() {
    local directory="$1"
    shift
    rm -rf "${filtered_qt_plugins}/${directory}"
    mkdir -p "${filtered_qt_plugins}/${directory}"

    local plugin
    for plugin in "$@"; do
        if [[ -e "${real_qt_plugins}/${directory}/${plugin}" ]]; then
            cp -a "${real_qt_plugins}/${directory}/${plugin}" \
                "${filtered_qt_plugins}/${directory}/"
        fi
    done
}

filter_plugin_directory imageformats \
    libqgif.so \
    libqico.so \
    libqjpeg.so \
    libqsvg.so \
    libqwebp.so
filter_plugin_directory platforminputcontexts \
    libcomposeplatforminputcontextplugin.so \
    libibusplatforminputcontextplugin.so
rm -rf "${filtered_qt_plugins}/platformthemes" \
       "${filtered_qt_plugins}/styles"

# linuxdeploy-plugin-qt deploys explicitly requested Wayland platform plugins,
# but not the dynamically loaded client integrations they require at runtime.
for plugin_directory in \
        wayland-decoration-client \
        wayland-graphics-integration-client \
        wayland-shell-integration; do
    if [[ ! -d "${filtered_qt_plugins}/${plugin_directory}" ]]; then
        echo "Required Qt Wayland plugin directory is missing: ${plugin_directory}" >&2
        exit 1
    fi
    mkdir -p "${appdir}/usr/plugins"
    cp -a "${filtered_qt_plugins}/${plugin_directory}" \
        "${appdir}/usr/plugins/"
done

qmake_wrapper="${build_dir}/qmake-forevertas"
cat > "${qmake_wrapper}" <<'QMAKE_WRAPPER'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "-query" && "$#" -eq 1 ]]; then
    while IFS= read -r line; do
        if [[ "${line}" == QT_INSTALL_PLUGINS:* ]]; then
            printf 'QT_INSTALL_PLUGINS:%s\n' "${FOREVERTAS_FILTERED_QT_PLUGINS}"
        else
            printf '%s\n' "${line}"
        fi
    done < <("${FOREVERTAS_REAL_QMAKE}" -query)
else
    exec "${FOREVERTAS_REAL_QMAKE}" "$@"
fi
QMAKE_WRAPPER
chmod +x "${qmake_wrapper}"

export FOREVERTAS_REAL_QMAKE="${real_qmake}"
export FOREVERTAS_FILTERED_QT_PLUGINS="${filtered_qt_plugins}"
export QMAKE="${qmake_wrapper}"
export QML_SOURCES_PATHS="${repo_root}/qml"
export EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS:-libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so}"
output="${dist_dir}/ForeverTAS-${version}-linux-${appimage_arch}.AppImage"
rm -f "${output}" "${output}.sha256"
export LDAI_OUTPUT="${output}"
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$(dirname "${qt_plugin}"):${PATH}"

if [[ "${FOREVERTAS_ENABLE_STRIP:-0}" == "1" ]]; then
    unset NO_STRIP
else
    export NO_STRIP=1
fi

"${linuxdeploy}" \
    --appdir "${appdir}" \
    --desktop-file "${appdir}/usr/share/applications/dev.skycrafter.forevertas.desktop" \
    --icon-file "${appdir}/usr/share/icons/hicolor/256x256/apps/dev.skycrafter.forevertas.png" \
    --exclude-library 'libcuda.so*' \
    --executable "${appdir}/usr/bin/ForeverTAS" \
    --plugin qt \
    --output appimage

smoke_root="$(mktemp -d)"
trap 'rm -rf "${smoke_root}"' EXIT
(
    cd "${smoke_root}"
    "${output}" --appimage-extract >/dev/null
)
extracted_appdir="${smoke_root}/squashfs-root"
test -x "${extracted_appdir}/usr/bin/ForeverTAS"
for platform_plugin in libqxcb.so libqoffscreen.so \
        libqwayland-egl.so libqwayland-generic.so; do
    test -f "${extracted_appdir}/usr/plugins/platforms/${platform_plugin}"
done
test -f "${extracted_appdir}/usr/plugins/wayland-shell-integration/libxdg-shell.so"
test -f "${extracted_appdir}/usr/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so"
test -f "${extracted_appdir}/usr/plugins/wayland-decoration-client/libadwaita.so"
if [[ "${FOREVERTAS_ENABLE_CUDA:-OFF}" == "ON" ]]; then
    test -f "${extracted_appdir}/usr/lib/libnvrtc-builtins.so.12.8"
    for required_cuda_runtime in 'libnvrtc.so*' 'libnvJitLink.so*'; do
        if ! find "${extracted_appdir}" \
                \( -type f -o -type l \) \
                -name "${required_cuda_runtime}" -print -quit |
                grep -q .; then
            echo "AppImage is missing ${required_cuda_runtime}." >&2
            exit 1
        fi
    done
fi

if find "${extracted_appdir}" \
        \( -type f -o -type l \) \
        -name 'libcuda.so*' -print -quit | grep -q .; then
    echo "AppImage must not bundle the host NVIDIA driver." >&2
    exit 1
fi
if readelf -d "${extracted_appdir}/usr/bin/ForeverTAS" |
        grep -Eq 'NEEDED.*libcuda\.so'; then
    echo "ForeverTAS must not require the NVIDIA driver at process startup." >&2
    exit 1
fi

unexpected_missing_libraries="$(
    while IFS= read -r -d '' candidate; do
        ldd "${candidate}" 2>/dev/null || true
    done < <(
        find "${extracted_appdir}/usr/bin" \
                 "${extracted_appdir}/usr/plugins/platforms" \
                 "${extracted_appdir}/usr/plugins/wayland-decoration-client" \
                 "${extracted_appdir}/usr/plugins/wayland-graphics-integration-client" \
                 "${extracted_appdir}/usr/plugins/wayland-shell-integration" \
                 -type f -print0
    ) |
        awk '/not found/ { print $1 }' |
        sort -u
)"
if [[ -n "${unexpected_missing_libraries}" ]]; then
    echo "Unexpected missing AppImage libraries:" >&2
    printf '%s\n' "${unexpected_missing_libraries}" >&2
    exit 1
fi

QT_QPA_PLATFORM=offscreen \
QSG_RHI_BACKEND=software \
APPIMAGE_EXTRACT_AND_RUN=1 \
    "${output}" --qml-smoke-test
echo "Validated AppImage startup without requiring an NVIDIA driver."
rm -rf "${smoke_root}"
trap - EXIT

(
    cd "${dist_dir}"
    sha256sum "$(basename "${output}")" > "$(basename "${output}").sha256"
)
echo "Created ${output}"
