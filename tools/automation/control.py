#!/usr/bin/env python3
"""Submit retry-safe local UE control requests without keyboard/mouse automation.

The matching protocol-13 client reads its user-local control directory while
running. A request ID identifies immutable intent; reuse it after a timeout.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

NAME = re.compile(r'[A-Za-z0-9_-]{1,64}\Z')
SERIAL = re.compile(r'[A-Za-z0-9_.:-]{1,128}\Z')
PACKAGE = 'org.mrfps.arena'
OPERATIONS = {
    'status', 'view.set', 'multiview.set', 'reconstructed.set', 'focus.set',
    'window.set', 'build-mode.set', 'admin.action', 'objects.list',
    'object.ensure', 'object.remove', 'wall.set',
    'player.calibrate', 'haptics.set', 'capture.set',
    'menu.set', 'connection.set', 'camera.set',
}


def root_directory() -> Path:
    if os.name == 'nt':
        value = os.environ.get('LOCALAPPDATA', '')
        if not value or not Path(value).is_absolute():
            raise ValueError('LOCALAPPDATA must be an absolute path')
        return Path(value) / 'MRFPS' / 'Automation'
    if sys.platform == 'darwin':
        return Path.home() / 'Library' / 'Application Support' / 'MRFPS' / 'Automation'
    value = os.environ.get('XDG_STATE_HOME', '')
    return (Path(value) if value and Path(value).is_absolute() else Path.home() / '.local' / 'state') / 'MRFPS' / 'Automation'


def prepare_directory(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if os.name != 'nt':
        path.chmod(0o700)
        return
    # Set only DACL access entries. Set-Acl with an empty SACL requires
    # SeSecurityPrivilege, while DirectoryInfo.SetAccessControl does not.
    script = ("$ErrorActionPreference='Stop'; $d=New-Object System.IO.DirectoryInfo($env:MRFPS_CONTROL_DIRECTORY); "
              "$a=$d.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access); "
              "$a.SetAccessRuleProtection($true,$false); foreach($x in @($a.Access)){$a.RemoveAccessRuleSpecific($x)}; "
              "$i=[System.Security.Principal.WindowsIdentity]::GetCurrent().User; "
              "$r=New-Object System.Security.AccessControl.FileSystemAccessRule($i,'FullControl','ContainerInherit,ObjectInherit','None','Allow'); "
              "$a.AddAccessRule($r); $d.SetAccessControl($a)")
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
                            env={**os.environ, 'MRFPS_CONTROL_DIRECTORY': str(path)},
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode:
        raise OSError('failed to restrict the control directory to this user')


@contextmanager
def lock_directory(path: Path):
    """OS-released file lock avoids permanent locks after a CLI crash."""
    with (path / '.lock').open('a+b') as lock:
        lock.seek(0, os.SEEK_END)
        if not lock.tell():
            lock.write(b'0')
            lock.flush()
        lock.seek(0)
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == 'nt':
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def encode(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(',', ':'), allow_nan=False).encode('ascii')


def atomic_write(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with os.fdopen(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        try:
            tmp.unlink()
        except FileNotFoundError:
            pass


def submit(directory: Path, request: dict) -> Path:
    operation_id = request['id']
    folder = directory / operation_id
    with lock_directory(directory):
        folder.mkdir(exist_ok=True, mode=0o700)
        if folder.is_symlink() or folder.resolve().parent != directory.resolve():
            raise ValueError('unsafe operation directory')
        file = folder / 'request.json'
        if file.exists():
            if file.is_symlink() or file.stat().st_size > 8192:
                raise ValueError('unsafe request file')
            if json.loads(file.read_text(encoding='utf-8')) != request:
                raise ValueError('operation ID already identifies a different request')
        else:
            # A result without a matching request must never be reused.
            if (folder / 'result.json').exists():
                raise ValueError('operation ID already has a result but no request')
            atomic_write(file, encode(request))
    return folder / 'result.json'


def adb(args: argparse.Namespace, command: list[str], data: bytes | None = None, check: bool = True) -> bytes | None:
    result = subprocess.run([args.adb, '-s', args.serial, *command], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    if result.returncode:
        if check:
            raise OSError('ADB operation failed; verify the Quest connection and debuggable APK')
        return None
    return result.stdout


def quest_submit(args: argparse.Namespace, request: dict) -> str:
    # The only interpolated values pass strict NAME / UUID validation.
    base = f'files/MRFPS/Automation/{args.name}/{args.operation_id}'
    request_path = base + '/request.json'
    existing = adb(args, ['shell', 'run-as', PACKAGE, 'cat', request_path], check=False)
    if existing is not None:
        if json.loads(existing) != request:
            raise ValueError('Quest operation ID already identifies a different request')
    else:
        # Android's app-private SELinux policy can forbid hard links. An
        # atomic mkdir lock prevents two ADB hosts replacing each other's
        # requests; an interrupted publisher leaves a lock and fails closed.
        temporary = base + '/request.' + uuid.uuid4().hex + '.tmp'
        lock = base + '/.publish-lock'
        script = (f"umask 077; mkdir -p {base} || exit 1; mkdir {lock} 2>/dev/null || exit 75; "
                  f"cat > {temporary}; result=$?; "
                  f"if [ $result -eq 0 ] && [ ! -f {request_path} ]; then mv {temporary} {request_path}; result=$?; fi; "
                  f"rm -f {temporary}; rmdir {lock}; exit $result")
        adb(args, ['shell', '-T', f"run-as {PACKAGE} sh -c '{script}'"], data=encode(request))
        published = adb(args, ['shell', 'run-as', PACKAGE, 'cat', request_path])
        if json.loads(published) != request:
            raise ValueError('Quest operation ID was concurrently claimed by a different request')
    return base + '/result.json'


def quest_result(args: argparse.Namespace, remote_path: str) -> dict | None:
    data = adb(args, ['shell', 'run-as', PACKAGE, 'cat', remote_path], check=False)
    if data is None:
        return None
    if len(data) > 16384:
        raise ValueError('Quest result is too large')
    return json.loads(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='MRFPS-Client', help='client window name')
    parser.add_argument('--platform', choices=('desktop','quest'), default='desktop')
    parser.add_argument('--serial', default='', help='required for Quest')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--operation-id', required=True, help='UUIDv4: reuse the same ID to retry')
    parser.add_argument('--operation', required=True, choices=sorted(OPERATIONS))
    parameter_input = parser.add_mutually_exclusive_group()
    parameter_input.add_argument('--parameters', help='JSON object describing the requested state')
    parameter_input.add_argument('--parameters-file', type=Path, help='UTF-8 JSON file; avoids shell quoting')
    parser.add_argument('--wait-seconds', type=float, default=5)
    args = parser.parse_args()
    try:
        if not NAME.fullmatch(args.name):
            raise ValueError('invalid client name')
        if args.platform == 'quest' and not SERIAL.fullmatch(args.serial):
            raise ValueError('Quest serial is required and must be a valid ADB serial')
        parsed_id = uuid.UUID(args.operation_id)
        if parsed_id.version != 4 or str(parsed_id) != args.operation_id:
            raise ValueError('operation ID must be a canonical lowercase UUIDv4')
        if not 0 <= args.wait_seconds <= 60:
            raise ValueError('wait-seconds must be between 0 and 60')
        if args.parameters_file:
            if args.parameters_file.stat().st_size > 8192:
                raise ValueError('parameters file is too large')
            parameter_json = args.parameters_file.read_text(encoding='utf-8-sig')
        else:
            parameter_json = args.parameters or '{}'
        params = json.loads(parameter_json, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('non-finite value')))
        if not isinstance(params, dict) or len(encode(params)) > 4096:
            raise ValueError('parameters must be a JSON object of at most 4096 bytes')
        directory = (root_directory() / ('quest-' + hashlib.sha256(args.serial.encode()).hexdigest()[:24]) / args.name
                     if args.platform == 'quest' else root_directory() / args.name)
        prepare_directory(directory)
        request = {'version': 1, 'id': args.operation_id, 'operation': args.operation, 'parameters': params}
        result_file = submit(directory, request)
        remote_path = None
        if args.platform == 'quest' and not result_file.exists():
            with lock_directory(directory):
                remote_path = quest_submit(args,request)
        deadline = time.monotonic() + args.wait_seconds
        while not result_file.exists() and time.monotonic() < deadline:
            if remote_path:
                remote = quest_result(args,remote_path)
                if remote is not None:
                    atomic_write(result_file,encode(remote))
                    break
            time.sleep(.1)
        if result_file.exists():
            if result_file.is_symlink() or result_file.stat().st_size > 16384:
                raise ValueError('unsafe result file')
            response = json.loads(result_file.read_text(encoding='utf-8'))
            if response.get('id') != args.operation_id:
                raise ValueError('client returned a mismatched operation ID')
            print(json.dumps(response, ensure_ascii=False))
            return 0 if response.get('status') in ('ok', 'already_applied') else 2 if response.get('status') == 'submitted' else 1
        print(json.dumps({'id': args.operation_id, 'status': 'pending', 'message': 'No completed client response yet; retry with the same operation ID.'}))
        return 2
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print(json.dumps({'status':'error','message':str(error)},ensure_ascii=False),file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
