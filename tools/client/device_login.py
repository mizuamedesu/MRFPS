#!/usr/bin/env python3
"""Idempotent, credential-quiet entry points for the device-login workflow.

Windows: launch a named standalone client once with a code file.
Quest: provision a development APK through adb stdin and launch its login once.
To deliberately start a new session, use --replace; never embed a code in argv.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = "org.mrfps.arena"
ROLES = ("player", "inspector", "admin")
DEFAULT_SERVER = "mrfps.mizuame.app:7777"


def endpoint(value: str) -> str:
    if not re.fullmatch(r"(?:[A-Za-z0-9.-]+|\[[0-9A-Fa-f:]+\])(?::[0-9]{1,5})?", value):
        raise ValueError("invalid endpoint")
    if value.startswith("["):
        tail = value[value.index("]") + 1:]
    else:
        tail = value[value.find(":"):] if ":" in value else ""
    if tail and not 1 <= int(tail[1:]) <= 65535:
        raise ValueError("invalid port")
    return value


def test_auth_url(value: str) -> bool:
    match=re.fullmatch(r"http://(?:127\.0\.0\.1|localhost):([0-9]{1,5})(?:/[A-Za-z0-9_./-]*)?",value)
    return bool(match and 1<=int(match.group(1))<=65535)


def quest_glove_config(args: argparse.Namespace) -> dict | None:
    """Validate an explicitly pinned PC relay before using it in an adb intent."""
    sender = getattr(args, "glove_sender", None)
    device = getattr(args, "glove_device", None)
    port = getattr(args, "glove_port", None)
    if sender is None:
        if device is not None or port is not None:
            raise ValueError("glove device and port require --glove-sender")
        return None
    if args.role != "player":
        raise ValueError("glove relay is available only for a player")
    try:
        address = ipaddress.IPv4Address(sender)
    except (ipaddress.AddressValueError, TypeError):
        raise ValueError("glove sender must be a single IPv4 address") from None
    if address.is_unspecified or address.is_multicast or int(address) == 0xffffffff:
        raise ValueError("glove sender must be a unicast IPv4 address")
    if device is not None and not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", device):
        raise ValueError("invalid glove device identifier")
    if port is None:
        port = 25788
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError("invalid glove relay port")
    return {"sender": str(address), "port": port, "device": device or ""}


def validate_code(data: bytes) -> bytes:
    if len(data) > 64:
        raise ValueError("code file must contain exactly six ASCII digits")
    data = data.strip()
    if not re.fullmatch(rb"[0-9]{6}", data):
        raise ValueError("code file must contain exactly six ASCII digits")
    return data


def read_code(path: Path) -> bytes:
    with path.open("rb") as source:
        return validate_code(source.read(65))


def adb_result(adb: str, serial: str, args: list[str], *, data: bytes | None = None) -> subprocess.CompletedProcess:
    command = [adb] + (["-s", serial] if serial else []) + args
    return subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)


def run_adb(adb: str, serial: str, args: list[str], *, data: bytes | None = None, check: bool = True) -> str:
    result = adb_result(adb, serial, args, data=data)
    if check and result.returncode:
        raise RuntimeError(f"adb operation failed (exit {result.returncode}); contents omitted")
    return result.stdout.decode("utf-8", "replace").strip()


def quest_pid(adb: str, serial: str) -> str:
    # pidof returns 1 with no output when the process is absent. Do not mistake
    # a disconnected device, a missing command or malformed output for absence.
    result = adb_result(adb, serial, ["shell", "pidof", PACKAGE])
    value = result.stdout.decode("utf-8", "replace").strip()
    error = result.stderr.decode("utf-8", "replace").strip()
    if result.returncode == 1 and not value and not error:
        return ""
    if result.returncode == 0 and not error and re.fullmatch(r"[0-9]+(?:\s+[0-9]+)*", value):
        return value
    raise RuntimeError("Cannot reliably inspect the Quest app process; no dialog was closed")


_QUEST_ROOT = re.compile(r"(?m)^  \* Task\{[^\r\n]*#(?P<id>\d+)[^\r\n]*\r?$")
_QUEST_PERMISSION_ACTIVITY = re.compile(
    r"(?:MediaProjection(?:Permission|Consent)|ScreenCapture(?:Permission|Consent)|GrantPermissionsActivity)", re.IGNORECASE)


def assert_quest_share_clear(adb: str, serial: str) -> None:
    """Observe only: never accept, reject, close, or remove an OS permission task."""
    activity_dump = run_adb(adb, serial, ["shell", "dumpsys", "activity", "activities"])
    if _QUEST_PERMISSION_ACTIVITY.search(activity_dump):
        raise RuntimeError("Quest may have a screen-sharing or privacy permission prompt; handle it on the headset before retrying")
    roots = list(_QUEST_ROOT.finditer(activity_dump))
    if not roots:
        raise RuntimeError("Cannot reliably inspect Quest activity roots; no permission prompt was changed")
    root_ids = [int(root.group("id")) for root in roots]
    if len(set(root_ids)) != len(root_ids) or any(task > 2147483647 for task in root_ids):
        raise RuntimeError("Quest activity roots are ambiguous; no permission prompt was changed")


def load_state(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError, OSError):
        return {}


def load_submission_state(path: Path, platform: str) -> dict:
    """Never discard an unreadable journal and accidentally reuse a code."""
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}
    except (ValueError, OSError) as exc:
        raise RuntimeError(platform + " submission journal is unreadable; no code was sent") from exc
    if not isinstance(state, dict):
        raise RuntimeError(platform + " submission journal is invalid; no code was sent")
    return state


def load_quest_state(path: Path) -> dict:
    return load_submission_state(path, "Quest")


def submission_history(state: dict, platform: str) -> list[dict]:
    """Migrate legacy last-submission records and retain every attempted marker."""
    history = state.get("submission_history", [])
    if not isinstance(history, list):
        raise RuntimeError(platform + " submission journal is invalid; no code was sent")
    history = list(history)
    if any(entry is None for entry in history):
        raise RuntimeError(platform + " submission journal is invalid; no code was sent")
    for entry in history + [state.get("submitted"), state.get("submission_inflight")]:
        if entry is None:
            continue
        if (not isinstance(entry, dict) or set(entry) != {"path", "modified_ns"} or
                not isinstance(entry["path"], str) or not entry["path"] or
                type(entry["modified_ns"]) is not int or entry["modified_ns"] < 0):
            raise RuntimeError(platform + " submission journal is invalid; no code was sent")
    # Previously only the most recent successful submission was recorded.
    old = state.get("submitted")
    if old is not None and old not in history:
        history.append(old)
    inflight = state.get("submission_inflight")
    if inflight is not None and inflight not in history:
        raise RuntimeError(platform + " submission journal is inconsistent; no code was sent")
    if old is not None:
        state.setdefault("submitted_config", state.get("config"))
        state.setdefault("submitted_pid", state.get("pid"))
        if platform == "Windows":
            state.setdefault("submitted_created", state.get("created"))
    state["submission_history"] = history
    return history


def quest_submission_history(state: dict) -> list[dict]:
    return submission_history(state, "Quest")


def save_state(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(value), encoding="utf-8")
    tmp.replace(path)


@contextmanager
def state_lock(path: Path):
    """A crash releases the OS lock; simultaneous invocations cannot launch twice."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.with_suffix(".lock").open("a+b") as lock:
        lock.seek(0, os.SEEK_END)
        if not lock.tell():
            lock.write(b"0")
            lock.flush()
        lock.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError("another command is updating this client") from exc
        try:
            yield
        finally:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def submission(code_file: Path | None) -> tuple[dict | None, str | None]:
    if code_file is None:
        return None, None
    resolved = code_file.resolve(strict=True)
    # Read the bytes and identity from one open handle. The issuer may replace
    # its code file atomically while a launcher is reading; never associate a
    # newer code with an older marker or silently submit it again on retry.
    with resolved.open("rb") as source:
        before = os.fstat(source.fileno())
        data = source.read(65)
        after = os.fstat(source.fileno())
        current = resolved.stat()
        # Verify the same handle again after looking up the path; a rewrite
        # that restores mtime/size must not evade the consistency check.
        source.seek(0)
        confirmed = source.read(65)
        final = os.fstat(source.fileno())
    def identity(info: os.stat_result) -> tuple[int, int, int, int, int]:
        return (info.st_dev, info.st_ino, info.st_mtime_ns, info.st_ctime_ns, info.st_size)
    def path_identity(info: os.stat_result) -> tuple[int, int, int, int]:
        return (info.st_dev, info.st_ino, info.st_mtime_ns, info.st_size)
    # CPython on Windows may expose change time through fstat().st_ctime_ns
    # but creation time through stat().st_ctime_ns. Compare ctime between
    # observations of the same handle, and compare the stable path identity
    # separately. POSIX still requires the path's ctime to agree as well.
    same_path = path_identity(final) == path_identity(current)
    if os.name != "nt": same_path = same_path and final.st_ctime_ns == current.st_ctime_ns
    elif hasattr(final, "st_birthtime_ns") and hasattr(current, "st_birthtime_ns"):
        same_path = same_path and final.st_birthtime_ns == current.st_birthtime_ns
    if data != confirmed or identity(before) != identity(after) or identity(after) != identity(final) or not same_path:
        raise RuntimeError("code file changed while reading; no code was sent")
    marker = {"path": str(resolved), "modified_ns": after.st_mtime_ns}
    return marker, validate_code(data).decode("ascii")


def control_data(address: str, role: str, code: str | None) -> bytes:
    body = {"server": address, "role": role}
    if code is not None:
        body["code"] = code
    return json.dumps(body, separators=(",", ":")).encode("utf-8")


def quest_control(args: argparse.Namespace, address: str, code: str | None) -> None:
    root = run_adb(args.adb, args.serial, ["shell", "run-as", PACKAGE, "pwd"])
    if not re.fullmatch(r"/data/(?:user/[0-9]+|data)/" + re.escape(PACKAGE), root):
        raise RuntimeError("Quest development APK must support run-as")
    run_adb(args.adb, args.serial, ["shell", f"run-as {PACKAGE} sh -c 'umask 077; mkdir -p files/MRFPS && chmod 700 files/MRFPS'"])
    command = f"run-as {PACKAGE} sh -c 'umask 077; cat > files/MRFPS/device-control.tmp && chmod 600 files/MRFPS/device-control.tmp && mv files/MRFPS/device-control.tmp files/MRFPS/device-control.json'"
    run_adb(args.adb, args.serial, ["shell", "-T", command], data=control_data(address, args.role, code))


def clear_quest_control(args: argparse.Namespace) -> None:
    root = run_adb(args.adb, args.serial, ["shell", "run-as", PACKAGE, "pwd"])
    if not re.fullmatch(r"/data/(?:user/[0-9]+|data)/" + re.escape(PACKAGE), root):
        raise RuntimeError("Quest development APK must support run-as")
    # Called only while this package is stopped. Never remove other app data.
    run_adb(args.adb, args.serial, ["shell", f"run-as {PACKAGE} rm -f files/MRFPS/device-control.json files/MRFPS/device-control.tmp"])


def quest(args: argparse.Namespace) -> dict:
    address = endpoint(args.server)
    serial = args.serial
    state_path = quest_state_path(serial)
    with state_lock(state_path):
        result = quest_locked(args, address, state_path)
        if getattr(args, "show_settings", False):
            # The app owns the saved address; no control message is sent here.
            # It may already have advanced since launch, so don't assert a page.
            result.update(serverArgumentApplied=False, pageConfirmed=False)
        return result


def quest_state_path(serial: str) -> Path:
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", serial):
        raise ValueError("invalid serial")
    return ROOT / "Saved" / "DeviceAuth" / ("quest-" + serial + ".json")


def stop_quest(args: argparse.Namespace) -> dict:
    """Stop only this package on the specified device; repeated calls are safe."""
    state_path = quest_state_path(args.serial)
    with state_lock(state_path):
        previous = load_state(state_path)
        was_running = bool(quest_pid(args.adb, args.serial))
        was_pending = bool(previous.get("pending"))
        assert_quest_share_clear(args.adb, args.serial)
        # Even without a PID, an Activity launch may still be queued by Android.
        # force-stop is limited to the fixed app package and specified serial.
        run_adb(args.adb, args.serial, ["shell", "am", "force-stop", PACKAGE])
        for _ in range(8):
            if not quest_pid(args.adb, args.serial):
                break
            time.sleep(.25)
        else:
            raise RuntimeError("Quest app still has a process after force-stop")
        if previous and (previous.get("pid") is not None or previous.get("pending")):
            previous["pid"] = None
            previous["pending"] = False
            save_state(state_path, previous)
        return {"status": "stopped" if was_running or was_pending else "already_stopped", "serial": args.serial, "package": PACKAGE}


def quest_locked(args: argparse.Namespace, address: str, state_path: Path) -> dict:
    serial = args.serial
    show_settings = getattr(args, "show_settings", False)
    if show_settings and args.code_file is not None:
        raise ValueError("--show-settings cannot include a code")
    requested_glove = quest_glove_config(args)
    marker, code = submission(args.code_file)
    current_pid = quest_pid(args.adb, serial)
    previous = load_quest_state(state_path)
    history = quest_submission_history(previous)
    previous_pids = set(str(previous.get("pid") or "").split())
    same_process = bool(previous_pids.intersection(current_pid.split()))
    known = same_process or previous.get("pending") or getattr(args, "adopt", False)
    previous_config = previous.get("config", {})
    if not isinstance(previous_config, dict):
        raise RuntimeError("Quest configuration journal is invalid; no code was sent")
    assert_quest_share_clear(args.adb, serial)
    prior_glove = previous_config.get("glove")
    if current_pid and known and not args.replace and requested_glove is not None and requested_glove != prior_glove:
        raise RuntimeError("Quest glove relay is a launch setting; use --replace to change it")
    # A later code-only submission must not silently discard the launch pin.
    glove = prior_glove if requested_glove is None and current_pid and same_process and not args.replace else requested_glove
    wanted = {"server": address, "role": args.role}
    if glove is not None:
        wanted["glove"] = glove
    if marker is not None and marker in history:
        submitted_pids = set(str(previous.get("submitted_pid") or "").split())
        launched_pending = bool(previous.get("pending") and not previous_pids and not submitted_pids)
        if (current_pid and known and not args.replace and marker == previous.get("submitted") and
                not previous.get("submission_inflight") and previous.get("config") == wanted and
                previous.get("submitted_config") == wanted and
                (launched_pending or submitted_pids.intersection(current_pid.split()))):
            if launched_pending:
                previous["pid"] = current_pid
                previous["pending"] = False
                previous["submitted_pid"] = current_pid
                save_state(state_path, previous)
            return {"status": "already_running", "pid": current_pid, "config": wanted}
        raise RuntimeError("Quest code was already submitted or its outcome is unknown; use a new code")

    def submit_control(pid: str | None) -> None:
        if marker is not None:
            # Reserve before any ADB command can publish the code. An interrupted
            # submission is unknown and must never be silently retried.
            if len(history) >= 16384:
                raise RuntimeError("Quest submission journal is full; no code was sent")
            history.append(marker)
            previous["submission_inflight"] = marker
            save_state(state_path, previous)
        quest_control(args, address, code)
        previous["config"] = wanted
        previous["configured"] = True
        if marker is not None:
            previous["submitted"] = marker
            previous["submitted_config"] = wanted
            previous["submitted_pid"] = pid
            previous["submission_inflight"] = None
        save_state(state_path, previous)

    if current_pid and known and not args.replace:
        if not same_process:
            previous["pid"] = current_pid
            previous["pending"] = False
            save_state(state_path, previous)
        if show_settings:
            if previous.get("config") != wanted or previous.get("configured", True):
                raise RuntimeError("settings page requires a new instance; use --replace")
            return {"status": "already_running", "pid": current_pid, "config": wanted}
        if previous.get("config") == wanted and previous.get("configured", True) and marker is None:
            return {"status": "already_running", "pid": current_pid, "config": wanted}
        submit_control(current_pid)
        return {"status": "login_queued" if marker else "settings_queued", "pid": current_pid, "config": wanted}
    if current_pid and same_process and previous.get("config") == wanted and previous.get("configured", True) and marker is None and not show_settings:
        return {"status": "already_running", "pid": current_pid, "config": wanted}
    if current_pid and not args.replace:
        raise RuntimeError("Quest app is running with unknown or different settings; use --replace to restart this app")
    if current_pid:
        run_adb(args.adb, serial, ["shell", "am", "force-stop", PACKAGE])
        for _ in range(8):
            if not quest_pid(args.adb, serial):
                break
            time.sleep(.25)
        else:
            raise RuntimeError("Quest app still has a process after force-stop; no dialog was closed")
    assert_quest_share_clear(args.adb, serial)
    if quest_pid(args.adb, serial):
        raise RuntimeError("Quest app restarted before launch; no dialog was closed")
    if show_settings:
        clear_quest_control(args)
        previous["config"] = wanted
        previous["configured"] = False
        save_state(state_path, previous)
    else:
        submit_control(None)
    if quest_pid(args.adb, serial):
        raise RuntimeError("Quest app restarted before launch")
    cmdline = "/Game/Maps/MRFPSArena -vr -log"
    if glove is not None:
        cmdline += f" -MRFPSGloveSender={glove['sender']} -MRFPSGlovePort={glove['port']}"
        if glove["device"]:
            cmdline += f" -MRFPSGloveDevice={glove['device']}"
    # Control-file provisioning can take several ADB round trips. Check again
    # immediately before journalling the launch; never dismiss an OS prompt.
    assert_quest_share_clear(args.adb, serial)
    previous["pid"] = None
    previous["pending"] = True
    save_state(state_path, previous)
    run_adb(args.adb, serial, ["shell", "am", "start", "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-n", PACKAGE + "/com.epicgames.unreal.GameActivity", "--es", "cmdline", "'" + cmdline + "'"])
    current_pid = ""
    for _ in range(12):
        current_pid = quest_pid(args.adb, serial)
        if current_pid:
            break
        time.sleep(.25)
    if current_pid:
        previous["pid"] = current_pid
        previous["pending"] = False
        if marker is not None:
            previous["submitted_pid"] = current_pid
        save_state(state_path, previous)
    return {"status": "launched" if current_pid else "launch_pending", "pid": current_pid or None, "config": wanted}


def windows_control(name: str, address: str, role: str, code: str | None) -> None:
    local = os.environ.get("LOCALAPPDATA", "")
    if not local or not Path(local).is_absolute():
        raise RuntimeError("LOCALAPPDATA is required for the private control channel")
    folder = Path(local) / "MRFPS" / "DeviceLogin" / "controls"
    folder.mkdir(parents=True, exist_ok=True)
    who = subprocess.run(["whoami.exe", "/user", "/fo", "csv", "/nh"], check=True, capture_output=True, text=True, timeout=10)
    match = re.search(r"S-1-(?:\d+-)+\d+", who.stdout)
    if not match:
        raise RuntimeError("cannot identify Windows user for private control ACL")
    subprocess.run(["icacls.exe", str(folder), "/inheritance:r", "/grant:r", f"*{match.group(0)}:(OI)(CI)F"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    temporary = folder / (name + "." + str(uuid.uuid4()) + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(control_data(address, role, code))
        os.replace(temporary, folder / (name + ".json"))
    finally:
        temporary.unlink(missing_ok=True)


def windows(args: argparse.Namespace) -> dict:
    if os.name != "nt":
        raise RuntimeError("windows launch requires Windows")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", args.name):
        raise ValueError("invalid window name")
    state_path = ROOT / "Saved" / "DeviceAuth" / ("windows-" + args.name + ".json")
    with state_lock(state_path):
        return windows_locked(args, state_path)


def windows_locked(args: argparse.Namespace, state_path: Path) -> dict:
    import ctypes
    from ctypes import wintypes
    address = endpoint(args.server)
    if args.show_settings and args.code_file is not None:
        raise ValueError("--show-settings cannot include a code")
    if args.auth_test_url and not test_auth_url(args.auth_test_url):
        raise ValueError("test auth URL must be a localhost HTTP URL")
    binary = args.executable.resolve(strict=True)
    wanted = {"server": address, "role": args.role, "name": args.name, "auth_test_url": args.auth_test_url or "",
              "binary":str(binary), "editor":args.editor}
    marker, code = submission(args.code_file)
    expected_marker = getattr(args, "expected_marker", None)
    expected_code = getattr(args, "expected_code", None)
    before_submit = getattr(args, "before_submit", None)
    if expected_marker is not None or expected_code is not None:
        if not isinstance(expected_marker,dict) or not isinstance(expected_code,str) or marker != expected_marker or code != expected_code:
            raise RuntimeError("issued code changed before submission; no code was sent")
    if before_submit is not None and not callable(before_submit):
        raise ValueError("invalid submission validator")
    old = load_submission_state(state_path, "Windows")
    history = submission_history(old, "Windows")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetExitCodeProcess.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.TerminateProcess.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    def creation_time(pid: int) -> int | None:
        ctypes.set_last_error(0)
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            if ctypes.get_last_error() == 87:  # ERROR_INVALID_PARAMETER: no such PID.
                return None
            raise RuntimeError("cannot inspect the Windows guard process")
        try:
            exit_code = wintypes.DWORD()
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                raise RuntimeError("cannot inspect the Windows guard process")
            if exit_code.value != 259:
                return None
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
                raise RuntimeError("cannot verify the Windows guard identity")
            return (created.dwHighDateTime << 32) | created.dwLowDateTime
        finally:
            kernel.CloseHandle(handle)
    pid = old.get("pid") or 0
    if type(pid) is not int or pid < 0:
        raise RuntimeError("Windows guard state is invalid; no client was launched")
    if pid and (type(old.get("created")) is not int or old["created"] <= 0):
        raise RuntimeError("Windows guard identity is incomplete; no client was launched")
    running_creation = creation_time(pid) if pid else None
    if running_creation is not None and running_creation != old.get("created"):
        raise RuntimeError("Windows guard process identity changed; no client was launched")
    active = running_creation is not None
    if old.get("launch_pending"):
        raise RuntimeError("previous Windows launch outcome is unknown; no client was launched")
    prior = old.get("config", {})
    if not isinstance(prior, dict):
        raise RuntimeError("Windows guard state is invalid; no client was launched")
    same_binary = all(prior.get(key) == wanted.get(key) for key in ("name", "auth_test_url", "binary", "editor"))
    if marker is not None and marker in history:
        if (active and not args.replace and old.get("configured", True) and old.get("config") == wanted and
                old.get("submitted") == marker and not old.get("submission_inflight") and
                old.get("submitted_config") == wanted and old.get("submitted_pid") == pid and
                old.get("submitted_created") == running_creation):
            return {"status": "already_running", "guard_pid": pid, "config": wanted}
        raise RuntimeError("Windows code was already submitted or its outcome is unknown; use a new code")

    def submit_control(guard_pid: int | None, guard_created: int | None) -> None:
        # An internal caller can validate an issuer snapshot immediately before
        # publication while it holds the issuer's cross-process lock.
        if before_submit is not None:
            before_submit()
        if marker is not None:
            if len(history) >= 16384:
                raise RuntimeError("Windows submission journal is full; no code was sent")
            # Persist the intent before the private control file can be visible.
            # An interruption at any later point must not send this code again.
            history.append(marker)
            old["submission_inflight"] = marker
            save_state(state_path, old)
        windows_control(args.name, address, args.role, code)
        old["config"] = wanted
        old["configured"] = True
        if marker is not None:
            old["submitted"] = marker
            old["submitted_config"] = wanted
            old["submitted_pid"] = guard_pid
            old["submitted_created"] = guard_created
            old["submission_inflight"] = None
        save_state(state_path, old)

    if active and not args.replace:
        if args.show_settings:
            if old.get("config") != wanted or old.get("configured", True):
                raise RuntimeError("settings page requires a new instance; use --replace")
            return {"status": "already_running", "guard_pid": pid, "config": wanted}
        if not same_binary:
            raise RuntimeError("a client with this name already runs with different settings; use --replace")
        if old.get("config") == wanted and old.get("configured", True) and marker is None:
            return {"status": "already_running", "guard_pid": pid, "config": wanted}
        submit_control(pid, running_creation)
        return {"status": "login_queued" if marker else "settings_queued", "guard_pid": pid, "config": wanted}
    if active:
        if not args.replace:
            raise RuntimeError("a client with this name already runs with different settings; use --replace")
        # Open and check the same creation time again before touching this one
        # owned process. A reused PID must never cause an unrelated app to exit.
        handle = kernel.OpenProcess(0x1000 | 0x0001 | 0x00100000, False, pid)
        if not handle:
            raise RuntimeError("cannot open the owned client for replacement")
        try:
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)) or ((created.dwHighDateTime << 32) | created.dwLowDateTime) != old.get("created"):
                raise RuntimeError("client process identity changed")
            if not kernel.TerminateProcess(handle, 0):
                raise RuntimeError("cannot stop the owned client")
            if kernel.WaitForSingleObject(handle, 10000) != 0:
                raise RuntimeError("owned client guard did not exit in time")
        finally:
            kernel.CloseHandle(handle)
    if args.show_settings:
        local = os.environ.get("LOCALAPPDATA", "")
        if not local or not Path(local).is_absolute(): raise RuntimeError("LOCALAPPDATA is required")
        # This name belongs to a non-running guarded instance at this point.
        # Remove only its stale command so startup remains on server settings.
        (Path(local)/"MRFPS"/"DeviceLogin"/"controls"/(args.name+".json")).unlink(missing_ok=True)
    else:
        submit_control(None, None)
    run_id = uuid.uuid4().hex
    report = ROOT / "Saved" / "Validation" / "DeviceLogin" / args.name / run_id / "report.json"
    launch_config = state_path.parent / ("windows-" + args.name + "-launch-" + run_id + ".json")
    save_state(launch_config, {"binary":str(binary), "editor":args.editor, "name":args.name,
                               "test_url":args.auth_test_url or "", "report":str(report), "python":sys.executable})
    guard = ROOT / "Scripts" / "Start-DeviceLoginGuarded.ps1"
    # Save before spawning: a crash between Popen and process identity capture
    # must not produce a second, untracked client on retry.
    old.update({"pid":None, "created":None, "config":wanted, "launch_pending":True,
                "configured":not args.show_settings, "report":str(report), "launch_config":str(launch_config)})
    save_state(state_path, old)
    # The guard owns a Windows Job Object and monitors system memory; no UE
    # process can escape the 10 GB / 75% constraints used for the build.
    process = subprocess.Popen(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(guard), "-ConfigPath", str(launch_config)],
                               cwd=ROOT, close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    created = creation_time(process.pid)
    if not created:
        raise RuntimeError("unable to verify client process identity")
    old.update({"pid":process.pid, "created":created, "launch_pending":False})
    if marker is not None:
        old["submitted_pid"] = process.pid
        old["submitted_created"] = created
    save_state(state_path, old)
    return {"status": "launching", "guard_pid": process.pid, "report":str(report), "config": wanted}


def reset(args: argparse.Namespace) -> dict:
    if args.platform == "windows":
        local = os.environ.get("LOCALAPPDATA", "")
        if not local or not Path(local).is_absolute():
            raise RuntimeError("LOCALAPPDATA is required")
        for name in ("issuer-public.pem", "access.jwt"):
            (Path(local) / "MRFPS" / "Auth" / name).unlink(missing_ok=True)
    else:
        if args.serial and not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", args.serial):
            raise ValueError("invalid serial")
        run_adb(args.adb, args.serial, ["shell", "run-as", PACKAGE, "rm", "-f", "files/MRFPS/issuer-public.pem", "files/MRFPS/access.jwt"])
    return {"status": "trust_cleared", "platform": args.platform, "restart_required_if_running": True}


def status(args: argparse.Namespace) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", args.name):
        raise ValueError("invalid window name")
    state = load_submission_state(ROOT / "Saved" / "DeviceAuth" / ("windows-" + args.name + ".json"), "Windows")
    if not state:
        return {"status": "not_started", "name": args.name}
    if state.get("launch_pending"):
        return {"status":"launch_unknown", "name":args.name,
                "guard_pid":state.get("pid"), "guard_report":state.get("report"),
                "launch_config":state.get("launch_config"), "events":[]}
    report_path = Path(state.get("report", ""))
    report = load_state(report_path) if report_path.is_file() else {}
    log = report_path.parent / "engine.log"
    events = []
    if log.is_file():
        # Emit only fixed, credential-free product event tokens; never copy a
        # free-form engine, HTTP, request, or server line into command output.
        pattern = re.compile(r"MRFPS_DEVICE_(?:CONTROL role=(?:player|inspector|admin) has_code=[01]|EXCHANGE_START role=(?:player|inspector|admin)|EXCHANGE_VERIFIED role=(?:player|inspector|admin)|EXCHANGE_ERROR code=[A-Z0-9_]+|CONNECT role=(?:player|inspector|admin))")
        with log.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = pattern.search(line)
                if match:
                    events.append(match.group(0))
        events = events[-15:]
    return {"status":report.get("status", "initializing"), "name":args.name,
            "guard_pid":state.get("pid"), "guard_report":str(report_path), "events":events}


def stop_windows(args: argparse.Namespace) -> dict:
    if os.name != "nt" or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", args.name):
        raise ValueError("Windows and a valid window name are required")
    import ctypes
    from ctypes import wintypes
    path = ROOT / "Saved" / "DeviceAuth" / ("windows-" + args.name + ".json")
    with state_lock(path):
        state = load_submission_state(path, "Windows")
        if state.get("launch_pending"):
            raise RuntimeError("previous Windows launch outcome is unknown; guard identity is unavailable")
        pid = int(state.get("pid") or 0)
        if not pid:
            return {"status":"already_stopped", "name":args.name}
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
        kernel.GetProcessTimes.restype = wintypes.BOOL
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        ctypes.set_last_error(0)
        handle = kernel.OpenProcess(0x1000 | 0x00100000, False, pid)
        if not handle:
            if ctypes.get_last_error() == 87:
                return {"status":"already_stopped", "name":args.name}
            raise RuntimeError("cannot inspect the owned guard process")
        try:
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            code = wintypes.DWORD()
            if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
                raise RuntimeError("cannot verify guard identity")
            if ((created.dwHighDateTime << 32) | created.dwLowDateTime) != state.get("created"):
                raise RuntimeError("guard process identity changed")
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
                raise RuntimeError("cannot inspect the owned guard process")
            if code.value != 259:
                return {"status":"already_stopped", "name":args.name}
            user32 = ctypes.WinDLL("user32", use_last_error=True)
            user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
            user32.FindWindowW.restype = wintypes.HWND
            user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
            user32.GetWindowThreadProcessId.restype = wintypes.DWORD
            user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            user32.PostMessageW.restype = wintypes.BOOL
            window = user32.FindWindowW(None, args.name)
            if not window:
                raise RuntimeError("owned client window is not available yet")
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
            class Entry(ctypes.Structure):
                _fields_ = [("size",wintypes.DWORD),("usage",wintypes.DWORD),("pid",wintypes.DWORD),
                            ("heap",ctypes.c_size_t),("module",wintypes.DWORD),("threads",wintypes.DWORD),
                            ("parent",wintypes.DWORD),("priority",wintypes.LONG),("flags",wintypes.DWORD),("exe",wintypes.WCHAR*260)]
            kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
            kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
            kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
            kernel.Process32FirstW.restype = wintypes.BOOL
            kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
            kernel.Process32NextW.restype = wintypes.BOOL
            snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
            if snapshot == ctypes.c_void_p(-1).value:
                raise RuntimeError("cannot inspect client process ancestry")
            try:
                entry = Entry()
                entry.size = ctypes.sizeof(entry)
                parents = {}
                exists = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
                while exists:
                    parents[entry.pid] = entry.parent
                    exists = kernel.Process32NextW(snapshot, ctypes.byref(entry))
            finally:
                kernel.CloseHandle(snapshot)
            cursor = owner.value
            for _ in range(16):
                if cursor == pid:
                    break
                cursor = parents.get(cursor, 0)
                if not cursor:
                    break
            if cursor != pid:
                raise RuntimeError("window does not belong to this guarded client")
            if not user32.PostMessageW(window, 0x0010, 0, 0):
                raise RuntimeError("cannot request the client window to close")
            return {"status":"stopped" if kernel.WaitForSingleObject(handle, 20000)==0 else "close_requested", "name":args.name}
        finally:
            kernel.CloseHandle(handle)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("windows", "quest"):
        p = sub.add_parser(name)
        p.add_argument("--server", default=DEFAULT_SERVER)
        p.add_argument("--role", choices=ROLES, default="player")
        p.add_argument("--code-file", type=Path, help="omit to show login screen and submit a code later")
        if name == "windows":
            p.add_argument("--executable", type=Path, required=True)
            p.add_argument("--editor", action="store_true")
            p.add_argument("--name", default="MRFPS-Client")
            p.add_argument("--replace", action="store_true")
            p.add_argument("--auth-test-url", default="", help="development builds only; localhost HTTP")
            p.add_argument("--show-settings", action="store_true", help="start at the server settings page")
        else:
            p.add_argument("--adb", default="adb")
            p.add_argument("--serial", required=True)
            p.add_argument("--replace", action="store_true")
            p.add_argument("--adopt", action="store_true", help="explicitly take control of an already running MRFPS app")
            p.add_argument("--show-settings", action="store_true", help="start at the saved server settings page")
            p.add_argument("--glove-sender", help="pin the PC glove relay's IPv4 address when launching")
            p.add_argument("--glove-port", type=int, help="Quest receive port; defaults to 25788")
            p.add_argument("--glove-device", help="optional DivingStation device ID")
    p = sub.add_parser("reset-trust")
    p.add_argument("platform", choices=("windows", "quest"))
    p.add_argument("--adb", default="adb")
    p.add_argument("--serial", default="")
    p = sub.add_parser("status")
    p.add_argument("--name", default="MRFPS-Client")
    p = sub.add_parser("stop-windows")
    p.add_argument("--name", default="MRFPS-Client")
    p = sub.add_parser("stop-quest")
    p.add_argument("--adb", default="adb")
    p.add_argument("--serial", required=True)
    args = parser.parse_args()
    try:
        result = (windows(args) if args.command == "windows" else quest(args) if args.command == "quest" else
                  reset(args) if args.command == "reset-trust" else status(args) if args.command == "status" else
                  stop_quest(args) if args.command == "stop-quest" else stop_windows(args))
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(json.dumps({"status": "error", "message": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
