"""Identity and resource checks for an explicitly selected Windows window.

This module never captures pixels and never chooses a monitor. All HWNDs are
supplied by the caller and must belong to the same explicitly named process.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path
import uuid

POPUP_TITLES = {"MRFPS - P1", "MRFPS - P2", "MRFPS - 観戦2"}
CLIENT_BINARIES = {
    "mrfps.exe", "mrfps-win64-development.exe", "mrfps-win64-shipping.exe",
    "unrealeditor.exe", "unrealeditor-win64-debuggame.exe",
}
MAX_PIXELS = 4096 * 2160
MAX_SIDE = 4096


def _positive(value: object, field: str) -> int:
    if type(value) is not int or not 0 < value < (1 << 63):
        raise ValueError(f"{field} must be a positive integer")
    return value


def validate_marker(marker: object, prefix: str) -> None:
    if not isinstance(marker, dict) or set(marker) != {"name", "value"}:
        raise ValueError("invalid window instance marker")
    name, value = marker["name"], marker["value"]
    if not isinstance(name, str) or not name.startswith(prefix) or type(value) is not int:
        raise ValueError("invalid window instance marker")
    raw = name[len(prefix):]
    try:
        parsed = uuid.UUID(raw)
    except (ValueError, AttributeError) as error:
        raise ValueError("invalid window instance marker") from error
    expected = (int.from_bytes(hashlib.sha256(raw.encode("ascii")).digest()[:4], "big") & 0x7fffffff) or 1
    if parsed.version != 4 or str(parsed) != raw or value != expected:
        raise ValueError("invalid window instance marker")


class Windows:
    def __init__(self) -> None:
        if os.name != "nt":
            raise RuntimeError("window capture is available only on Windows")
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        u, k = self.user, self.kernel
        u.IsWindow.argtypes = [wintypes.HWND]; u.IsWindow.restype = wintypes.BOOL
        u.IsWindowVisible.argtypes = [wintypes.HWND]; u.IsWindowVisible.restype = wintypes.BOOL
        u.IsIconic.argtypes = [wintypes.HWND]; u.IsIconic.restype = wintypes.BOOL
        u.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]; u.GetAncestor.restype = wintypes.HWND
        u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        u.GetWindowThreadProcessId.restype = wintypes.DWORD
        u.GetWindowTextLengthW.argtypes = [wintypes.HWND]; u.GetWindowTextLengthW.restype = ctypes.c_int
        u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]; u.GetWindowTextW.restype = ctypes.c_int
        u.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]; u.GetClassNameW.restype = ctypes.c_int
        u.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]; u.GetWindowRect.restype = wintypes.BOOL
        self.callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        u.EnumWindows.argtypes = [self.callback_type, wintypes.LPARAM]; u.EnumWindows.restype = wintypes.BOOL
        u.SetPropW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.HANDLE]; u.SetPropW.restype = wintypes.BOOL
        u.GetPropW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]; u.GetPropW.restype = wintypes.HANDLE
        u.RemovePropW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]; u.RemovePropW.restype = wintypes.HANDLE
        k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]; k.OpenProcess.restype = wintypes.HANDLE
        k.CloseHandle.argtypes = [wintypes.HANDLE]; k.CloseHandle.restype = wintypes.BOOL
        k.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
                                     ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
        k.GetProcessTimes.restype = wintypes.BOOL
        k.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]; k.GetExitCodeProcess.restype = wintypes.BOOL
        k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        k.QueryFullProcessImageNameW.restype = wintypes.BOOL

    def title(self, hwnd: int) -> str:
        length = self.user.GetWindowTextLengthW(hwnd)
        if length <= 0 or length > 512:
            raise RuntimeError("window title unavailable or too long")
        text = ctypes.create_unicode_buffer(length + 1)
        if self.user.GetWindowTextW(hwnd, text, len(text)) != length:
            raise RuntimeError("window title changed during inspection")
        return text.value

    def process(self, pid: int) -> dict:
        _positive(pid, "process ID")
        handle = self.kernel.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            raise RuntimeError("cannot inspect the selected process")
        try:
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            code = wintypes.DWORD()
            if (not self.kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                                ctypes.byref(system), ctypes.byref(user))
                    or not self.kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value != 259):
                raise RuntimeError("selected process is not alive")
            size = wintypes.DWORD(32768)
            image = ctypes.create_unicode_buffer(size.value)
            if not self.kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size)):
                raise RuntimeError("cannot inspect the process executable")
            executable = Path(image.value).name.lower()
            if executable not in CLIENT_BINARIES:
                raise RuntimeError("selected process is not an MRFPS or Unreal client executable")
            return {"pid": pid, "created": (created.dwHighDateTime << 32) | created.dwLowDateTime,
                    "executable": executable}
        finally:
            self.kernel.CloseHandle(handle)

    def window(self, hwnd: int, pid: int, allow_minimized: bool = False) -> dict:
        _positive(hwnd, "HWND"); _positive(pid, "process ID")
        u = self.user
        iconic = bool(u.IsIconic(hwnd))
        if not u.IsWindow(hwnd) or not u.IsWindowVisible(hwnd) or (iconic and not allow_minimized):
            raise RuntimeError("selected window is closed, hidden, or minimized")
        if u.GetAncestor(hwnd, 2) != hwnd:  # GA_ROOT: never capture a child control
            raise RuntimeError("selected HWND is not a top-level window")
        owner = wintypes.DWORD()
        thread = int(u.GetWindowThreadProcessId(hwnd, ctypes.byref(owner)))
        if not thread or owner.value != pid:
            raise RuntimeError("selected window has a different owner")
        class_name = ctypes.create_unicode_buffer(256)
        if not u.GetClassNameW(hwnd, class_name, len(class_name)):
            raise RuntimeError("selected window class is unavailable")
        rect = wintypes.RECT()
        if not u.GetWindowRect(hwnd, ctypes.byref(rect)):
            raise RuntimeError("selected window dimensions are unavailable")
        width, height = rect.right - rect.left, rect.bottom - rect.top
        if not iconic and not (0 < width <= MAX_SIDE and 0 < height <= MAX_SIDE and width * height <= MAX_PIXELS):
            raise RuntimeError("selected window dimensions exceed the capture limit")
        title = self.title(hwnd)
        matches = []

        @self.callback_type
        def visit(candidate, _):
            try:
                if self.title(candidate) == title:
                    matches.append(int(candidate))
            except RuntimeError:
                pass
            return True

        if not u.EnumWindows(visit, 0) or matches != [hwnd]:
            raise RuntimeError("window title is missing or ambiguous")
        return {"hwnd": hwnd, "thread": thread, "class": class_name.value, "title": title}

    def list_windows(self, pid: int) -> dict:
        """List window metadata only for the explicitly selected client PID."""
        process = self.process(pid)
        windows = []

        @self.callback_type
        def visit(candidate, _):
            owner = wintypes.DWORD()
            thread = int(self.user.GetWindowThreadProcessId(candidate, ctypes.byref(owner)))
            if thread and owner.value == pid and self.user.IsWindowVisible(candidate):
                try:
                    title = self.title(candidate)
                    windows.append({"hwnd": int(candidate), "title": title,
                                    "kind": "popout" if title in POPUP_TITLES else "main_candidate",
                                    "minimized": bool(self.user.IsIconic(candidate))})
                except RuntimeError:
                    pass
            return True

        if not self.user.EnumWindows(visit, 0) or self.process(pid) != process:
            raise RuntimeError("cannot list windows for the selected process")
        return {"process": process, "windows": windows}

    def snapshot(self, pid: int, main_hwnd: int, target_hwnd: int, allow_minimized: bool = False) -> dict:
        process = self.process(pid)
        main = self.window(main_hwnd, pid, allow_minimized)
        if target_hwnd == main_hwnd:
            target = main
        else:
            target = self.window(target_hwnd, pid, allow_minimized)
            if target["title"] not in POPUP_TITLES or main["title"] in POPUP_TITLES:
                raise RuntimeError("target is not an MRFPS player or inspector popout")
        # Recheck process identity after both windows to detect a process restart.
        if self.process(pid) != process:
            raise RuntimeError("selected process changed during inspection")
        return {"process": process, "main": main, "target": target}

    def validate_snapshot(self, identity: dict, allow_minimized: bool = False) -> None:
        try:
            pid = identity["process"]["pid"]
            main = identity["main"]["hwnd"]
            target = identity["target"]["hwnd"]
        except (KeyError, TypeError) as error:
            raise ValueError("invalid window identity") from error
        expected = {key: identity[key] for key in ("process", "main", "target")}
        actual = (self.snapshot(pid, main, target, allow_minimized=True) if allow_minimized
                  else self.snapshot(pid, main, target))
        if actual != expected:
            raise RuntimeError("window or process identity changed")

    def validate(self, identity: dict, marker: dict | None = None, allow_minimized: bool = False) -> None:
        if set(identity) != {"process", "main", "target", "window_marker"}:
            raise ValueError("invalid window identity")
        self.validate_snapshot(identity, allow_minimized)
        main, target = identity["main"]["hwnd"], identity["target"]["hwnd"]
        stable = identity["window_marker"]
        validate_marker(stable, "MRFPS-Identity-")
        for hwnd in {main, target}:
            if self.user.GetPropW(hwnd, stable["name"]) != stable["value"]:
                raise RuntimeError("identified window was closed or replaced")
        if marker is not None:
            validate_marker(marker, "MRFPS-Capture-")
            for hwnd in {main, target}:
                if self.user.GetPropW(hwnd, marker["name"]) != marker["value"]:
                    raise RuntimeError("window instance changed")

    def _attach(self, identity: dict, marker: dict) -> None:
        attached = []
        try:
            for hwnd in {identity["main"]["hwnd"], identity["target"]["hwnd"]}:
                if self.user.GetPropW(hwnd, marker["name"]):
                    raise RuntimeError("capture marker already exists")
                if not self.user.SetPropW(hwnd, marker["name"], marker["value"]):
                    raise RuntimeError("cannot mark the selected window instance")
                attached.append(hwnd)
        except Exception:
            for hwnd in attached:
                if self.user.GetPropW(hwnd, marker["name"]) == marker["value"]:
                    self.user.RemovePropW(hwnd, marker["name"])
            raise

    def bind(self, snapshot: dict, stable: dict, allow_minimized: bool = False) -> dict:
        """Bind the initial selection to window objects, not recycled HWNDs."""
        validate_marker(stable, "MRFPS-Identity-")
        self.validate_snapshot(snapshot, allow_minimized)
        self._attach(snapshot, stable)
        identity = {**snapshot, "window_marker": stable}
        try:
            self.validate(identity, allow_minimized=allow_minimized)
        except Exception:
            self.unmark(identity, stable)
            raise
        return identity

    def mark(self, identity: dict, marker: dict) -> None:
        validate_marker(marker, "MRFPS-Capture-")
        self.validate(identity)
        self._attach(identity, marker)
        try:
            self.validate(identity, marker)
        except Exception:
            self.unmark(identity, marker)
            raise

    def unmark(self, identity: dict, marker: dict) -> None:
        for hwnd in {identity["main"]["hwnd"], identity["target"]["hwnd"]}:
            # Remove only our own named property with its exact value.
            if self.user.GetPropW(hwnd, marker["name"]) == marker["value"]:
                self.user.RemovePropW(hwnd, marker["name"])


def memory_safe() -> bool:
    if os.name != "nt":
        raise RuntimeError("memory guard requires Windows")

    class Status(ctypes.Structure):
        _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD),
                    ("total_phys", ctypes.c_ulonglong), ("available_phys", ctypes.c_ulonglong),
                    ("total_page", ctypes.c_ulonglong), ("available_page", ctypes.c_ulonglong),
                    ("total_virtual", ctypes.c_ulonglong), ("available_virtual", ctypes.c_ulonglong),
                    ("available_extended", ctypes.c_ulonglong)]

    status = Status(); status.length = ctypes.sizeof(status)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(Status)]; kernel.GlobalMemoryStatusEx.restype = wintypes.BOOL
    if not kernel.GlobalMemoryStatusEx(ctypes.byref(status)) or not status.total_page:
        raise RuntimeError("cannot measure system memory")
    return status.available_phys >= 10 * (1024 ** 3) and 100 * (status.total_page - status.available_page) <= 75 * status.total_page
