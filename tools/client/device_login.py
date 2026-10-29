#!/usr/bin/env python3
"""Idempotent, credential-quiet entry points for the device-login workflow.

Windows: launch a named standalone client once with a code file.
Quest: provision a development APK through adb stdin and launch its login once.
To deliberately start a new session, use --replace; never embed a code in argv.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
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


def read_code(path: Path) -> bytes:
    data = path.read_bytes().strip()
    if not re.fullmatch(rb"[0-9]{6}", data):
        raise ValueError("code file must contain exactly six ASCII digits")
    return data


def run_adb(adb: str, serial: str, args: list[str], *, data: bytes | None = None, check: bool = True) -> str:
    command = [adb] + (["-s", serial] if serial else []) + args
    result = subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if check and result.returncode:
        raise RuntimeError(f"adb operation failed (exit {result.returncode}); contents omitted")
    return result.stdout.decode("utf-8", "replace").strip()


def quest_pid(adb: str, serial: str) -> str:
    value = run_adb(adb, serial, ["shell", "pidof", PACKAGE], check=False)
    return value if re.fullmatch(r"[0-9]+(?:\s+[0-9]+)*", value) else ""


def load_state(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError, OSError):
        return {}


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
    marker = {"path": str(code_file.resolve(strict=True)), "modified_ns": code_file.stat().st_mtime_ns}
    return marker, read_code(code_file).decode("ascii")


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


def quest(args: argparse.Namespace) -> dict:
    address = endpoint(args.server)
    serial = args.serial
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", serial):
        raise ValueError("invalid serial")
    state_path = ROOT / "Saved" / "DeviceAuth" / ("quest-" + serial + ".json")
    with state_lock(state_path):
        return quest_locked(args, address, state_path)


def quest_locked(args: argparse.Namespace, address: str, state_path: Path) -> dict:
    serial = args.serial
    wanted = {"server": address, "role": args.role}
    marker, code = submission(args.code_file)
    current_pid = quest_pid(args.adb, serial)
    previous = load_state(state_path)
    previous_pids = set(str(previous.get("pid") or "").split())
    same_process = bool(previous_pids.intersection(current_pid.split()))
    known = same_process or previous.get("pending") or getattr(args, "adopt", False)
    if current_pid and known and not args.replace:
        if not same_process:
            previous["pid"] = current_pid
            previous["pending"] = False
            save_state(state_path, previous)
        if previous.get("config") == wanted and (marker is None or marker == previous.get("submitted")):
            return {"status": "already_running", "pid": current_pid, "config": wanted}
        quest_control(args, address, code)
        previous["config"] = wanted
        previous["submitted"] = marker
        save_state(state_path, previous)
        return {"status": "login_queued" if marker else "settings_queued", "pid": current_pid, "config": wanted}
    if current_pid and same_process and previous.get("config") == wanted and (marker is None or marker == previous.get("submitted")):
        return {"status": "already_running", "pid": current_pid, "config": wanted}
    if current_pid and not args.replace:
        raise RuntimeError("Quest app is running with unknown or different settings; use --replace to restart this app")
    if current_pid:
        run_adb(args.adb, serial, ["shell", "am", "force-stop", PACKAGE])
    quest_control(args, address, code)
    save_state(state_path, {"pid": None, "pending": True, "config": wanted, "submitted": marker})
    cmdline = "/Game/Maps/MRFPSArena -vr -log"
    run_adb(args.adb, serial, ["shell", "am", "start", "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-n", PACKAGE + "/com.epicgames.unreal.GameActivity", "--es", "cmdline", "'" + cmdline + "'"])
    current_pid = ""
    for _ in range(12):
        current_pid = quest_pid(args.adb, serial)
        if current_pid:
            break
        time.sleep(.25)
    if current_pid:
        save_state(state_path, {"pid": current_pid, "pending": False, "config": wanted, "submitted": marker})
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
    old = load_state(state_path)
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
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        try:
            exit_code = wintypes.DWORD()
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)) or exit_code.value != 259:
                return None
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
                return None
            return (created.dwHighDateTime << 32) | created.dwLowDateTime
        finally:
            kernel.CloseHandle(handle)
    pid = old.get("pid", 0)
    active = bool(pid and creation_time(pid) == old.get("created"))
    prior = old.get("config", {})
    same_binary = all(prior.get(key) == wanted.get(key) for key in ("name", "auth_test_url", "binary", "editor"))
    if active and not args.replace:
        if args.show_settings:
            if old.get("config") != wanted or old.get("configured", True):
                raise RuntimeError("settings page requires a new instance; use --replace")
            return {"status": "already_running", "guard_pid": pid, "config": wanted}
        if not same_binary:
            raise RuntimeError("a client with this name already runs with different settings; use --replace")
        if old.get("config") == wanted and old.get("configured", True) and (marker is None or marker == old.get("submitted")):
            return {"status": "already_running", "guard_pid": pid, "config": wanted}
        windows_control(args.name, address, args.role, code)
        old["config"] = wanted
        old["submitted"] = marker
        old["configured"] = True
        save_state(state_path, old)
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
        windows_control(args.name, address, args.role, code)
    run_id = uuid.uuid4().hex
    report = ROOT / "Saved" / "Validation" / "DeviceLogin" / args.name / run_id / "report.json"
    launch_config = state_path.parent / ("windows-" + args.name + "-launch-" + run_id + ".json")
    save_state(launch_config, {"binary":str(binary), "editor":args.editor, "name":args.name,
                               "test_url":args.auth_test_url or "", "report":str(report), "python":sys.executable})
    guard = ROOT / "Scripts" / "Start-DeviceLoginGuarded.ps1"
    # The guard owns a Windows Job Object and monitors system memory; no UE
    # process can escape the 10 GB / 75% constraints used for the build.
    process = subprocess.Popen(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(guard), "-ConfigPath", str(launch_config)],
                               cwd=ROOT, close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    created = creation_time(process.pid)
    if not created:
        raise RuntimeError("unable to verify client process identity")
    save_state(state_path, {"pid": process.pid, "created": created, "config": wanted, "submitted": marker,
                            "configured": not args.show_settings, "report":str(report)})
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
    state = load_state(ROOT / "Saved" / "DeviceAuth" / ("windows-" + args.name + ".json"))
    if not state:
        return {"status": "not_started", "name": args.name}
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
        state = load_state(path)
        pid = int(state.get("pid", 0))
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
        handle = kernel.OpenProcess(0x1000 | 0x00100000, False, pid)
        if not handle:
            return {"status":"already_stopped", "name":args.name}
        try:
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            code = wintypes.DWORD()
            if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
                raise RuntimeError("cannot verify guard identity")
            if ((created.dwHighDateTime << 32) | created.dwLowDateTime) != state.get("created"):
                raise RuntimeError("guard process identity changed")
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value != 259:
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
    p = sub.add_parser("reset-trust")
    p.add_argument("platform", choices=("windows", "quest"))
    p.add_argument("--adb", default="adb")
    p.add_argument("--serial", default="")
    p = sub.add_parser("status")
    p.add_argument("--name", default="MRFPS-Client")
    p = sub.add_parser("stop-windows")
    p.add_argument("--name", default="MRFPS-Client")
    args = parser.parse_args()
    try:
        result = windows(args) if args.command == "windows" else quest(args) if args.command == "quest" else reset(args) if args.command == "reset-trust" else status(args) if args.command == "status" else stop_windows(args)
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(json.dumps({"status": "error", "message": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
