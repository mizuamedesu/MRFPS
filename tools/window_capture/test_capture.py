import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import uuid
from unittest import mock
import ctypes
from ctypes import wintypes
from contextlib import redirect_stdout
import sys
import zlib

import capture
import capture_helper
import win_identity


def identity():
    return {"process": {"pid": 42, "created": 123456, "executable": "mrfps.exe"},
            "main": {"hwnd": 101, "thread": 7, "title": "MRFPS-Test", "class": "UnrealWindow"},
            "target": {"hwnd": 102, "thread": 7, "title": "MRFPS - P1", "class": "UnrealWindow"},
            "window_marker": capture.marker_for("64bc1f97-07c0-482c-8f0d-c8513693af9d", "MRFPS-Identity-")}


def fake_png():
    def chunk(kind, content):
        crc = zlib.crc32(content, zlib.crc32(kind)) & 0xffffffff
        return struct.pack(">I", len(content)) + kind + content + struct.pack(">I", crc)
    header = struct.pack(">IIBBBBB", 4, 3, 8, 6, 0, 0, 0)
    pixels = (b"\x00" + b"\x00" * 16) * 3
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")


class FakeApi:
    def __init__(self):
        self.validations = []
        self.marked = False
        self.fail_on = None

    def validate(self, item, marker=None):
        self.validations.append((item, marker))
        if self.fail_on == len(self.validations):
            raise RuntimeError("window changed")
        if marker is not None and not self.marked:
            raise RuntimeError("marker missing")

    def mark(self, item, marker):
        self.marked = True

    def unmark(self, item, marker):
        self.marked = False


def write_capture(root, operation_id):
    data = fake_png()
    (root / (operation_id + ".partial.png")).write_bytes(data)
    return {"width": 4, "height": 3, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "journal").mkdir()
        self.addCleanup(self.temp.cleanup)
        self.api = FakeApi()
        self.id = str(uuid.uuid4())

    def test_success_is_reused_without_second_capture(self):
        runner = mock.Mock(side_effect=lambda root, operation_id, *_: write_capture(root, operation_id))
        first = capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        second = capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        self.assertEqual(first, second)
        self.assertEqual(first["status"], "ok")
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(Path(first["image_path"]).parent, self.root)
        self.assertFalse(self.api.marked)

    def test_same_uuid_different_request_is_rejected(self):
        runner = lambda root, operation_id, *_: write_capture(root, operation_id)
        capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        other = identity(); other["target"]["hwnd"] = 103
        with self.assertRaisesRegex(RuntimeError, "different request"):
            capture.capture(self.root, self.id, other, 15, self.api, runner)

    def test_unknown_never_retries_or_adopts_partial(self):
        def interrupted(root, operation_id, *_):
            write_capture(root, operation_id)
            raise capture.UnknownCapture()
        runner = mock.Mock(side_effect=interrupted)
        first = capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        second = capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        self.assertEqual(first["status"], "unknown")
        self.assertEqual(first, second)
        self.assertEqual(runner.call_count, 1)
        self.assertFalse((self.root / (self.id + ".png")).exists())
        self.assertFalse((self.root / (self.id + ".partial.png")).exists())

    def test_pending_crash_is_unknown_and_not_replayed(self):
        path = self.root / "journal" / (self.id + ".json")
        capture.atomic_json(path, {"id": self.id, "request": {"identity": identity(), "timeout_seconds": 15}, "status": "pending"})
        runner = mock.Mock()
        result = capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["code"], "interrupted")
        runner.assert_not_called()

    def test_modified_success_becomes_unknown_not_recaptured(self):
        runner = mock.Mock(side_effect=lambda root, operation_id, *_: write_capture(root, operation_id))
        capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        (self.root / (self.id + ".png")).write_bytes(b"changed")
        result = capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["code"], "result_missing_or_modified")
        self.assertEqual(runner.call_count, 1)

    def test_window_change_before_adoption_discards_image(self):
        def switched(root, operation_id, *_):
            result = write_capture(root, operation_id)
            self.api.fail_on = len(self.api.validations) + 1
            return result
        result = capture.capture(self.root, self.id, identity(), 15, self.api, switched)
        self.assertEqual(result["status"], "error")
        self.assertFalse((self.root / (self.id + ".png")).exists())
        self.assertFalse((self.root / (self.id + ".partial.png")).exists())

    def test_unjournalled_existing_output_is_not_overwritten(self):
        path = self.root / (self.id + ".png"); path.write_bytes(b"existing")
        with self.assertRaisesRegex(RuntimeError, "without a journal"):
            capture.capture(self.root, self.id, identity(), 15, self.api, mock.Mock())
        self.assertEqual(path.read_bytes(), b"existing")

    def test_concurrent_attempt_cannot_start_a_second_capture(self):
        runner = mock.Mock()
        with capture.directory_lock(self.root):
            with self.assertRaises((RuntimeError, OSError)):
                capture.capture(self.root, self.id, identity(), 15, self.api, runner)
        runner.assert_not_called()

    def test_png_hash_and_dimensions_are_verified(self):
        data = fake_png(); path = self.root / "x.png"; path.write_bytes(data)
        receipt = {"width": 4, "height": 3, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        capture.inspect_png(path, receipt)
        for field, value in (("width", 5), ("size", 0), ("sha256", "bad")):
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                capture.inspect_png(path, {**receipt, field: value})

    def test_truncated_png_is_rejected_even_if_hash_matches(self):
        data = fake_png()[:-8]; path = self.root / "truncated.png"; path.write_bytes(data)
        receipt = {"width": 4, "height": 3, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        with self.assertRaises(RuntimeError):
            capture.inspect_png(path, receipt)


class FakePixels:
    shape = (3, 4, 4)
    dtype = "uint8"


class FakeFrame:
    width, height, frame_buffer = 4, 3, FakePixels()


class FakeEncoded:
    size = len(fake_png())
    def tobytes(self):
        return fake_png()


class FakeCapture:
    calls = []
    def __init__(self, **kwargs):
        self.calls.append(kwargs)
        self.handlers = {}

    def event(self, function):
        self.handlers[function.__name__] = function
        return function

    def start(self):
        self.handlers["on_frame_arrived"](FakeFrame(), type("Control", (), {"stop": lambda self: None})())


class HelperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "MRFPS" / "WindowCapture"
        self.root.mkdir(parents=True)
        self.api = FakeApi(); self.api.marked = True
        self.id = str(uuid.uuid4()); self.marker = capture.marker_for(self.id)
        self.request = {"id": self.id, "identity": identity(), "marker": self.marker, "root": str(self.root)}
        FakeCapture.calls = []

    def test_explicit_hwnd_only_and_child_validation_at_each_stage(self):
        cv2 = type("Encoder", (), {"imencode": staticmethod(lambda *_: (True, FakeEncoded()))})
        with mock.patch.dict(os.environ, {"LOCALAPPDATA": self.temp.name}), \
             mock.patch.object(capture_helper, "require_ack") as ack:
            result = capture_helper.run(self.request, self.api, FakeCapture, cv2)
        self.assertEqual(result["event"], "saved")
        self.assertEqual(FakeCapture.calls[0]["window_hwnd"], 102)
        self.assertIsNone(FakeCapture.calls[0]["window_name"])
        self.assertIsNone(FakeCapture.calls[0]["monitor_index"])
        self.assertGreaterEqual(len(self.api.validations), 5)
        self.assertEqual([call.args[0] for call in ack.call_args_list], ["frame", "save"])
        self.assertTrue((self.root / (self.id + ".partial.png")).is_file())

    def test_missing_or_zero_hwnd_never_constructs_library(self):
        for value in (None, 0, -1, True):
            item = identity(); item["target"]["hwnd"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                capture_helper.build_capture(FakeCapture, item, self.marker, self.api)
        self.assertEqual(FakeCapture.calls, [])

    def test_child_rejects_window_change_before_saving(self):
        cv2 = type("Encoder", (), {"imencode": staticmethod(lambda *_: (True, FakeEncoded()))})
        self.api.fail_on = 4
        with mock.patch.dict(os.environ, {"LOCALAPPDATA": self.temp.name}), \
             mock.patch.object(capture_helper, "require_ack"):
            result = capture_helper.run(self.request, self.api, FakeCapture, cv2)
        self.assertEqual(result["event"], "error")
        self.assertFalse((self.root / (self.id + ".partial.png")).exists())

    def test_other_directory_is_rejected(self):
        self.request["root"] = str(Path(self.temp.name) / "other")
        with mock.patch.dict(os.environ, {"LOCALAPPDATA": self.temp.name}), self.assertRaises(ValueError):
            capture_helper.run(self.request, self.api, FakeCapture, object())

    def test_unexpected_frame_shape_is_rejected_before_save(self):
        cv2 = type("Encoder", (), {"imencode": staticmethod(lambda *_: (True, FakeEncoded()))})
        with mock.patch.dict(os.environ, {"LOCALAPPDATA": self.temp.name}), \
             mock.patch.object(capture_helper, "require_ack"), \
             mock.patch.object(FakePixels, "shape", (3000, 4000, 4)):
            result = capture_helper.run(self.request, self.api, FakeCapture, cv2)
        self.assertEqual(result["event"], "error")
        self.assertFalse((self.root / (self.id + ".partial.png")).exists())


class ProtocolTests(unittest.TestCase):
    def test_parent_validates_each_helper_stage(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root / "venv" / "Scripts").mkdir(parents=True)
            (root / "venv" / "Scripts" / "python.exe").touch()
            data = fake_png()
            result = {"event": "saved", "width": 4, "height": 3, "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()}
            messages = '\n'.join(json.dumps(v) for v in ({"event": "frame"}, {"event": "save"}, result)) + '\n'
            process = mock.Mock()
            process.stdout = io.StringIO(messages); process.stdin = io.StringIO(); process.wait.return_value = 0
            job = mock.Mock(); api = FakeApi(); api.marked = True
            with mock.patch.object(capture.subprocess, "Popen", return_value=process):
                receipt = capture.supervise(root, str(uuid.uuid4()), identity(), {"name": "x", "value": 1}, 5,
                                            api, memory_check=lambda: True, job_factory=lambda: job)
            self.assertEqual(receipt["width"], 4)
            self.assertGreaterEqual(len(api.validations), 4)
            self.assertEqual([json.loads(line).get("ack") for line in process.stdin.getvalue().splitlines()[1:]], ["frame", "save"])
            job.assign.assert_called_once_with(process)
            job.close.assert_called_once()

    def test_parent_blocks_missing_hwnd_before_start(self):
        for value in (None, 0, -1, True):
            item = identity(); item["target"]["hwnd"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                capture.validate_explicit_identity(item)

    def test_canonical_uuid_required(self):
        for value in ("", "../../x", str(uuid.uuid1()), str(uuid.uuid4()).upper()):
            with self.subTest(value=value), self.assertRaises(ValueError):
                capture.valid_id(value)

    def test_invalid_helper_protocol_is_unknown_and_closes_owned_job(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root / "venv" / "Scripts").mkdir(parents=True)
            (root / "venv" / "Scripts" / "python.exe").touch()
            process = mock.Mock(); process.stdout = io.StringIO('{"event":"save"}\n')
            process.stdin = io.StringIO(); process.wait.return_value = 0
            job = mock.Mock(); api = FakeApi(); api.marked = True
            with mock.patch.object(capture.subprocess, "Popen", return_value=process), self.assertRaises(capture.UnknownCapture):
                capture.supervise(root, str(uuid.uuid4()), identity(), {"name": "x", "value": 1}, 5,
                                  api, memory_check=lambda: True, job_factory=lambda: job)
            job.close.assert_called_once()
            process.terminate.assert_not_called()

    def test_resource_rejection_never_starts_helper(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root / "venv" / "Scripts").mkdir(parents=True)
            (root / "venv" / "Scripts" / "python.exe").touch()
            with mock.patch.object(capture.subprocess, "Popen") as spawn, self.assertRaisesRegex(RuntimeError, "memory"):
                capture.supervise(root, str(uuid.uuid4()), identity(), {"name": "x", "value": 1}, 5,
                                  FakeApi(), memory_check=lambda: False, job_factory=mock.Mock())
            spawn.assert_not_called()

    def test_unresponsive_helper_is_terminated_by_its_own_handle(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root / "venv" / "Scripts").mkdir(parents=True)
            (root / "venv" / "Scripts" / "python.exe").touch()
            process = mock.Mock(); process.stdout = io.StringIO(""); process.stdin = io.StringIO()
            process.wait.side_effect = [capture.subprocess.TimeoutExpired("helper", 3), 0]
            job = mock.Mock(); api = FakeApi(); api.marked = True
            with mock.patch.object(capture.subprocess, "Popen", return_value=process), self.assertRaises(capture.UnknownCapture):
                capture.supervise(root, str(uuid.uuid4()), identity(), {"name": "x", "value": 1}, 5,
                                  api, memory_check=lambda: True, job_factory=lambda: job)
            job.close.assert_called_once()
            process.terminate.assert_called_once()


class PropertyUser:
    def __init__(self):
        self.props = {}
    def GetPropW(self, hwnd, name):
        return self.props.get((hwnd, name), 0)
    def SetPropW(self, hwnd, name, value):
        self.props[(hwnd, name)] = value
        return 1
    def RemovePropW(self, hwnd, name):
        return self.props.pop((hwnd, name), 0)


class WindowIdentityTests(unittest.TestCase):
    def setUp(self):
        self.api = win_identity.Windows.__new__(win_identity.Windows)
        self.api.user = PropertyUser()
        self.raw = {key: identity()[key] for key in ("process", "main", "target")}
        self.api.snapshot = mock.Mock(return_value=self.raw)
        self.stable = capture.marker_for(str(uuid.uuid4()), "MRFPS-Identity-")

    def test_identify_binds_both_windows_and_reuse_is_rejected(self):
        item = self.api.bind(self.raw, self.stable)
        self.api.validate(item)
        # The HWND was recycled for a different window with the same visible
        # metadata; the per-window property is gone on the replacement.
        del self.api.user.props[(item["target"]["hwnd"], self.stable["name"])]
        with self.assertRaisesRegex(RuntimeError, "closed or replaced"):
            self.api.validate(item)

    def test_existing_property_is_never_removed_by_failed_bind(self):
        self.api.user.props[(101, self.stable["name"])] = self.stable["value"]
        with self.assertRaisesRegex(RuntimeError, "already exists"):
            self.api.bind(self.raw, self.stable)
        self.assertEqual(self.api.user.props[(101, self.stable["name"])], self.stable["value"])

    def test_changed_process_start_time_is_rejected(self):
        item = self.api.bind(self.raw, self.stable)
        changed = {**self.raw, "process": {**self.raw["process"], "created": 999}}
        self.api.snapshot.return_value = changed
        with self.assertRaisesRegex(RuntimeError, "identity changed"):
            self.api.validate(item)

    def test_marker_tampering_is_rejected(self):
        item = self.api.bind(self.raw, self.stable)
        item["window_marker"] = {**self.stable, "value": self.stable["value"] + 1}
        with self.assertRaises(ValueError):
            self.api.validate(item)

    def test_duplicate_title_in_another_window_is_rejected(self):
        class User:
            def IsWindow(self, hwnd): return True
            def IsWindowVisible(self, hwnd): return True
            def IsIconic(self, hwnd): return False
            def GetAncestor(self, hwnd, flag): return hwnd
            def GetWindowThreadProcessId(self, hwnd, out):
                ctypes.cast(out, ctypes.POINTER(wintypes.DWORD))[0] = 42
                return 7
            def GetClassNameW(self, hwnd, out, size):
                out.value = "UnrealWindow"
                return len(out.value)
            def GetWindowRect(self, hwnd, out):
                rect = ctypes.cast(out, ctypes.POINTER(wintypes.RECT)).contents
                rect.left = 0; rect.top = 0; rect.right = 800; rect.bottom = 600
                return True
            def EnumWindows(self, callback, _):
                callback(102, 0); callback(103, 0)
                return True
        self.api.user = User()
        self.api.callback_type = lambda function: function
        self.api.title = lambda hwnd: "MRFPS - P1"
        with self.assertRaisesRegex(RuntimeError, "ambiguous"):
            self.api.window(102, 42)

    def test_minimized_identity_is_opt_in_and_does_not_enable_capture(self):
        class User:
            def IsWindow(self, hwnd): return True
            def IsWindowVisible(self, hwnd): return True
            def IsIconic(self, hwnd): return True
            def GetAncestor(self, hwnd, flag): return hwnd
            def GetWindowThreadProcessId(self, hwnd, out):
                ctypes.cast(out, ctypes.POINTER(wintypes.DWORD))[0] = 42
                return 7
            def GetClassNameW(self, hwnd, out, size):
                out.value = "UnrealWindow"
                return len(out.value)
            def GetWindowRect(self, hwnd, out):
                rect = ctypes.cast(out, ctypes.POINTER(wintypes.RECT)).contents
                rect.left = -32000; rect.top = -32000; rect.right = -31840; rect.bottom = -31973
                return True
            def EnumWindows(self, callback, _):
                callback(102, 0)
                return True
        self.api.user = User()
        self.api.callback_type = lambda function: function
        self.api.title = lambda hwnd: "MRFPS - P1"
        with self.assertRaisesRegex(RuntimeError, "minimized"):
            self.api.window(102, 42)
        self.assertEqual(self.api.window(102, 42, allow_minimized=True)["hwnd"], 102)


class CommandDispatchTests(unittest.TestCase):
    def test_identify_and_release_dispatch_to_their_operations(self):
        root = Path(tempfile.gettempdir())
        identity_id = str(uuid.uuid4())
        api = object()
        for command in ("identify", "release"):
            argv = ["capture.py", command, "--identity-id", identity_id]
            if command == "identify":
                argv += ["--pid", "42", "--main-hwnd", "101", "--target-hwnd", "102"]
            output = io.StringIO()
            with self.subTest(command=command), mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(capture, "root_directory", return_value=root), \
                 mock.patch.object(capture, "prepare_directory"), \
                 mock.patch.object(win_identity, "Windows", return_value=api), \
                 mock.patch.object(capture, "identify", return_value={"status": "ok"}) as identify_call, \
                 mock.patch.object(capture, "release", return_value={"status": "released"}) as release_call, \
                 redirect_stdout(output):
                self.assertEqual(capture.main(), 0)
                if command == "identify":
                    identify_call.assert_called_once_with(root, identity_id, 42, 101, 102, api, allow_minimized=False)
                    release_call.assert_not_called()
                else:
                    release_call.assert_called_once_with(root, identity_id, api)
                    identify_call.assert_not_called()


class IdentityJournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); (self.root / "identities").mkdir()
        self.api = win_identity.Windows.__new__(win_identity.Windows); self.api.user = PropertyUser()
        self.raw = {key: identity()[key] for key in ("process", "main", "target")}
        self.api.snapshot = mock.Mock(return_value=self.raw)
        self.id = str(uuid.uuid4())

    def test_identify_and_release_are_idempotent(self):
        first = capture.identify(self.root, self.id, 42, 101, 102, self.api)
        second = capture.identify(self.root, self.id, 42, 101, 102, self.api)
        self.assertEqual(first, second)
        self.assertEqual(first["status"], "ok")
        self.assertEqual(len(self.api.user.props), 2)
        released = capture.release(self.root, self.id, self.api)
        self.assertEqual(released["status"], "released")
        self.assertEqual(capture.release(self.root, self.id, self.api), released)
        self.assertEqual(len(self.api.user.props), 0)
        self.assertEqual(capture.identify(self.root, self.id, 42, 101, 102, self.api)["status"], "released")
        self.assertEqual(len(self.api.user.props), 0)

    def test_opt_in_minimized_identify_and_existing_identity_replay(self):
        first = capture.identify(self.root, self.id, 42, 101, 102, self.api, allow_minimized=True)
        self.assertEqual(first["status"], "ok")
        self.assertTrue(first["request"]["allow_minimized"])
        self.api.snapshot.assert_called_with(42, 101, 102, allow_minimized=True)
        self.assertEqual(capture.identify(self.root, self.id, 42, 101, 102, self.api,
                                          allow_minimized=True), first)
        with self.assertRaisesRegex(RuntimeError, "different window"):
            capture.identify(self.root, self.id, 42, 101, 102, self.api)

    def test_normal_identity_replay_allows_window_to_be_minimized(self):
        first = capture.identify(self.root, self.id, 42, 101, 102, self.api)
        self.assertEqual(capture.identify(self.root, self.id, 42, 101, 102, self.api), first)
        self.api.snapshot.assert_called_with(42, 101, 102, allow_minimized=True)

    def test_identity_id_cannot_be_reassigned(self):
        capture.identify(self.root, self.id, 42, 101, 102, self.api)
        with self.assertRaisesRegex(RuntimeError, "different window"):
            capture.identify(self.root, self.id, 42, 101, 103, self.api)

    def test_interrupted_identify_is_unknown_and_removes_own_marker(self):
        stable = capture.marker_for(self.id, "MRFPS-Identity-")
        item = self.api.bind(self.raw, stable)
        state = {"identity_id": self.id, "request": {"pid": 42, "main_hwnd": 101, "target_hwnd": 102},
                 "status": "pending", "identity": item}
        capture.atomic_json(self.root / "identities" / (self.id + ".json"), state)
        result = capture.identify(self.root, self.id, 42, 101, 102, self.api)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(len(self.api.user.props), 0)
        self.assertEqual(capture.identify(self.root, self.id, 42, 101, 102, self.api), result)

    def test_reused_hwnd_invalidates_existing_identity(self):
        result = capture.identify(self.root, self.id, 42, 101, 102, self.api)
        stable = result["identity"]["window_marker"]
        del self.api.user.props[(102, stable["name"])]
        repeated = capture.identify(self.root, self.id, 42, 101, 102, self.api)
        self.assertEqual(repeated["status"], "unknown")
        self.assertEqual(len(self.api.user.props), 0)


if __name__ == "__main__":
    unittest.main()
