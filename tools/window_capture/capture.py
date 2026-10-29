#!/usr/bin/env python3
"""Capture exactly one identified MRFPS window with a journalled WGC helper."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

import win_identity

MAX_PNG = 64 * 1024 * 1024
HELPER = Path(__file__).with_name("capture_helper.py")


def root_directory() -> Path:
    base = os.environ.get("LOCALAPPDATA", "")
    if os.name != "nt" or not base or not Path(base).is_absolute():
        raise RuntimeError("a Windows LOCALAPPDATA directory is required")
    return Path(base) / "MRFPS" / "WindowCapture"


def reject_reparse(path: Path) -> None:
    try:
        stat = path.lstat()
    except FileNotFoundError:
        return
    if path.is_symlink() or getattr(stat, "st_file_attributes", 0) & 0x400:
        raise RuntimeError("capture directory contains a reparse point")


def restrict_directories(paths: list[Path]) -> None:
    if os.name == "nt":
        script = ("$ErrorActionPreference='Stop'; foreach($p in (ConvertFrom-Json $env:MRFPS_CAPTURE_PATHS)){ "
                  "$d=New-Object System.IO.DirectoryInfo($p); "
                  "if(($d.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'reparse point'}; "
                  "$a=$d.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access); "
                  "$a.SetAccessRuleProtection($true,$false); foreach($x in @($a.Access)){$a.RemoveAccessRuleSpecific($x)}; "
                  "$i=[System.Security.Principal.WindowsIdentity]::GetCurrent().User; "
                  "$r=New-Object System.Security.AccessControl.FileSystemAccessRule($i,'FullControl','ContainerInherit,ObjectInherit','None','Allow'); "
                  "$a.AddAccessRule($r); $d.SetAccessControl($a) }")
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                env={**os.environ, "MRFPS_CAPTURE_PATHS": json.dumps([str(path) for path in paths])},
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if result.returncode:
            raise RuntimeError("could not restrict the capture directory")
    else:
        for path in paths:
            path.chmod(0o700)  # Portable mock tests; production requires Windows.


def prepare_directory(root: Path) -> None:
    if not root.is_absolute():
        raise ValueError("capture directory must be absolute")
    reject_reparse(root.parent)
    reject_reparse(root)
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    reject_reparse(root)
    # No image or journal is written before this DACL is established.
    restrict_directories([root])
    journal = root / "journal"
    reject_reparse(journal)
    journal.mkdir(exist_ok=True, mode=0o700)
    reject_reparse(journal)
    if journal.resolve().parent != root.resolve():
        raise RuntimeError("unsafe journal directory")
    identities = root / "identities"
    reject_reparse(identities)
    identities.mkdir(exist_ok=True, mode=0o700)
    reject_reparse(identities)
    if identities.resolve().parent != root.resolve():
        raise RuntimeError("unsafe identities directory")
    # Pre-existing subdirectories may have inheritance disabled, so restrict
    # both explicitly instead of assuming they inherit from the parent.
    restrict_directories([journal, identities])


@contextmanager
def directory_lock(root: Path):
    path = root / ".capture.lock"
    reject_reparse(path)
    with path.open("a+b") as lock:
        lock.seek(0, os.SEEK_END)
        if not lock.tell():
            lock.write(b"0"); lock.flush()
        lock.seek(0)
        if os.name == "nt":
            import msvcrt
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise RuntimeError("another capture operation is running") from error
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            if os.name == "nt":
                lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def encode(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def atomic_json(path: Path, value: object) -> None:
    reject_reparse(path)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
        with os.fdopen(os.open(temporary, flags, 0o600), "wb") as output:
            output.write(encode(value)); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def read_json(path: Path, limit: int = 16384) -> dict:
    reject_reparse(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise RuntimeError("invalid or oversized state file")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise RuntimeError("invalid state file")
    return value


def valid_id(value: str) -> str:
    try:
        parsed = uuid.UUID(value)
    except ValueError as error:
        raise ValueError("operation ID must be a UUIDv4") from error
    if parsed.version != 4 or str(parsed) != value:
        raise ValueError("operation ID must be a lowercase UUIDv4")
    return value


def marker_for(operation_id: str, prefix: str = "MRFPS-Capture-") -> dict:
    digest = hashlib.sha256(operation_id.encode("ascii")).digest()
    return {"name": prefix + operation_id, "value": (int.from_bytes(digest[:4], "big") & 0x7fffffff) or 1}


def validate_explicit_identity(identity: dict) -> None:
    """Fail before spawning the helper if any exact window target is absent."""
    if not isinstance(identity, dict) or set(identity) != {"process", "main", "target", "window_marker"}:
        raise ValueError("invalid window identity")
    try:
        process, main, target = identity["process"], identity["main"], identity["target"]
        win_identity._positive(process["pid"], "process ID")
        win_identity._positive(process["created"], "process creation time")
        if process["executable"] not in win_identity.CLIENT_BINARIES:
            raise ValueError("invalid client executable")
        for item in (main, target):
            win_identity._positive(item["hwnd"], "HWND")
            win_identity._positive(item["thread"], "window thread")
            if any(not isinstance(item[key], str) or not 0 < len(item[key]) <= 512 for key in ("title", "class")):
                raise ValueError("invalid window metadata")
        if main["hwnd"] != target["hwnd"] and (target["title"] not in win_identity.POPUP_TITLES or main["title"] in win_identity.POPUP_TITLES):
            raise ValueError("invalid MRFPS popout")
        win_identity.validate_marker(identity["window_marker"], "MRFPS-Identity-")
    except (KeyError, TypeError) as error:
        raise ValueError("invalid window identity") from error


def identify(root: Path, identity_id: str, pid: int, main_hwnd: int, target_hwnd: int,
             api: win_identity.Windows, allow_minimized: bool = False) -> dict:
    identity_id = valid_id(identity_id)
    pid = win_identity._positive(pid, "process ID")
    main_hwnd = win_identity._positive(main_hwnd, "main HWND")
    target_hwnd = win_identity._positive(target_hwnd, "target HWND")
    request = {"pid": pid, "main_hwnd": main_hwnd, "target_hwnd": target_hwnd}
    if allow_minimized:
        request["allow_minimized"] = True
    path = root / "identities" / (identity_id + ".json")
    with directory_lock(root):
        if path.exists():
            state = read_json(path)
            if state.get("request") != request:
                raise RuntimeError("identity ID already identifies a different window")
            if state.get("status") == "pending":
                api.unmark(state["identity"], state["identity"]["window_marker"])
                state.update(status="unknown", code="identity_interrupted")
                atomic_json(path, state)
            elif state.get("status") == "ok":
                try:
                    api.validate(state["identity"], allow_minimized=True)
                except Exception:
                    api.unmark(state["identity"], state["identity"]["window_marker"])
                    state.update(status="unknown", code="window_changed")
                    atomic_json(path, state)
            elif state.get("status") not in ("error", "released", "unknown"):
                raise RuntimeError("invalid identity state")
            return state
        snapshot = (api.snapshot(pid, main_hwnd, target_hwnd, allow_minimized=True) if allow_minimized
                    else api.snapshot(pid, main_hwnd, target_hwnd))
        stable = marker_for(identity_id, "MRFPS-Identity-")
        state = {"identity_id": identity_id, "request": request, "status": "pending",
                 "identity": {**snapshot, "window_marker": stable}}
        atomic_json(path, state)
        try:
            state["identity"] = (api.bind(snapshot, stable, allow_minimized=True) if allow_minimized
                                 else api.bind(snapshot, stable))
            state["status"] = "ok"
        except Exception:
            state.update(status="error", code="identity_failed")
        atomic_json(path, state)
        return state


def release(root: Path, identity_id: str, api: win_identity.Windows) -> dict:
    identity_id = valid_id(identity_id)
    path = root / "identities" / (identity_id + ".json")
    with directory_lock(root):
        state = read_json(path)
        if state.get("identity_id") != identity_id:
            raise RuntimeError("invalid identity state")
        if state.get("status") == "released":
            return state
        if state.get("status") not in ("ok", "pending", "unknown", "error"):
            raise RuntimeError("invalid identity state")
        identity = state["identity"]
        stable = identity["window_marker"]
        win_identity.validate_marker(stable, "MRFPS-Identity-")
        api.unmark(identity, stable)
        state.update(status="released")
        state.pop("code", None)
        atomic_json(path, state)
        return state


def inspect_png(path: Path, expected: dict) -> None:
    reject_reparse(path)
    if not path.is_file() or not 57 <= path.stat().st_size <= MAX_PNG:
        raise RuntimeError("capture image is absent or oversized")
    size = 0
    digest = hashlib.sha256()
    with path.open("rb") as source:
        def take(count: int) -> bytes:
            nonlocal size
            block = source.read(count)
            if len(block) != count:
                raise RuntimeError("capture PNG is truncated")
            digest.update(block); size += len(block)
            return block

        if take(8) != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError("capture is not a PNG")
        saw_header = saw_data = ended_data = False
        expanded = 0
        inflater = zlib.decompressobj()
        max_expanded = 0
        while True:
            length = struct.unpack(">I", take(4))[0]
            chunk_type = take(4)
            if length > MAX_PNG or (not saw_header and chunk_type != b"IHDR"):
                raise RuntimeError("invalid capture PNG chunks")
            if chunk_type == b"IHDR":
                if saw_header or length != 13:
                    raise RuntimeError("invalid capture PNG header")
                chunk = take(length)
                width, height, depth, color, compression, filter_method, interlace = struct.unpack(">IIBBBBB", chunk)
                if (width != expected.get("width") or height != expected.get("height")
                        or not 0 < width <= win_identity.MAX_SIDE or not 0 < height <= win_identity.MAX_SIDE
                        or width * height > win_identity.MAX_PIXELS
                        or (depth, color, compression, filter_method, interlace) != (8, 6, 0, 0, 0)):
                    raise RuntimeError("capture dimensions or PNG format do not match")
                max_expanded = height * (width * 4 + 1)
                saw_header = True
                crc = zlib.crc32(chunk, zlib.crc32(chunk_type))
            else:
                if chunk_type == b"IDAT":
                    if ended_data:
                        raise RuntimeError("invalid PNG data order")
                    saw_data = True
                elif saw_data:
                    ended_data = True
                if chunk_type == b"IEND" and (length != 0 or not saw_data):
                    raise RuntimeError("invalid PNG ending")
                crc = zlib.crc32(chunk_type)
                remaining = length
                while remaining:
                    chunk = take(min(remaining, 65536)); remaining -= len(chunk)
                    crc = zlib.crc32(chunk, crc)
                    if chunk_type == b"IDAT":
                        if inflater.eof:
                            raise RuntimeError("extra compressed PNG data")
                        try:
                            while chunk:
                                decoded = inflater.decompress(chunk, 65536)
                                expanded += len(decoded)
                                if expanded > max_expanded or inflater.unused_data:
                                    raise RuntimeError("invalid compressed PNG data")
                                chunk = inflater.unconsumed_tail
                        except zlib.error as error:
                            raise RuntimeError("invalid compressed PNG data") from error
            if struct.unpack(">I", take(4))[0] != crc & 0xffffffff:
                raise RuntimeError("capture PNG checksum failed")
            if chunk_type == b"IEND":
                if not inflater.eof or expanded != max_expanded or source.read(1):
                    raise RuntimeError("capture PNG is incomplete")
                break
    if size != expected.get("size") or digest.hexdigest() != expected.get("sha256"):
        raise RuntimeError("capture image differs from its receipt")


class OwnedJob:
    """Kill only our helper, including if the controller itself crashes."""
    def __init__(self) -> None:
        if os.name != "nt":
            raise RuntimeError("Windows job objects are required")

        class Basic(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                        ("flags", wintypes.DWORD), ("min_working", ctypes.c_size_t), ("max_working", ctypes.c_size_t),
                        ("active_limit", wintypes.DWORD), ("affinity", ctypes.c_size_t),
                        ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]

        class Io(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

        class Extended(ctypes.Structure):
            _fields_ = [("basic", Basic), ("io", Io), ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        k = self.kernel
        k.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]; k.CreateJobObjectW.restype = wintypes.HANDLE
        k.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        k.SetInformationJobObject.restype = wintypes.BOOL
        k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]; k.AssignProcessToJobObject.restype = wintypes.BOOL
        k.CloseHandle.argtypes = [wintypes.HANDLE]; k.CloseHandle.restype = wintypes.BOOL
        self.handle = k.CreateJobObjectW(None, None)
        if not self.handle:
            raise RuntimeError("cannot create helper job")
        limits = Extended(); limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE
        if not k.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.close(); raise RuntimeError("cannot configure helper job")

    def assign(self, process) -> None:
        if not self.kernel.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise RuntimeError("cannot contain capture helper")

    def close(self) -> None:
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def supervise(root: Path, operation_id: str, identity: dict, marker: dict, seconds: int,
              api: win_identity.Windows, memory_check=win_identity.memory_safe, job_factory=OwnedJob) -> dict:
    validate_explicit_identity(identity)
    python = root / "venv" / "Scripts" / "python.exe"
    reject_reparse(root / "venv"); reject_reparse(root / "venv" / "Scripts"); reject_reparse(python)
    if python.resolve().parent != (root / "venv" / "Scripts").resolve():
        raise RuntimeError("unsafe capture environment")
    if not python.is_file():
        raise RuntimeError("isolated capture environment is not installed")
    if not memory_check():
        raise RuntimeError("insufficient memory headroom")
    allowed_env = {"SYSTEMROOT", "WINDIR", "PATH", "LOCALAPPDATA", "TEMP", "TMP", "USERPROFILE", "COMSPEC"}
    env = {key: value for key, value in os.environ.items() if key.upper() in allowed_env}
    env["PYTHONNOUSERSITE"] = "1"
    job = job_factory()
    process = None
    messages: queue.Queue = queue.Queue(maxsize=16)
    started = False
    try:
        process = subprocess.Popen([str(python), "-u", str(HELPER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True, encoding="utf-8", bufsize=1,
                                   env=env, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        # The child waits for stdin; it cannot begin capturing until contained.
        job.assign(process)
        api.validate(identity, marker)
        process.stdin.write(encode({"id": operation_id, "identity": identity, "marker": marker, "root": str(root)}).decode("ascii") + "\n")
        process.stdin.flush(); started = True

        def read_messages():
            try:
                while True:
                    line = process.stdout.readline(8193)
                    if not line:
                        break
                    messages.put(line, timeout=1)
            except Exception:
                pass
            try:
                messages.put(None, timeout=1)
            except queue.Full:
                pass

        threading.Thread(target=read_messages, daemon=True).start()
        deadline = time.monotonic() + seconds
        stage = 0
        while time.monotonic() < deadline:
            if not memory_check():
                raise RuntimeError("memory limit reached during capture")
            try:
                line = messages.get(timeout=min(0.1, max(0.001, deadline - time.monotonic())))
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError("capture helper exited without a completed image")
            if len(line) > 8192:
                raise RuntimeError("invalid capture helper response")
            message = json.loads(line)
            if not isinstance(message, dict):
                raise RuntimeError("invalid capture helper response")
            event = message.get("event")
            if event == "error":
                raise RuntimeError("capture helper could not complete")
            if event in ("frame", "save"):
                if (stage, event) not in ((0, "frame"), (1, "save")) or set(message) != {"event"}:
                    raise RuntimeError("unexpected capture helper stage")
                api.validate(identity, marker)
                process.stdin.write(encode({"ack": event}).decode("ascii") + "\n"); process.stdin.flush()
                stage += 1
            elif event == "saved" and stage == 2:
                if set(message) != {"event", "width", "height", "sha256", "size"}:
                    raise RuntimeError("invalid capture receipt")
                api.validate(identity, marker)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("capture helper did not stop before its deadline")
                    if not memory_check():
                        raise RuntimeError("memory limit reached during capture")
                    try:
                        code = process.wait(timeout=min(0.1, remaining))
                        break
                    except subprocess.TimeoutExpired:
                        continue
                if code != 0:
                    raise RuntimeError("capture helper did not stop cleanly")
                return {key: message[key] for key in ("width", "height", "sha256", "size")}
            else:
                raise RuntimeError("unexpected capture helper response")
        raise TimeoutError("capture helper deadline expired")
    except Exception as error:
        # Once the request reached the helper its state cannot be inferred from
        # an interrupted IPC or timeout, even though no final image is adopted.
        if started:
            raise UnknownCapture() from error
        raise
    finally:
        job.close()  # closes only the job that owns this helper
        if process is not None:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    process.terminate()  # process handle belongs to our Popen
                    process.wait(timeout=3)
                except (OSError, subprocess.TimeoutExpired):
                    pass


class UnknownCapture(RuntimeError):
    pass


def capture(root: Path, operation_id: str, identity: dict, seconds: int, api: win_identity.Windows,
            runner=supervise) -> dict:
    operation_id = valid_id(operation_id)
    validate_explicit_identity(identity)
    if type(seconds) is not int or not 1 <= seconds <= 30:
        raise ValueError("capture timeout must be between 1 and 30 seconds")
    request = {"identity": identity, "timeout_seconds": seconds}
    receipt_path = root / "journal" / (operation_id + ".json")
    image = root / (operation_id + ".png")
    partial = root / (operation_id + ".partial.png")
    marker = marker_for(operation_id)
    with directory_lock(root):
        if receipt_path.exists():
            receipt = read_json(receipt_path)
            if receipt.get("request") != request:
                raise RuntimeError("operation ID already identifies a different request")
            status = receipt.get("status")
            if status == "pending":
                # The previous controller died. Its job is closed on death;
                # never retry or adopt an image from the interrupted attempt.
                api.unmark(identity, marker)
                receipt["status"] = "unknown"; receipt["code"] = "interrupted"
                # No path left by an interrupted attempt is an accepted image.
                for stale in (partial, image):
                    try:
                        reject_reparse(stale)
                        stale.unlink(missing_ok=True)
                    except (OSError, RuntimeError):
                        pass
                atomic_json(receipt_path, receipt)
            elif status == "ok":
                try:
                    inspect_png(image, receipt["image"])
                except Exception:
                    receipt["status"] = "unknown"; receipt["code"] = "result_missing_or_modified"
                    atomic_json(receipt_path, receipt)
            elif status not in ("error", "unknown"):
                raise RuntimeError("invalid journal state")
            return receipt
        reject_reparse(image); reject_reparse(partial)
        if image.exists() or partial.exists():
            raise RuntimeError("capture output already exists without a journal")
        api.validate(identity)
        receipt = {"id": operation_id, "request": request, "status": "pending"}
        atomic_json(receipt_path, receipt)
        marked = False
        try:
            api.mark(identity, marker); marked = True
            result = runner(root, operation_id, identity, marker, seconds, api)
            api.validate(identity, marker)
            inspect_png(partial, result)
            # The final adoption is a separate parent-side identity check.
            api.validate(identity, marker)
            if image.exists() or image.is_symlink():
                raise RuntimeError("final image unexpectedly exists")
            os.replace(partial, image)
            receipt.update(status="ok", image=result, image_path=str(image))
        except UnknownCapture:
            receipt.update(status="unknown", code="capture_interrupted")
        except Exception:
            receipt.update(status="error", code="capture_failed")
        finally:
            if marked:
                api.unmark(identity, marker)
            try:
                reject_reparse(partial)
                partial.unlink(missing_ok=True)
            except (OSError, RuntimeError):
                pass
        atomic_json(receipt_path, receipt)
        return receipt


def parse_int(value: str) -> int:
    try:
        return win_identity._positive(int(value, 0), "identifier")
    except ValueError as error:
        raise argparse.ArgumentTypeError("identifier must be a positive decimal or 0x integer") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("init", help="create the restricted capture directory")
    listing = commands.add_parser("list", help="list window metadata for one client PID")
    listing.add_argument("--pid", type=parse_int, required=True)
    identify_parser = commands.add_parser("identify", help="inspect explicitly supplied main and target HWNDs")
    identify_parser.add_argument("--identity-id", required=True)
    identify_parser.add_argument("--pid", type=parse_int, required=True)
    identify_parser.add_argument("--main-hwnd", type=parse_int, required=True)
    identify_parser.add_argument("--target-hwnd", type=parse_int, required=True)
    identify_parser.add_argument("--allow-minimized", action="store_true",
                                 help="allow identifying a minimized window for state operations, not capture")
    release_parser = commands.add_parser("release", help="remove this identification's window properties")
    release_parser.add_argument("--identity-id", required=True)
    shot = commands.add_parser("capture", help="capture the previously identified window once")
    shot.add_argument("--identity-id", required=True)
    shot.add_argument("--id", required=True)
    shot.add_argument("--timeout-seconds", type=int, default=15)
    args = parser.parse_args()
    try:
        if args.command == "list":
            value = win_identity.Windows().list_windows(args.pid)
        else:
            root = root_directory(); prepare_directory(root)
            if args.command == "init":
                value = {"status": "ok", "directory": str(root)}
            elif args.command == "identify":
                value = identify(root, args.identity_id, args.pid, args.main_hwnd, args.target_hwnd,
                                 win_identity.Windows(), allow_minimized=args.allow_minimized)
            elif args.command == "release":
                value = release(root, args.identity_id, win_identity.Windows())
            else:
                identity_id = valid_id(args.identity_id)
                identity_state = read_json(root / "identities" / (identity_id + ".json"))
                if identity_state.get("identity_id") != identity_id or identity_state.get("status") not in ("ok", "released"):
                    raise RuntimeError("window identification is not available")
                value = capture(root, args.id, identity_state["identity"], args.timeout_seconds, win_identity.Windows())
        print(json.dumps(value, ensure_ascii=True, separators=(",", ":")))
        return 0 if value.get("status", "ok") in ("ok", "released") else 1
    except Exception as error:
        print(json.dumps({"status": "error", "code": str(error)}, ensure_ascii=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
