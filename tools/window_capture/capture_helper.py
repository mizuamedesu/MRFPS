"""One-shot, single-HWND WGC worker. Invoked only by capture.py via pipes."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import sys

import win_identity

MAX_PNG = 64 * 1024 * 1024


def emit(value: dict) -> None:
    sys.stdout.write(json.dumps(value, ensure_ascii=True, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def receive() -> dict:
    line = sys.stdin.readline(8193)
    if not line or len(line) > 8192:
        raise RuntimeError("capture controller disconnected")
    value = json.loads(line)
    if not isinstance(value, dict):
        raise ValueError("invalid capture protocol")
    return value


def require_ack(stage: str) -> None:
    emit({"event": stage})
    if receive() != {"ack": stage}:
        raise RuntimeError("capture controller rejected the window")


def validate_target(identity: dict, marker: dict, api: win_identity.Windows) -> int:
    """Reject missing/invalid HWND before the library can default to a monitor."""
    hwnd = win_identity._positive(identity.get("target", {}).get("hwnd"), "HWND")
    api.validate(identity, marker)
    return hwnd


def build_capture(capture_class, identity: dict, marker: dict, api: win_identity.Windows):
    hwnd = validate_target(identity, marker, api)
    # Do not offer monitor_index, window_name, secondary windows, or DXGI.
    # None preserves the platform's default (secondary windows are excluded).
    return capture_class(window_hwnd=hwnd, window_name=None, monitor_index=None,
                         secondary_window=None, draw_border=None)


def run(request: dict, api: win_identity.Windows, capture_class, cv2) -> dict:
    identity, marker = request["identity"], request["marker"]
    operation_id, root = request["id"], Path(request["root"])
    if not re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", operation_id):
        raise ValueError("invalid operation ID")
    expected_root = Path(os.environ["LOCALAPPDATA"]) / "MRFPS" / "WindowCapture"
    if not root.is_absolute() or root.resolve() != expected_root.resolve():
        raise ValueError("invalid capture directory")
    partial = root / (operation_id + ".partial.png")
    if partial.exists() or partial.is_symlink():
        raise RuntimeError("partial capture already exists")

    capture = build_capture(capture_class, identity, marker, api)
    outcome: dict = {}

    @capture.event
    def on_frame_arrived(frame, control):
        if outcome:
            control.stop()
            return
        try:
            # The parent and the child both inspect the exact HWND at frame
            # receipt, and again before a temporary PNG can be written.
            validate_target(identity, marker, api)
            require_ack("frame")
            validate_target(identity, marker, api)
            width, height = int(frame.width), int(frame.height)
            if not (0 < width <= win_identity.MAX_SIDE and 0 < height <= win_identity.MAX_SIDE
                    and width * height <= win_identity.MAX_PIXELS):
                raise RuntimeError("captured frame dimensions exceed the limit")
            pixels = frame.frame_buffer
            if getattr(pixels, "shape", None) != (height, width, 4) or str(getattr(pixels, "dtype", "")) != "uint8":
                raise RuntimeError("captured frame has an unexpected pixel format")
            require_ack("save")
            validate_target(identity, marker, api)
            ok, encoded = cv2.imencode(".png", pixels)
            if not ok or not 0 < encoded.size <= MAX_PNG:
                raise RuntimeError("could not encode the frame")
            data = encoded.tobytes()
            validate_target(identity, marker, api)
            with os.fdopen(os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_BINARY, 0o600), "wb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            validate_target(identity, marker, api)
            outcome.update(event="saved", width=width, height=height,
                           sha256=hashlib.sha256(data).hexdigest(), size=len(data))
        except Exception:
            outcome.update(event="error", code="frame_or_identity_failed")
        finally:
            control.stop()

    @capture.event
    def on_closed():
        pass

    # start() may block in the native library. The parent owns the deadline
    # and the kill-on-close job; it never waits indefinitely for this worker.
    capture.start()
    return outcome or {"event": "error", "code": "window_closed_without_frame"}


def main() -> int:
    try:
        request = receive()
        api = win_identity.Windows()
        validate_target(request["identity"], request["marker"], api)
        import importlib.metadata
        expected = {"windows-capture": "2.0.1", "numpy": "2.2.6", "opencv-python": "4.12.0.88"}
        if any(importlib.metadata.version(name) != version for name, version in expected.items()):
            raise RuntimeError("capture dependency version mismatch")
        import cv2
        from windows_capture import WindowsCapture
        emit(run(request, api, WindowsCapture, cv2))
        return 0
    except Exception:
        emit({"event": "error", "code": "helper_failed"})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
