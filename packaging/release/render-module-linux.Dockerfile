# The Linux In-game render module's toolchain: the app's system (the same
# Ubuntu 22.04 as linux.Dockerfile, so the module asks no newer glibc than the
# AppImage) with the libraries the module links and what its build script (its
# repository's forevertas/build-module.sh linux) compiles its dependencies
# with. A separate image, so the app's image, and its compiler cache, stay as
# they are. release.py tags it by this file's hash.
FROM ubuntu:22.04@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982

ARG DEBIAN_FRONTEND=noninteractive
ARG CMAKE_VERSION=4.4.0
ARG CCACHE_VERSION=4.11.3
ARG CCACHE_SHA256=7766991b91b3a5a177ab33fa043fe09e72c68586d5a86d20a563a05b74f119c0

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake bison build-essential ca-certificates curl file \
        flex git libegl-dev libepoxy-dev libglfw3-dev libglm-dev \
        libjpeg-turbo8-dev libssl-dev libtool libvulkan-dev make nasm \
        ninja-build patch perl pkg-config python3 python3-pip xz-utils \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir "cmake==${CMAKE_VERSION}"

RUN curl --fail --location --retry 3 \
        "https://github.com/ccache/ccache/releases/download/v${CCACHE_VERSION}/ccache-${CCACHE_VERSION}-linux-x86_64.tar.xz" \
        -o /tmp/ccache.tar.xz \
    && echo "${CCACHE_SHA256}  /tmp/ccache.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/ccache.tar.xz -C /tmp \
    && install -m 0755 "/tmp/ccache-${CCACHE_VERSION}-linux-x86_64/ccache" /usr/local/bin/ccache \
    && rm -rf /tmp/ccache.tar.xz "/tmp/ccache-${CCACHE_VERSION}-linux-x86_64"

RUN cmake --version | grep -F "cmake version ${CMAKE_VERSION}" \
    && ccache --version | grep -F "${CCACHE_VERSION}"
