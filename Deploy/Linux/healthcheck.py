"""Check the server owns its bound game UDP socket; no claim about full match health."""
import os
import pathlib
import socket
import struct
import sys
from auth_config import validate_environment

HEALTH_ADDRESS = b'\0mrfps-health-v1'
HEALTH_MAGIC = b'MRFH1'
HEALTH_NONCE_BYTES = 16
HEALTH_RESPONSE_BYTES = 24
GAME_EXECUTABLES = frozenset((
    '/opt/mrfps/MRFPS/Binaries/Linux/MRFPSServer',
    '/opt/mrfps/MRFPS/Binaries/Linux/MRFPSServer-Linux-Shipping',
))


def check_local_responder(port, expected_pid=1, expected_uid=10001, address=HEALTH_ADDRESS):
    """Verify the actual peer process and one fresh game-thread response."""
    if not (1 <= port <= 65535) or os.geteuid() != expected_uid:
        return False
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2.0)
            client.connect(address)
            size = struct.calcsize('3i')
            pid, uid, _gid = struct.unpack('3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, size))
            if pid != expected_pid or uid != expected_uid:
                return False
            nonce = os.urandom(HEALTH_NONCE_BYTES)
            client.sendall(HEALTH_MAGIC + nonce)
            response = bytearray()
            while len(response) < HEALTH_RESPONSE_BYTES:
                chunk = client.recv(HEALTH_RESPONSE_BYTES - len(response))
                if not chunk:
                    return False
                response.extend(chunk)
            return response == HEALTH_MAGIC + nonce + struct.pack('!HB', port, 0x03)
    except (OSError, ValueError, struct.error):
        return False


def check_health(proc_root=pathlib.Path('/proc'), port=None, expected_executables=GAME_EXECUTABLES):
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
            executable = os.readlink(process / 'exe')
            if executable not in expected_executables:
                continue
            for fd in (process / 'fd').iterdir():
                if os.readlink(fd) in sockets:
                    return True
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            continue
    # gVisor may deny /proc/<pid>/fd even when the process is the same UID.
    # The Flash-only fallback requires the game executable as PID 1 and asks
    # that very process for a fresh response on an abstract, local socket.
    if os.environ.get('MRFPS_HEALTH_LOCAL_SOCKET') == '1' and os.geteuid() == 10001:
        try:
            executable = os.readlink(proc_root / '1' / 'exe')
            if executable in expected_executables:
                return check_local_responder(port)
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            pass
    return False


if __name__ == '__main__':
    try:
        healthy = check_health()
    except (ValueError, OSError):
        healthy = False
    print('MRFPSServer game UDP socket ready' if healthy else 'MRFPSServer game UDP socket unavailable')
    sys.exit(0 if healthy else 1)
