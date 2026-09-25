#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
base_image="$("${repo_root}/packaging/release/ensure-linux-toolchain.sh")"
dockerfile="${repo_root}/packaging/release/linux-hip-toolchain.Dockerfile"
identity="$(printf '%s\n' "${base_image}" "$(sha256sum "${dockerfile}" | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1)"
image="forevertas-linux-hip:${identity:0:16}"

if ! docker image inspect "${image}" >/dev/null 2>&1; then
    docker build --file "${dockerfile}" --build-arg "BASE_IMAGE=${base_image}" \
        --tag "${image}" "${repo_root}/packaging/release" >&2
fi
docker image inspect --format '{{.Id}}' "${image}"
