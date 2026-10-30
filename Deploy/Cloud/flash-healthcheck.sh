#!/bin/sh
set -eu
# This fallback is limited to Flash's unprivileged game process; the Python
# probe still requires PID/UID peer credentials and a fresh game-thread reply.
export MRFPS_HEALTH_LOCAL_SOCKET=1
# Docker/Flash launches probes using the configured container UID (root on
# Flash). /proc ownership must match the already unprivileged game process.
if [ "$(id -u)" = 0 ]; then
    exec setpriv --reuid=10001 --regid=10001 --clear-groups \
      --bounding-set=-all --inh-caps=-all --ambient-caps=-all --no-new-privs \
      /usr/bin/python3 /usr/local/bin/mrfps-healthcheck.py
fi
if [ "$(id -u)" != 10001 ]; then exit 77; fi
exec /usr/bin/python3 /usr/local/bin/mrfps-healthcheck.py
