"""Check the server owns its bound game UDP socket; no claim about full match health."""
import os
import pathlib
import sys
from auth_config import validate_environment


def check_health(proc_root=pathlib.Path('/proc'), port=None):
    if not validate_environment():
        return False
    port = int(port if port is not None else os.environ.get('MRFPS_PORT', '7777'))
    sockets = set()
    for name in ('udp', 'udp6'):
        path = proc_root / 'net' / name
        if not path.is_file():
            continue
        for line in path.read_text().splitlines()[1:]:
            fields = line.split()
            if len(fields) >= 10 and int(fields[1].rsplit(':', 1)[1], 16) == port:
                sockets.add('socket:[' + fields[9] + ']')
    if not sockets:
        return False
    for process in proc_root.iterdir():
        if not process.name.isdigit():
            continue
        try:
            executable = os.readlink(process / 'exe').rsplit('/', 1)[-1]
            if executable not in ('MRFPSServer', 'MRFPSServer-Linux-Shipping'):
                continue
            for fd in (process / 'fd').iterdir():
                if os.readlink(fd) in sockets:
                    return True
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            continue
    return False


if __name__ == '__main__':
    try:
        healthy = check_health()
    except (ValueError, OSError):
        healthy = False
    print('MRFPSServer game UDP socket ready' if healthy else 'MRFPSServer game UDP socket unavailable')
    sys.exit(0 if healthy else 1)
