FROM ubuntu:22.04@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982

ARG DEBIAN_FRONTEND=noninteractive
ARG CMAKE_VERSION=4.4.0
ARG AQTINSTALL_VERSION=3.3.0
ARG SCCACHE_VERSION=0.17.0
ARG SCCACHE_SHA256=67c4a96dd237c1f518f6b36083f270f9976d516f1e57fce891755ea782e50006
ARG CUDA_KEYRING_SHA256=d93190d50b98ad4699ff40f4f7af50f16a76dac3bb8da1eaaf366d47898ff8df

RUN apt-get update && apt-get install -y --no-install-recommends \
        appstream build-essential ca-certificates curl desktop-file-utils \
        file git libasound2-dev libdbus-1-3 libegl1 libfontconfig1 libfuse2 \
        libgl1-mesa-dev libgstreamer-gl1.0-0 libpulse-dev libssl-dev \
        libnspr4-dev libnss3-dev libxcomposite-dev libxdamage-dev \
        libx11-xcb1 libxcb-cursor0 libxcb-glx0 libxcb-icccm4 \
        libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
        libxcb-render0 libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-util1 \
        libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb-dev libxcb-xkb1 libxcb1 \
        libxi6 libxkbcommon-dev libxkbcommon-x11-0 libxkbfile-dev \
        libxrandr-dev libxrender1 libxtst-dev locales \
        ninja-build p7zip-full patchelf pkg-config python3 python3-pip \
        software-properties-common wget xz-utils zlib1g-dev \
    && locale-gen de_DE.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q \
        https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb \
        -O /tmp/cuda-keyring.deb \
    && echo "${CUDA_KEYRING_SHA256}  /tmp/cuda-keyring.deb" | sha256sum -c - \
    && dpkg -i /tmp/cuda-keyring.deb \
    && rm /tmp/cuda-keyring.deb \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        cuda-cccl-12-8=12.8.90-1 cuda-cudart-12-8=12.8.90-1 \
        cuda-cuobjdump-12-8=12.8.90-1 cuda-nvcc-12-8=12.8.93-1 \
        cuda-nvrtc-dev-12-8=12.8.93-1 libnvjitlink-dev-12-8=12.8.93-1 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir \
        "aqtinstall==${AQTINSTALL_VERSION}" \
        "cmake==${CMAKE_VERSION}" \
        "py7zr==1.0.0" \
    && aqt install-qt -O /opt/Qt \
        linux desktop 6.9.3 linux_gcc_64 \
        -m qtpositioning qtquick3d qtserialport qtshadertools qtwebchannel \
           qtwebengine

RUN curl --fail --location --retry 3 \
        "https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/sccache-v${SCCACHE_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
        -o /tmp/sccache.tar.gz \
    && echo "${SCCACHE_SHA256}  /tmp/sccache.tar.gz" | sha256sum -c - \
    && mkdir -p /tmp/sccache \
    && tar -xzf /tmp/sccache.tar.gz -C /tmp/sccache --strip-components=1 \
    && install -m 0755 /tmp/sccache/sccache /usr/local/bin/sccache \
    && rm -rf /tmp/sccache /tmp/sccache.tar.gz

ENV CUDA_PATH=/usr/local/cuda-12.8 \
    LANG=de_DE.UTF-8 \
    LC_ALL=de_DE.UTF-8 \
    QML2_IMPORT_PATH=/opt/Qt/6.9.3/gcc_64/qml \
    QMAKE=/opt/Qt/6.9.3/gcc_64/bin/qmake \
    QT_PLUGIN_PATH=/opt/Qt/6.9.3/gcc_64/plugins \
    QT_ROOT_DIR=/opt/Qt/6.9.3/gcc_64
ENV PATH="${QT_ROOT_DIR}/bin:${CUDA_PATH}/bin:${PATH}"
ENV LD_LIBRARY_PATH="${QT_ROOT_DIR}/lib:${CUDA_PATH}/lib64"

RUN cmake --version | grep -F "cmake version ${CMAKE_VERSION}" \
    && nvcc --version | grep -F "release 12.8" \
    && qmake -query QT_VERSION | grep -Fx "6.9.3" \
    && sccache --version | grep -F "sccache ${SCCACHE_VERSION}" \
    && test -x "${CUDA_PATH}/bin/cuobjdump"
