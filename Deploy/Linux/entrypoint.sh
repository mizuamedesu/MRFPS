#!/bin/sh
set -eu
/usr/bin/python3 /usr/local/bin/auth_config.py
MRFPS_PORT="${MRFPS_PORT:-7777}"
export MRFPS_PORT
case "${MRFPS_PORT:-7777}" in
    ''|*[!0-9]*) echo 'MRFPS_PORT must be an integer.' >&2; exit 64 ;;
esac
if [ "$MRFPS_PORT" -lt 1 ] || [ "$MRFPS_PORT" -gt 65535 ]; then
    echo 'MRFPS_PORT must be between 1 and 65535.' >&2
    exit 64
fi
cd /opt/mrfps
binary=/opt/mrfps/MRFPS/Binaries/Linux/MRFPSServer
if [ ! -f "$binary" ]; then binary=/opt/mrfps/MRFPS/Binaries/Linux/MRFPSServer-Linux-Shipping; fi
if [ ! -x "$binary" ]; then echo 'Packaged MRFPSServer executable is missing.' >&2; exit 66; fi
exec "$binary" "$@" "-port=$MRFPS_PORT" -log -unattended -NullRHI -nosound -NoSplash -stdout -FullStdOutLogOutput
