# The Linux release toolchain: one image for the app (Qt), the CUDA module
# and the HIP module. Ubuntu 22.04 so the AppImage runs on glibc 2.35 and
# later. release.py tags it by this file's hash: edit it, and the next
# release builds it again.
FROM ubuntu:22.04@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982

ARG DEBIAN_FRONTEND=noninteractive
ARG CMAKE_VERSION=4.4.0
ARG AQTINSTALL_VERSION=3.3.0
ARG CCACHE_VERSION=4.11.3
ARG CCACHE_SHA256=7766991b91b3a5a177ab33fa043fe09e72c68586d5a86d20a563a05b74f119c0
ARG CUDA_KEYRING_SHA256=d93190d50b98ad4699ff40f4f7af50f16a76dac3bb8da1eaaf366d47898ff8df

RUN apt-get update && apt-get install -y --no-install-recommends \
        appstream build-essential ca-certificates curl desktop-file-utils \
        file git gpg libdbus-1-3 libegl1 libfontconfig1 libfuse2 \
        libgl1-mesa-dev libgstreamer-gl1.0-0 libpulse-dev libssl-dev \
        libx11-xcb1 libxcb-cursor0 libxcb-glx0 libxcb-icccm4 \
        libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
        libxcb-render0 libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-util1 \
        libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb-dev libxcb-xkb1 libxcb1 \
        libxext-dev libxi6 libxkbcommon-dev libxkbcommon-x11-0 libxrender1 locales \
        libvulkan-dev libzstd-dev ninja-build patchelf pkg-config python3 python3-pip \
        rsync wget xz-utils zlib1g-dev zstd \
    && locale-gen de_DE.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

# CUDA 12.8: the last toolkit with every GPU generation from 5.0 (Maxwell).
RUN wget -q \
        https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb \
        -O /tmp/cuda-keyring.deb \
    && echo "${CUDA_KEYRING_SHA256}  /tmp/cuda-keyring.deb" | sha256sum -c - \
    && dpkg -i /tmp/cuda-keyring.deb \
    && rm /tmp/cuda-keyring.deb \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        cuda-cccl-12-8=12.8.90-1 cuda-cudart-dev-12-8=12.8.90-1 \
        cuda-cuobjdump-12-8=12.8.90-1 cuda-nvcc-12-8=12.8.93-1 \
        cuda-nvprune-12-8=12.8.90-1 cuda-profiler-api-12-8=12.8.90-1 \
    && rm -rf /var/lib/apt/lists/*

# ROCm 7.2.4 for the HIP module (RX 7000/9000).
RUN curl -fsSL https://repo.radeon.com/rocm/rocm.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/rocm.gpg \
    && printf '%s\n' \
        'deb [arch=amd64 signed-by=/usr/share/keyrings/rocm.gpg] https://repo.radeon.com/rocm/apt/7.2.4 jammy main' \
        > /etc/apt/sources.list.d/rocm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends --allow-downgrades \
        hip-dev rocm-hip-runtime-dev hipcub-dev \
        rocm-device-libs=1.0.0.70204-93~22.04 \
        rocm-cmake=0.14.0.70204-93~22.04 \
        rocminfo=1.0.0.70204-93~22.04 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir \
        "aqtinstall==${AQTINSTALL_VERSION}" \
        "cmake==${CMAKE_VERSION}" \
        "py7zr==1.0.0" \
    && aqt install-qt -O /opt/Qt \
        linux desktop 6.9.3 linux_gcc_64 \
        -m qtquick3d qtshadertools qtwebsockets

# ccache: caches C++, CUDA (nvcc) and HIP compiles across releases.
RUN curl --fail --location --retry 3 \
        "https://github.com/ccache/ccache/releases/download/v${CCACHE_VERSION}/ccache-${CCACHE_VERSION}-linux-x86_64.tar.xz" \
        -o /tmp/ccache.tar.xz \
    && echo "${CCACHE_SHA256}  /tmp/ccache.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/ccache.tar.xz -C /tmp \
    && install -m 0755 "/tmp/ccache-${CCACHE_VERSION}-linux-x86_64/ccache" /usr/local/bin/ccache \
    && rm -rf /tmp/ccache.tar.xz "/tmp/ccache-${CCACHE_VERSION}-linux-x86_64"

ENV CUDA_PATH=/usr/local/cuda-12.8 \
    HIP_PATH=/opt/rocm \
    ROCM_PATH=/opt/rocm \
    LANG=de_DE.UTF-8 \
    LC_ALL=de_DE.UTF-8 \
    QML2_IMPORT_PATH=/opt/Qt/6.9.3/gcc_64/qml \
    QMAKE=/opt/Qt/6.9.3/gcc_64/bin/qmake \
    QT_PLUGIN_PATH=/opt/Qt/6.9.3/gcc_64/plugins \
    QT_ROOT_DIR=/opt/Qt/6.9.3/gcc_64
ENV PATH="${QT_ROOT_DIR}/bin:${CUDA_PATH}/bin:/opt/rocm/bin:${PATH}"

RUN cmake --version | grep -F "cmake version ${CMAKE_VERSION}" \
    && nvcc --version | grep -F "release 12.8" \
    && nvprune --version >/dev/null \
    && hipcc --version >/dev/null \
    && qmake -query QT_VERSION | grep -Fx "6.9.3" \
    && ccache --version | grep -F "${CCACHE_VERSION}"

# The timeline's pointer lock on Wayland (wayland-scanner and the protocols).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libwayland-dev wayland-protocols \
    && rm -rf /var/lib/apt/lists/*
