#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dockerfile="${repo_root}/packaging/release/linux-toolchain.Dockerfile"
context="${repo_root}/packaging/release"
dockerfile_hash="$(sha256sum "${dockerfile}" | cut -d' ' -f1)"
image="forevertas-linux-toolchain:${dockerfile_hash:0:16}"

if docker image inspect "${image}" >/dev/null 2>&1 &&
        [[ "$(docker image inspect --format '{{ index .Config.Labels "dev.skycrafter.forevertas.toolchain-hash" }}' "${image}")" == "${dockerfile_hash}" ]]; then
    printf 'Reusing toolchain image %s\n' "${image}" >&2
else
    printf 'Building toolchain image %s\n' "${image}" >&2
    docker build \
        --file "${dockerfile}" \
        --label "dev.skycrafter.forevertas.toolchain-hash=${dockerfile_hash}" \
        --tag "${image}" \
        "${context}" >&2
fi

docker image inspect "${image}" >/dev/null
printf '%s\n' "${image}"
