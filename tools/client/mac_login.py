#!/usr/bin/env python3
"""Start a guarded Mac client and queue a device code without exposing it in argv.

Queue receipts describe only local delivery, not authentication or game join.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
import uuid

from device_login import DEFAULT_SERVER, ROLES, control_data, endpoint, read_code

ROOT = Path(__file__).resolve().parents[2]
NAME = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")


def validate_name(value: str) -> str:
    if not NAME.fullmatch(value):
        raise ValueError("invalid client name")
    return value


def operation_id(value: str) -> str:
    try:
        parsed = uuid.UUID(value)
    except ValueError as exc:
        raise ValueError("operation ID must be a UUIDv4") from exc
    if parsed.version != 4 or str(parsed) != value:
        raise ValueError("operation ID must be a canonical lowercase UUIDv4")
    return value


def private_directories() -> tuple[Path, Path]:
    home = Path(os.environ.get("HOME", ""))
    if not home.is_absolute() or not home.is_dir():
        raise ValueError("HOME must name an existing absolute directory")
    support = home / "Library" / "Application Support"
    support.mkdir(parents=True, exist_ok=True)
    try:
        support.resolve().relative_to(home.resolve())
    except ValueError as exc:
        raise ValueError("Application Support is outside HOME") from exc
    current = support
    for part in ("MRFPS", "DeviceLogin"):
        current = current / part
        try:
            current.mkdir(mode=0o700)
        except FileExistsError:
            pass
        info = current.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError("unsafe private directory")
        current.chmod(0o700)
    dirs = []
    for part in ("controls", "state"):
        directory = current / part
        try:
            directory.mkdir(mode=0o700)
        except FileExistsError:
            pass
        info = directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError("unsafe private directory")
        directory.chmod(0o700)
        dirs.append(directory)
    return dirs[0], dirs[1]


def client_directory(base: Path, name: str) -> Path:
    path = base / validate_name(name)
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError("unsafe client state directory")
    path.chmod(0o700)
    return path


@contextmanager
def state_lock(directory: Path):
    import fcntl
    fd = os.open(directory / ".lock", os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise ValueError("unsafe lock file")
        os.fchmod(fd, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("another command is updating this client") from exc
        try:
            yield
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def safe_read(path: Path, limit: int) -> bytes:
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_size > limit:
            raise ValueError("unsafe private file")
        return os.read(fd, limit + 1)
    finally:
        os.close(fd)


def atomic_private(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def save_json(path: Path, value: dict) -> None:
    atomic_private(path, json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf-8"))


def load_json(path: Path) -> dict | None:
    try:
        value = json.loads(safe_read(path, 8192))
    except FileNotFoundError:
        return None
    if not isinstance(value, dict):
        raise ValueError("invalid client state")
    return value


def local_key(directory: Path) -> bytes:
    path = directory / "journal-key"
    try:
        key = safe_read(path, 32)
    except FileNotFoundError:
        key = os.urandom(32)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(key)
            stream.flush()
            os.fsync(stream.fileno())
    if len(key) != 32:
        raise ValueError("invalid journal key")
    return key


def login_is_open(name: str) -> bool:
    command = [sys.executable, str(ROOT / "tools/automation/control.py"), "--name", name,
               "--operation-id", str(uuid.uuid4()), "--operation", "status", "--wait-seconds", "5"]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10, check=False)
    try:
        response = json.loads(result.stdout)
    except (ValueError, UnicodeDecodeError):
        return False
    return result.returncode == 0 and response.get("status") == "ok" and response.get("data", {}).get("login_panel") is True


def queue(args: argparse.Namespace) -> dict:
    name, identifier = validate_name(args.name), operation_id(args.operation_id)
    address = endpoint(args.server)
    code = read_code(args.code_file).decode("ascii")
    controls, base = private_directories()
    directory = client_directory(base, name)
    payload = control_data(address, args.role, code)
    journal = directory / (identifier + ".json")
    target = controls / (name + ".json")
    with state_lock(directory):
        digest = hmac.new(local_key(directory), payload, hashlib.sha256).hexdigest()
        previous = load_json(journal)
        if previous:
            prior_digest = previous.get("digest")
            if not isinstance(prior_digest, str) or not hmac.compare_digest(prior_digest, digest):
                raise ValueError("operation ID already identifies different input")
            prior_state = previous.get("state")
            return {"status": "already_queued" if prior_state == "queued" else "cancelled" if prior_state == "cancelled" else "unknown", "operation_id": identifier}
        if target.exists() or target.is_symlink():
            raise RuntimeError("another login instruction is still queued")
        if not login_is_open(name):
            raise RuntimeError("named client login screen is not available")
        # Record the ambiguous state first: a crash after the next atomic link
        # cannot cause this same single-use code to be submitted a second time.
        save_json(journal, {"version": 1, "digest": digest, "state": "unknown"})
        temporary = controls / (name + "." + identifier + "." + uuid.uuid4().hex + ".tmp")
        try:
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            try:
                os.link(temporary, target)  # atomic and never overwrites another instruction
            except FileExistsError as exc:
                raise RuntimeError("another login instruction is still queued; result is unknown") from exc
            save_json(journal, {"version": 1, "digest": digest, "state": "queued"})
        finally:
            temporary.unlink(missing_ok=True)
    return {"status": "queued", "operation_id": identifier}


def cancel(args: argparse.Namespace) -> dict:
    name, identifier = validate_name(args.name), operation_id(args.operation_id)
    controls, base = private_directories()
    directory = client_directory(base, name)
    path = directory / (identifier + ".json")
    target = controls / (name + ".json")
    with state_lock(directory):
        record = load_json(path)
        if not record:
            return {"status": "not_found", "operation_id": identifier}
        if record.get("state") == "cancelled":
            return {"status": "cancelled", "operation_id": identifier}
        try:
            payload = safe_read(target, 1024)
        except FileNotFoundError:
            # The GUI may have claimed it; neither exchange nor cancellation is
            # known. Do not recreate or claim that a remote exchange stopped.
            return {"status": "unknown", "operation_id": identifier}
        digest = hmac.new(local_key(directory), payload, hashlib.sha256).hexdigest()
        if not isinstance(record.get("digest"), str) or not hmac.compare_digest(record["digest"], digest):
            raise RuntimeError("queued instruction belongs to a different operation")
        try:
            target.unlink()
        except FileNotFoundError:
            return {"status": "unknown", "operation_id": identifier}
        save_json(path, {**record, "state": "cancelled"})
        return {"status": "cancelled", "operation_id": identifier}


def process_info(pid: int, marker: str) -> str | None:
    if not isinstance(pid, int) or pid < 1:
        return None
    try:
        started = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="], text=True, stderr=subprocess.DEVNULL, timeout=5).strip()
        command = subprocess.check_output(["ps", "-ww", "-p", str(pid), "-o", "args="], text=True, stderr=subprocess.DEVNULL, timeout=5).strip()
    except (OSError, subprocess.SubprocessError):
        return None
    token = "-MRFPSInstanceId=" + marker
    return started if started and token in command.split() and "mac_resource_guard.py" in command else None


def marker_exists(marker: str) -> bool:
    # If the guard died unexpectedly but its UE child survived, fail closed.
    try:
        listing = subprocess.check_output(["ps", "-axww", "-o", "args="], text=True, stderr=subprocess.DEVNULL, timeout=5)
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError("cannot verify client process identity") from exc
    token = "-MRFPSInstanceId=" + marker
    return any(token in line.split() for line in listing.splitlines())


def start(args: argparse.Namespace) -> dict:
    name = validate_name(args.name)
    binary = args.executable.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("client executable is not executable")
    _, base = private_directories()
    directory = client_directory(base, name)
    path = directory / "process.json"
    config = {"binary": str(binary), "editor": args.editor}
    with state_lock(directory):
        previous = load_json(path)
        if previous:
            marker = previous.get("instance", "")
            if not re.fullmatch(r"[0-9a-f]{32}", marker):
                raise RuntimeError("invalid previous process state")
            if previous.get("status") == "unknown":
                raise RuntimeError("previous launch outcome unknown; inspect processes before clearing state")
            started = process_info(previous.get("pid", 0), marker)
            if started and started == previous.get("started"):
                if previous.get("config") != config:
                    raise RuntimeError("client name already runs with different settings")
                return {"status": "already_running", "name": name, "guard_pid": previous["pid"]}
            if marker_exists(marker):
                raise RuntimeError("previous client process may still be running")
        marker = uuid.uuid4().hex
        save_json(path, {"status": "unknown", "instance": marker, "config": config})
        command = [str(binary)]
        if args.editor:
            command += [str(ROOT / "MRFPS.uproject"), "-game"]
        command += ["/Game/Maps/MRFPSArena", "-MRFPSDesktop", "-nohmd", "-nosound", "-NoLiveCoding",
                    "-MRFPSWindowName=" + name, "-MRFPSInstanceId=" + marker,
                    "-ExecCmds=t.MaxFPS 60,t.IdleWhenNotForeground 0", "-windowed", "-ResX=1280", "-ResY=720"]
        try:
            child = subprocess.Popen([sys.executable, str(ROOT / "tools/dev/mac_resource_guard.py"), "--", *command],
                                     cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True, close_fds=True)
        except OSError:
            save_json(path, {"status": "failed", "instance": marker, "config": config})
            raise
        time.sleep(.3)
        if child.poll() is not None and not marker_exists(marker):
            save_json(path, {"status": "failed", "instance": marker, "config": config})
            raise RuntimeError("memory guard exited before the client could start")
        started = process_info(child.pid, marker)
        if child.poll() is not None or not started:
            # Keep unknown: a process may have started even when identity could
            # not be read. The operator can inspect before launching again.
            raise RuntimeError("could not verify guarded launch; result is unknown")
        save_json(path, {"status": "running", "instance": marker, "pid": child.pid, "started": started, "config": config})
        return {"status": "launching", "name": name, "guard_pid": child.pid}


def status(args: argparse.Namespace) -> dict:
    name = validate_name(args.name)
    _, base = private_directories()
    directory = client_directory(base, name)
    with state_lock(directory):
        if args.operation_id:
            identifier = operation_id(args.operation_id)
            result = load_json(directory / (identifier + ".json"))
            return {"status": result.get("state", "unknown") if result else "not_found", "operation_id": identifier}
        state = load_json(directory / "process.json")
        if not state:
            return {"status": "not_started", "name": name}
        if state.get("status") == "unknown":
            return {"status": "unknown", "name": name}
        marker = state.get("instance", "")
        started = process_info(state.get("pid", 0), marker)
        if started and started == state.get("started"):
            return {"status": "running", "name": name, "guard_pid": state["pid"]}
        return {"status": "unknown" if marker_exists(marker) else "stopped", "name": name}


def recover(args: argparse.Namespace) -> dict:
    name = validate_name(args.name)
    _, base = private_directories()
    directory = client_directory(base, name)
    path = directory / "process.json"
    with state_lock(directory):
        previous = load_json(path)
        if not previous or previous.get("status") != "unknown":
            return {"status": "nothing_to_recover", "name": name}
        marker = previous.get("instance", "")
        if not re.fullmatch(r"[0-9a-f]{32}", marker) or marker_exists(marker):
            raise RuntimeError("cannot confirm that the previous client has exited")
        save_json(path, {**previous, "status": "failed"})
        return {"status": "recovered", "name": name}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("start")
    p.add_argument("--name", default="MRFPS-Client")
    p.add_argument("--executable", type=Path, required=True)
    p.add_argument("--editor", action="store_true")
    p = sub.add_parser("queue")
    p.add_argument("--name", default="MRFPS-Client")
    p.add_argument("--server", default=DEFAULT_SERVER)
    p.add_argument("--role", choices=ROLES, default="inspector")
    p.add_argument("--code-file", type=Path, required=True)
    p.add_argument("--operation-id", required=True)
    p = sub.add_parser("status")
    p.add_argument("--name", default="MRFPS-Client")
    p.add_argument("--operation-id")
    p = sub.add_parser("recover")
    p.add_argument("--name", default="MRFPS-Client")
    p = sub.add_parser("cancel")
    p.add_argument("--name", default="MRFPS-Client")
    p.add_argument("--operation-id", required=True)
    args = parser.parse_args()
    try:
        if sys.platform != "darwin":
            raise RuntimeError("macOS is required")
        result = start(args) if args.command == "start" else queue(args) if args.command == "queue" else status(args) if args.command == "status" else recover(args) if args.command == "recover" else cancel(args)
        print(json.dumps(result, ensure_ascii=False))
        return 2 if result.get("status") == "unknown" else 0
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(json.dumps({"status": "error", "message": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
