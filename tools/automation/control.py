#!/usr/bin/env python3
"""Submit retry-safe local UE control requests without keyboard/mouse automation.

The matching protocol-14 client reads its user-local control directory while
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
    'window.set', 'build-mode.set', 'admin.action', 'objects.list', 'roster.list',
    'object.ensure', 'object.remove', 'wall.set',
    'player.calibrate', 'player.input.set', 'player.input.clear', 'player.reload', 'player.wall',
    'haptics.set', 'capture.set',
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
    archived = directory / 'archive' / operation_id
    with lock_directory(directory):
        if (directory / 'archive').is_symlink():
            raise ValueError('unsafe archive directory')
        if archived.exists():
            if folder.exists() or archived.is_symlink() or archived.resolve().parent != (directory / 'archive').resolve():
                raise ValueError('operation has conflicting or unsafe archive state')
            file = archived / 'request.json'
            if file.is_symlink() or not file.is_file() or file.stat().st_size > 8192:
                raise ValueError('unsafe archived request file')
            if json.loads(file.read_text(encoding='utf-8')) != request:
                raise ValueError('operation ID already identifies a different request')
            result = archived / 'result.json'
            if result.is_symlink() or not result.is_file():
                raise ValueError('archived operation is missing a result')
            return result
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


def valid_id(text: str) -> bool:
    try:
        value = uuid.UUID(text)
        return value.version == 4 and str(value) == text
    except ValueError:
        return False


def completed_receipt(folder: Path, operation_id: str) -> bool:
    request, result = folder / 'request.json', folder / 'result.json'
    if any(path.is_symlink() or not path.is_file() for path in (request,result)):
        return False
    if request.stat().st_size > 8192 or result.stat().st_size > 16384:
        return False
    try:
        submitted = json.loads(request.read_text(encoding='utf-8'))
        response = json.loads(result.read_text(encoding='utf-8'))
        return submitted.get('id') == response.get('id') == operation_id and response.get('status') in ('ok','already_applied','error')
    except (ValueError,OSError,AttributeError):
        return False


def archive_local_locked(directory: Path, limit: int, eligible: set[str] | None = None) -> dict:
    archive = directory / 'archive'
    archive.mkdir(exist_ok=True, mode=0o700)
    if archive.is_symlink() or archive.resolve().parent != directory.resolve():
        raise ValueError('unsafe archive directory')
    moved = skipped = 0
    for folder in sorted(directory.iterdir(),key=lambda item:item.name):
        if moved >= limit:
            break
        if not valid_id(folder.name) or folder.is_symlink() or not folder.is_dir():
            continue
        if eligible is not None and folder.name not in eligible:
            continue
        if not completed_receipt(folder,folder.name):
            skipped += 1
            continue
        destination = archive / folder.name
        if destination.exists():
            skipped += 1
            continue
        try:
            os.rename(folder,destination)
            moved += 1
        except OSError:
            # Open files or a conflicting rename must not delete/overwrite data.
            skipped += 1
    return {'archived':moved,'skipped':skipped}


def adb(args: argparse.Namespace, command: list[str], data: bytes | None = None, check: bool = True) -> bytes | None:
    result = subprocess.run([args.adb, '-s', args.serial, *command], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    if result.returncode:
        if check:
            raise OSError('ADB operation failed; verify the Quest connection and debuggable APK')
        return None
    return result.stdout


def quest_submit(args: argparse.Namespace, request: dict) -> str:
    # Submit and archive share this per-ID lock on the device. Keeping it
    # outside the operation folder makes rename and rechecks one critical section.
    base = f'files/MRFPS/Automation/{args.name}'
    operation = f'{base}/{args.operation_id}'
    archive = f'{base}/archive/{args.operation_id}'
    temporary = f'{base}/.request-{uuid.uuid4().hex}.tmp'
    lock = f'{base}/.lock-{args.operation_id}'
    script = (f'umask 077; mkdir -p {base} || exit 1; mkdir {lock} 2>/dev/null || exit 75; '
              f'cat > {temporary}; result=$?; '
              f'if [ $result -eq 0 ]; then '
              f'if [ -f {archive}/request.json ]; then '
              f'if [ -e {operation} ]; then printf conflict; elif cmp -s {temporary} {archive}/request.json; then printf archived; else printf conflict; fi; '
              f'elif [ -f {operation}/request.json ]; then '
              f'if cmp -s {temporary} {operation}/request.json; then printf active; else printf conflict; fi; '
              f'elif [ -e {operation}/result.json ]; then printf conflict; '
              f'else mkdir -p {operation} && mv {temporary} {operation}/request.json && printf active || result=1; fi; fi; '
              f'rm -f {temporary}; rmdir {lock}; exit $result')
    state = adb(args, ['shell', '-T', f"run-as {PACKAGE} sh -c '{script}'"], data=encode(request)).decode('ascii','strict').strip()
    if state == 'conflict':
        raise ValueError('Quest operation ID already identifies a different or conflicting request')
    if state not in ('active','archived'):
        raise OSError('Quest operation state is invalid')
    return (archive if state == 'archived' else operation) + '/result.json'


def quest_result(args: argparse.Namespace, remote_path: str) -> dict | None:
    data = adb(args, ['shell', 'run-as', PACKAGE, 'cat', remote_path], check=False)
    if data is None:
        # Another machine may have atomically archived the completed request
        # after this caller selected the active result path.
        archived = f'files/MRFPS/Automation/{args.name}/archive/{args.operation_id}/result.json'
        if remote_path != archived:
            data = adb(args, ['shell', 'run-as', PACKAGE, 'cat', archived], check=False)
    if data is None:
        return None
    if len(data) > 16384:
        raise ValueError('Quest result is too large')
    return json.loads(data)


def quest_archive_locked(args: argparse.Namespace, directory: Path, limit: int) -> dict:
    base = f'files/MRFPS/Automation/{args.name}'
    # Distinguish an absent queue from a disconnected or non-debuggable app.
    adb(args,['shell','run-as',PACKAGE,'pwd'])
    listing = adb(args,['shell','run-as',PACKAGE,'ls','-1',base],check=False)
    if listing is None:
        return {'archived':0,'skipped':0,'remote_archived':0}
    remote_moved = skipped = 0
    names = [name.strip() for name in listing.decode('utf-8','strict').splitlines() if valid_id(name.strip())]
    for operation_id in sorted(names):
        if remote_moved >= limit:
            break
        active = f'{base}/{operation_id}'
        request_data = adb(args,['shell','run-as',PACKAGE,'cat',active+'/request.json'],check=False)
        result_data = adb(args,['shell','run-as',PACKAGE,'cat',active+'/result.json'],check=False)
        if not request_data or not result_data or len(request_data)>8192 or len(result_data)>16384:
            skipped += 1; continue
        try:
            request = json.loads(request_data)
            response = json.loads(result_data)
            if request.get('id') != operation_id or response.get('id') != operation_id or response.get('status') not in ('ok','already_applied','error'):
                skipped += 1; continue
        except (ValueError,AttributeError):
            skipped += 1; continue
        lock=f'{base}/.lock-{operation_id}'
        temporary=f'{base}/.result-{uuid.uuid4().hex}.tmp'
        destination=f'{base}/archive/{operation_id}'
        script = (f'umask 077; mkdir {lock} 2>/dev/null || exit 75; cat > {temporary}; result=$?; '
                  f'if [ $result -eq 0 ]; then '
                  f'if [ -e {destination} ]; then printf conflict; '
                  f'elif [ ! -f {active}/request.json ] || ! cmp -s {temporary} {active}/result.json; then printf changed; '
                  f'else mkdir -p {base}/archive && mv {active} {destination} && printf moved || result=1; fi; fi; '
                  f'rm -f {temporary}; rmdir {lock}; exit $result')
        try:
            state = adb(args,['shell','-T',f"run-as {PACKAGE} sh -c '{script}'"],data=result_data).decode('ascii','strict').strip()
        except OSError:
            skipped += 1; continue
        if state == 'moved':
            remote_moved += 1
        else:
            skipped += 1
    # Reconcile cached receipts even if a previous run stopped after the remote
    # atomic rename. Never create local requests for operations from another PC.
    eligible = set()
    for folder in sorted(directory.iterdir(),key=lambda item:item.name):
        if len(eligible) >= limit:
            break
        if not valid_id(folder.name) or folder.is_symlink() or not folder.is_dir():
            continue
        archived = f'{base}/archive/{folder.name}'
        request_data = adb(args,['shell','run-as',PACKAGE,'cat',archived+'/request.json'],check=False)
        result_data = adb(args,['shell','run-as',PACKAGE,'cat',archived+'/result.json'],check=False)
        if not request_data or not result_data or len(request_data)>8192 or len(result_data)>16384:
            continue
        try:
            remote_request, remote_result = json.loads(request_data),json.loads(result_data)
            local_request = json.loads((folder/'request.json').read_text(encoding='utf-8'))
            if remote_request != local_request or remote_result.get('id') != folder.name or remote_result.get('status') not in ('ok','already_applied','error'):
                continue
            local_result = folder/'result.json'
            if local_result.exists():
                if local_result.is_symlink() or json.loads(local_result.read_text(encoding='utf-8')) != remote_result:
                    continue
            else:
                atomic_write(local_result,encode(remote_result))
            eligible.add(folder.name)
        except (ValueError,OSError,AttributeError):
            continue
    result = archive_local_locked(directory,limit,eligible)
    result['remote_archived'] = remote_moved
    result['skipped'] += skipped
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='MRFPS-Client', help='client window name')
    parser.add_argument('--platform', choices=('desktop','quest'), default='desktop')
    parser.add_argument('--serial', default='', help='required for Quest')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--operation-id', help='UUIDv4: reuse the same ID to retry')
    parser.add_argument('--operation', choices=sorted(OPERATIONS))
    parser.add_argument('--archive-completed', action='store_true', help='move completed receipts out of the active scan')
    parser.add_argument('--archive-limit', type=int, default=100, help='maximum receipts moved per run (1..500)')
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
        if args.archive_completed and (args.operation_id or args.operation or args.parameters or args.parameters_file):
            raise ValueError('archive is a separate operation and takes no operation ID or parameters')
        if not args.archive_completed and (not args.operation_id or not valid_id(args.operation_id) or not args.operation):
            raise ValueError('operation ID must be a canonical lowercase UUIDv4')
        if not 1 <= args.archive_limit <= 500:
            raise ValueError('archive-limit must be between 1 and 500')
        if not 0 <= args.wait_seconds <= 60:
            raise ValueError('wait-seconds must be between 0 and 60')
        directory = (root_directory() / ('quest-' + hashlib.sha256(args.serial.encode()).hexdigest()[:24]) / args.name
                     if args.platform == 'quest' else root_directory() / args.name)
        prepare_directory(directory)
        if args.archive_completed:
            with lock_directory(directory):
                result = quest_archive_locked(args,directory,args.archive_limit) if args.platform == 'quest' else archive_local_locked(directory,args.archive_limit)
            print(json.dumps(result))
            return 0
        if args.parameters_file:
            if args.parameters_file.stat().st_size > 8192:
                raise ValueError('parameters file is too large')
            parameter_json = args.parameters_file.read_text(encoding='utf-8-sig')
        else:
            parameter_json = args.parameters or '{}'
        params = json.loads(parameter_json, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('non-finite value')))
        if not isinstance(params, dict) or len(encode(params)) > 4096:
            raise ValueError('parameters must be a JSON object of at most 4096 bytes')
        request = {'version': 1, 'id': args.operation_id, 'operation': args.operation, 'parameters': params}
        result_file = submit(directory, request)
        remote_path = None
        if args.platform == 'quest' and not result_file.exists():
            with lock_directory(directory):
                remote_path = quest_submit(args,request)
        deadline = time.monotonic() + args.wait_seconds
        while time.monotonic() < deadline:
            if not result_file.exists():
                archived_file = directory/'archive'/args.operation_id/'result.json'
                if archived_file.exists(): result_file=archived_file
            if result_file.exists(): break
            if remote_path:
                remote = quest_result(args,remote_path)
                if remote is not None:
                    with lock_directory(directory):
                        active = directory/args.operation_id
                        archived = directory/'archive'/args.operation_id
                        if active.is_dir():
                            result_file = active/'result.json'
                            atomic_write(result_file,encode(remote))
                        elif archived.is_dir():
                            result_file = archived/'result.json'
                        else:
                            raise ValueError('local operation directory disappeared')
                    break
            time.sleep(.1)
        if not result_file.exists():
            archived_file = directory/'archive'/args.operation_id/'result.json'
            if archived_file.exists(): result_file=archived_file
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
