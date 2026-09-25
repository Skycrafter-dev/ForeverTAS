#!/usr/bin/env bash
set -euo pipefail

app="$1"
temporary="$(mktemp -d)"
trap 'rm -rf "${temporary}"' EXIT
installed="${temporary}/ForeverTAS.AppImage"
downloaded="${temporary}/ForeverTAS.AppImage.download"
marker="${temporary}/started"

printf '#!/bin/sh\nprintf old\\n\n' > "${installed}"
printf '#!/bin/sh\nprintf new > "$FOREVERTAS_UPDATE_TEST_MARKER"\n' > "${downloaded}"
chmod +x "${installed}" "${downloaded}"

APPIMAGE="${downloaded}" FOREVERTAS_UPDATE_TEST_MARKER="${marker}" \
    "${app}" --apply-update "${installed}" 999999999

for _ in {1..50}; do
    [[ -f "${marker}" ]] && break
    sleep 0.1
done
[[ -f "${marker}" ]]
[[ "$(<"${marker}")" == new ]]
[[ -f "${installed}.previous" ]]
[[ "$(sed -n '2p' "${installed}.previous")" == 'printf old\n' ]]

failed_installed="${temporary}/Failed.AppImage"
failed_download="${temporary}/Failed.AppImage.download"
printf '#!/bin/sh\nprintf intact\n' > "${failed_installed}"
printf 'not executable\n' > "${failed_download}"
chmod +x "${failed_installed}"
if APPIMAGE="${failed_download}" "${app}" \
        --apply-update "${failed_installed}" 999999999 2>/dev/null; then
    echo "Updater unexpectedly launched a non-executable download" >&2
    exit 1
else
    [[ "$?" == 6 ]]
fi
[[ -f "${failed_installed}" && -f "${failed_download}" ]]
[[ ! -e "${failed_installed}.previous" ]]

backup_blocked="${temporary}/BackupBlocked.AppImage"
backup_blocked_download="${backup_blocked}.download"
printf '#!/bin/sh\nprintf intact\n' > "${backup_blocked}"
printf '#!/bin/sh\nprintf new\n' > "${backup_blocked_download}"
chmod +x "${backup_blocked}" "${backup_blocked_download}"
mkdir "${backup_blocked}.previous"
if APPIMAGE="${backup_blocked_download}" "${app}" \
        --apply-update "${backup_blocked}" 999999999 2>/dev/null; then
    echo "Updater unexpectedly replaced an image with a blocked backup" >&2
    exit 1
else
    [[ "$?" == 5 ]]
fi
[[ "$(sed -n '2p' "${backup_blocked}")" == 'printf intact' ]]
[[ "$(sed -n '2p' "${backup_blocked_download}")" == 'printf new' ]]
