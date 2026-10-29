#!/bin/sh
set -eu
# Flash currently overrides image USER with uid 0. Keep UE's root refusal and
# drop privileges before the existing public-key validation/game entrypoint.
if [ "$(id -u)" = 0 ]; then
    exec setpriv --reuid=10001 --regid=10001 --clear-groups \
      --bounding-set=-all --inh-caps=-all --ambient-caps=-all --no-new-privs \
      /usr/local/bin/mrfps-flash-entrypoint "$@"
fi
if [ "$(id -u)" != 10001 ]; then
    echo 'MRFPS_FLASH_RUNTIME_UID_INVALID' >&2
    exit 77
fi
caps=$(awk '/^CapEff:/ { print $2 }' /proc/self/status)
case "$caps" in ''|*[!0]*) echo 'MRFPS_FLASH_CAPABILITIES_INVALID' >&2; exit 77 ;; esac
echo "MRFPS_FLASH_RUNTIME uid=10001 capabilities=$caps"
exec /usr/local/bin/mrfps-entrypoint "$@"
