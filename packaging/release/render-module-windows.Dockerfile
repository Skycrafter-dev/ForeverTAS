# The Windows In-game render module's toolchain: MinGW-w64 (GCC 13, posix
# threads) and what the module's build script (its repository's
# forevertas/build-module.sh windows) compiles its dependencies with. The
# Linux module builds in linux.Dockerfile instead (the AppImage's glibc);
# Windows needs a newer MinGW than Ubuntu 22.04's (GCC 10 has no
# floating-point std::to_chars). release.py tags it by this file's hash.
FROM ubuntu:24.04@sha256:77eb101c607d6e3d4f5ad0c8a02c2136c689b98aba1f359e6502ba517e5c57ec

ARG DEBIAN_FRONTEND=noninteractive
ARG CCACHE_VERSION=4.11.3
ARG CCACHE_SHA256=7766991b91b3a5a177ab33fa043fe09e72c68586d5a86d20a563a05b74f119c0

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake bison build-essential ca-certificates cmake curl \
        flex g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix git \
        libglm-dev libtool libvulkan-dev mingw-w64-tools make meson nasm \
        ninja-build perl pkg-config python3 xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN curl --fail --location --retry 3 \
        "https://github.com/ccache/ccache/releases/download/v${CCACHE_VERSION}/ccache-${CCACHE_VERSION}-linux-x86_64.tar.xz" \
        -o /tmp/ccache.tar.xz \
    && echo "${CCACHE_SHA256}  /tmp/ccache.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/ccache.tar.xz -C /tmp \
    && install -m 0755 "/tmp/ccache-${CCACHE_VERSION}-linux-x86_64/ccache" /usr/local/bin/ccache \
    && rm -rf /tmp/ccache.tar.xz "/tmp/ccache-${CCACHE_VERSION}-linux-x86_64"

RUN x86_64-w64-mingw32-g++-posix --version | grep -q "(GCC) 13" \
    && ccache --version | grep -F "${CCACHE_VERSION}"
