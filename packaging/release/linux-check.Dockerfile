# A user's Linux, for release.py check: what a desktop has, and no CUDA, no
# ROCm, no build tools. The shipped AppImage loads every GPU module here, so
# a module missing a library it needs fails here, not on a user's computer.
FROM ubuntu:22.04@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libdbus-1-3 libegl1 libfontconfig1 libgl1 libglib2.0-0 libopengl0 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libx11-6 libx11-xcb1 \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
        libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxcb1 \
        libxkbcommon-x11-0 libxkbcommon0 libxrender1 libxi6 libxext6 libsm6 libice6 \
    && rm -rf /var/lib/apt/lists/*
