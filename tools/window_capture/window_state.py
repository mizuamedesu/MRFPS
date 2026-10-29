#!/usr/bin/env python3
"""Journalled desired-state control of exactly one identified MRFPS window."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import time

import capture
import win_identity

MAX_AGE = 10.0
SETTLE_TIMEOUT = 2.0
POSITION_FLAGS = 0x4000 | 0x0010 | 0x0004 | 0x0200
SUPPORTED_EXECUTABLES = {
    "mrfps.exe", "mrfps-win64-development.exe", "mrfps-win64-shipping.exe",
}


class GUIThreadInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


class WindowStateAPI:
    """Only acts on an HWND already checked by the identity validator."""
    def __init__(self, identity_api: win_identity.Windows):
        self.identity_api = identity_api
        self.user = identity_api.user
        u = self.user
        u.IsZoomed.argtypes = [wintypes.HWND]; u.IsZoomed.restype = wintypes.BOOL
        u.GetForegroundWindow.argtypes = []; u.GetForegroundWindow.restype = wintypes.HWND
        u.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]; u.GetWindow.restype = wintypes.HWND
        u.GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GUIThreadInfo)]
        u.GetGUIThreadInfo.restype = wintypes.BOOL
        u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wintypes.UINT]
        u.SetWindowPos.restype = wintypes.BOOL
        u.ShowWindowAsync.argtypes = [wintypes.HWND, ctypes.c_int]; u.ShowWindowAsync.restype = wintypes.BOOL
        u.SetForegroundWindow.argtypes = [wintypes.HWND]; u.SetForegroundWindow.restype = wintypes.BOOL
        u.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        self.imm = ctypes.WinDLL("imm32", use_last_error=True)
        self.imm.ImmGetDefaultIMEWnd.argtypes = [wintypes.HWND]
        self.imm.ImmGetDefaultIMEWnd.restype = wintypes.HWND

    @contextmanager
    def dpi(self):
        # GetWindowRect and SetWindowPos operate in the same physical coordinate
        # space, even when the caller itself is running under a different DPI.
        old = self.user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        if not old:
            raise RuntimeError("cannot enter per-monitor DPI context")
        try:
            yield
        finally:
            self.user.SetThreadDpiAwarenessContext(old)

    def validate(self, identity: dict) -> None:
        self.identity_api.validate(identity, allow_minimized=True)

    def read(self, identity: dict) -> dict:
        hwnd, thread = identity["target"]["hwnd"], identity["target"]["thread"]
        self.validate(identity)
        info = GUIThreadInfo(); info.cbSize = ctypes.sizeof(info)
        if not self.user.GetGUIThreadInfo(thread, ctypes.byref(info)):
            raise RuntimeError("cannot inspect target GUI thread")
        minimized = bool(self.user.IsIconic(hwnd))
        rect = None
        if not minimized:
            native = wintypes.RECT()
            if not self.user.GetWindowRect(hwnd, ctypes.byref(native)):
                raise RuntimeError("cannot inspect target rectangle")
            rect = {"x": native.left, "y": native.top,
                    "width": native.right - native.left, "height": native.bottom - native.top}
        result = {"rect": rect, "minimized": minimized, "maximized": bool(self.user.IsZoomed(hwnd)),
                  "foreground": self.user.GetForegroundWindow() == hwnd,
                  "move_size": bool(info.flags & 0x2)}
        self.validate(identity)
        return result

    def has_owned_window(self, hwnd: int) -> bool:
        # Minimizing an owner can implicitly hide its owned windows. Even
        # currently hidden owned windows cause refusal: they may reappear.
        found = []

        @self.identity_api.callback_type
        def visit(candidate, _):
            if candidate == hwnd:
                return True
            current, seen = candidate, set()
            while current:
                if current in seen or len(seen) >= 64:
                    found.append(True); return False
                seen.add(current)
                current = self.user.GetWindow(current, 4)  # GW_OWNER
                if current == hwnd:
                    found.append(True); return False
            return True

        completed = self.user.EnumWindows(visit, 0)
        if not completed and not found:
            raise RuntimeError("cannot inspect owned windows")
        return bool(found)

    def rect_owned_snapshot(self, identity: dict) -> dict:
        """Read the exact owner chain, including hidden windows; do not move it."""
        self.validate(identity)
        hwnd = identity["target"]["hwnd"]
        default_ime = int(self.imm.ImmGetDefaultIMEWnd(hwnd) or 0)
        found, errors = [], []

        @self.identity_api.callback_type
        def visit(candidate, _):
            try:
                candidate = int(candidate)
                if candidate == hwnd:
                    return True
                current, chain, seen = candidate, [], set()
                while current:
                    if current in seen or len(seen) >= 64:
                        raise RuntimeError("invalid window ownership chain")
                    seen.add(current)
                    current = int(self.user.GetWindow(current, 4) or 0)  # GW_OWNER
                    if current:
                        chain.append(current)
                    if current == hwnd:
                        break
                if not chain or chain[-1] != hwnd:
                    return True
                pid = wintypes.DWORD()
                thread = int(self.user.GetWindowThreadProcessId(candidate, ctypes.byref(pid)))
                class_name = ctypes.create_unicode_buffer(256)
                if (not self.user.IsWindow(candidate) or not thread or not pid.value or
                        not self.user.GetClassNameW(candidate, class_name, len(class_name))):
                    raise RuntimeError("owned window changed during inspection")
                found.append((candidate, tuple(chain), pid.value, thread, class_name.value,
                              bool(self.user.IsWindowVisible(candidate))))
                if not self.user.IsWindow(candidate):
                    raise RuntimeError("owned window disappeared during inspection")
                return True
            except Exception as error:
                errors.append(error)
                return False

        completed = self.user.EnumWindows(visit, 0)
        if errors or not completed:
            raise RuntimeError("cannot inspect owned windows")
        self.validate(identity)
        return {"default_ime": default_ime, "windows": tuple(sorted(found))}

    def dispatch(self, hwnd: int, desired: dict) -> None:
        if "rect" in desired:
            r = desired["rect"]
            if not self.user.SetWindowPos(hwnd, None, r["x"], r["y"], r["width"], r["height"], POSITION_FLAGS):
                raise RuntimeError("position request could not be posted")
        elif "minimized" in desired or "normal" in desired:
            command = 7 if desired.get("minimized") is True else 4  # never activate the target
            if not self.user.ShowWindowAsync(hwnd, command):
                raise RuntimeError("show-state request could not be posted")
        else:
            if not self.user.SetForegroundWindow(hwnd):
                raise RuntimeError("Windows declined foreground request")


def prepare(root: Path) -> None:
    capture.prepare_directory(root)
    for name in ("window-state", "window-state/snapshots", "window-state/operations"):
        directory = root / name
        capture.reject_reparse(directory)
        directory.mkdir(exist_ok=True, mode=0o700)
        capture.reject_reparse(directory)
        if directory.resolve().parent != directory.parent.resolve():
            raise RuntimeError("unsafe window-state directory")
        capture.restrict_directories([directory])


def identity_at(root: Path, identity_id: str) -> dict:
    identity_id = capture.valid_id(identity_id)
    state = capture.read_json(root / "identities" / (identity_id + ".json"))
    if state.get("identity_id") != identity_id or state.get("status") != "ok":
        raise RuntimeError("window identification is unavailable")
    identity = state.get("identity")
    capture.validate_explicit_identity(identity)
    if identity["process"]["executable"] not in SUPPORTED_EXECUTABLES:
        raise RuntimeError("window control requires an MRFPS client executable, not an editor")
    main, target = identity["main"], identity["target"]
    if not main["title"].startswith("MRFPS") or main["title"] in win_identity.POPUP_TITLES:
        raise RuntimeError("main window title must begin with MRFPS")
    if main["class"] != "UnrealWindow" or target["class"] != "UnrealWindow":
        raise RuntimeError("target is not an Unreal client window")
    return identity


def snapshot(root: Path, identity_id: str, snapshot_id: str, api: WindowStateAPI) -> dict:
    identity_id, snapshot_id = capture.valid_id(identity_id), capture.valid_id(snapshot_id)
    path = root / "window-state" / "snapshots" / (snapshot_id + ".json")
    with capture.directory_lock(root):
        if path.exists():
            previous = capture.read_json(path)
            if previous.get("identity_id") != identity_id or previous.get("snapshot_id") != snapshot_id:
                raise RuntimeError("snapshot ID already identifies a different request")
            return previous
        identity = identity_at(root, identity_id)
        with api.dpi():
            before = api.read(identity)
            after = api.read(identity)
        if before != after:
            raise RuntimeError("window state changed during inspection")
        result = {"status": "ok", "identity_id": identity_id, "snapshot_id": snapshot_id,
                  "state": before, "observed_at_monotonic": time.monotonic()}
        capture.atomic_json(path, result)
        return result


def validate_desired(desired: dict) -> None:
    if not isinstance(desired, dict) or len(desired) != 1:
        raise ValueError("choose exactly one desired window state")
    if "rect" in desired:
        rect = desired["rect"]
        if not isinstance(rect, dict) or set(rect) != {"x", "y", "width", "height"}:
            raise ValueError("invalid rectangle")
        x, y, width, height = (rect[key] for key in ("x", "y", "width", "height"))
        if (any(type(v) is not int for v in (x, y, width, height)) or
                not -16384 <= x <= 16384 or not -16384 <= y <= 16384 or
                not 320 <= width <= 4096 or not 240 <= height <= 2160 or
                width * height > win_identity.MAX_PIXELS):
            raise ValueError("rectangle is outside the supported bounds")
    elif "minimized" in desired or "foreground" in desired:
        if type(next(iter(desired.values()))) is not bool:
            raise ValueError("state must be true or false")
    elif "normal" in desired:
        if desired["normal"] is not True:
            raise ValueError("normal state must be true")
    else:
        raise ValueError("unknown desired state")


def result_matches(before: dict, current: dict, desired: dict) -> bool:
    if current["move_size"]:
        return False
    if "rect" in desired:
        return (current["rect"] == desired["rect"] and not current["minimized"] and
                not current["maximized"] and current["foreground"] == before["foreground"])
    if "minimized" in desired:
        return current["minimized"] == desired["minimized"] and current["foreground"] == before["foreground"]
    if "normal" in desired:
        return (not current["minimized"] and not current["maximized"] and
                current["foreground"] == before["foreground"])
    return (current["foreground"] is True and current["rect"] == before["rect"] and
            current["minimized"] == before["minimized"] and current["maximized"] == before["maximized"])


def rect_owned_windows_safe(snapshot: dict, identity: dict) -> bool:
    """Only the actual default IME and its hidden TSF child may accompany a move."""
    windows = snapshot["windows"]
    if not windows:
        return True
    hwnd = identity["target"]["hwnd"]
    pid = identity["process"]["pid"]
    thread = identity["target"]["thread"]
    ime = snapshot["default_ime"]
    if not ime or ime == hwnd:
        return False
    root_found = False
    for child, chain, owner_pid, owner_thread, class_name, visible in windows:
        if visible or owner_pid != pid or owner_thread != thread:
            return False
        if child == ime and chain == (hwnd,) and class_name == "IME":
            root_found = True
        elif chain == (ime, hwnd) and class_name == "MSCTFIME UI":
            pass
        else:
            return False
    return root_found


def ensure(root: Path, identity_id: str, snapshot_id: str, operation_id: str, desired: dict,
           api: WindowStateAPI, *, now=time.monotonic, sleep=time.sleep) -> dict:
    identity_id = capture.valid_id(identity_id)
    snapshot_id = capture.valid_id(snapshot_id)
    operation_id = capture.valid_id(operation_id)
    validate_desired(desired)
    request = {"identity_id": identity_id, "snapshot_id": snapshot_id, "desired": desired}
    path = root / "window-state" / "operations" / (operation_id + ".json")
    with capture.directory_lock(root):
        if path.exists():
            receipt = capture.read_json(path)
            if receipt.get("request") != request or receipt.get("operation_id") != operation_id:
                raise RuntimeError("operation ID already identifies a different request")
            if receipt.get("status") == "pending":
                receipt.update(status="unknown", code="interrupted")
                capture.atomic_json(path, receipt)
            elif receipt.get("status") not in ("ok", "error", "unknown"):
                raise RuntimeError("invalid operation state")
            return receipt
        receipt = {"operation_id": operation_id, "request": request, "status": "pending"}
        capture.atomic_json(path, receipt)
        try:
            identity = identity_at(root, identity_id)
            observed = capture.read_json(root / "window-state" / "snapshots" / (snapshot_id + ".json"))
            age = now() - observed["observed_at_monotonic"]
            if (observed.get("status") != "ok" or observed.get("identity_id") != identity_id or
                    observed.get("snapshot_id") != snapshot_id or not 0 <= age <= MAX_AGE):
                raise RuntimeError("snapshot is unavailable or stale")
            hwnd = identity["target"]["hwnd"]
            with api.dpi():
                before = api.read(identity)
                if before != observed.get("state") or before["move_size"]:
                    raise RuntimeError("window state changed after inspection")
                if "rect" in desired and (before["minimized"] or before["maximized"]):
                    receipt.update(status="error", code="restore_before_setting_rectangle")
                elif "foreground" in desired and not desired["foreground"]:
                    if before["foreground"]:
                        receipt.update(status="error", code="foreground_release_unsupported")
                    else:
                        receipt.update(status="ok", code="already_applied", state=before)
                elif ("rect" in desired and before["rect"] == desired["rect"] or
                      "minimized" in desired and before["minimized"] == desired["minimized"] or
                      "normal" in desired and not before["minimized"] and not before["maximized"] or
                      "foreground" in desired and desired["foreground"] and before["foreground"]):
                    receipt.update(status="ok", code="already_applied", state=before)
                elif "foreground" in desired and before["minimized"]:
                    receipt.update(status="error", code="restore_before_foreground")
                elif "minimized" in desired and desired["minimized"] and before["foreground"]:
                    receipt.update(status="error", code="minimize_foreground_unsupported")
                else:
                    # SetWindowPos does not hide/show owned windows. Permit only
                    # the target thread's verified, hidden default IME chain for
                    # rect operations, and compare that entire chain repeatedly.
                    rect_operation = "rect" in desired
                    ownership = api.rect_owned_snapshot(identity) if rect_operation else api.has_owned_window(hwnd)
                    blocked = (not rect_owned_windows_safe(ownership, identity)) if rect_operation else ownership
                    if blocked:
                        receipt.update(status="error", code="owned_windows_prevent_operation")
                    else:
                        if api.read(identity) != before:
                            raise RuntimeError("window state changed before dispatch")
                        current_ownership = api.rect_owned_snapshot(identity) if rect_operation else api.has_owned_window(hwnd)
                        if current_ownership != ownership:
                            raise RuntimeError("owned windows changed before dispatch")
                        api.validate(identity)
                        api.dispatch(hwnd, desired)
                        deadline = now() + SETTLE_TIMEOUT
                        while True:
                            current = api.read(identity)
                            current_ownership = api.rect_owned_snapshot(identity) if rect_operation else api.has_owned_window(hwnd)
                            if current_ownership != ownership:
                                raise RuntimeError("owned windows changed during operation")
                            if result_matches(before, current, desired):
                                api.validate(identity)
                                receipt.update(status="ok", code="applied", state=current)
                                break
                            if current["foreground"] != before["foreground"] and "foreground" not in desired:
                                raise RuntimeError("foreground changed during operation")
                            if current["move_size"] or now() >= deadline:
                                raise RuntimeError("window state did not settle")
                            sleep(0.05)
        except Exception:
            # The operation may have been posted despite a lost response or
            # interruption. Never repeat this UUID automatically.
            receipt.update(status="unknown", code="conflict_or_incomplete")
        capture.atomic_json(path, receipt)
        return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect", help="record one stable target state")
    inspect.add_argument("--identity-id", required=True); inspect.add_argument("--snapshot-id", required=True)
    change = commands.add_parser("ensure", help="apply exactly one desired state from a recent snapshot")
    change.add_argument("--identity-id", required=True); change.add_argument("--snapshot-id", required=True)
    change.add_argument("--operation-id", required=True)
    desired = change.add_mutually_exclusive_group(required=True)
    desired.add_argument("--rect", nargs=4, type=int, metavar=("X", "Y", "WIDTH", "HEIGHT"))
    desired.add_argument("--minimized", choices=("true", "false"))
    desired.add_argument("--normal", action="store_true", help="restore minimized or maximized window without activation")
    desired.add_argument("--foreground", choices=("true", "false"))
    args = parser.parse_args()
    try:
        root = capture.root_directory(); prepare(root)
        api = WindowStateAPI(win_identity.Windows())
        if args.command == "inspect":
            result = snapshot(root, args.identity_id, args.snapshot_id, api)
        else:
            if args.rect is not None:
                desired_state = {"rect": dict(zip(("x", "y", "width", "height"), args.rect))}
            elif args.minimized is not None:
                desired_state = {"minimized": args.minimized == "true"}
            elif args.normal:
                desired_state = {"normal": True}
            else:
                desired_state = {"foreground": args.foreground == "true"}
            result = ensure(root, args.identity_id, args.snapshot_id, args.operation_id, desired_state, api)
        print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
        return 0 if result["status"] == "ok" else 1
    except Exception as error:
        print(json.dumps({"status": "error", "code": str(error)}, ensure_ascii=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
