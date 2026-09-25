ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
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
        cuda-profiler-api-12-8=12.8.90-1 \
    && rm -rf /var/lib/apt/lists/*

RUN hipcc --version && nvcc --version && test -x /opt/rocm/bin/hipcc
