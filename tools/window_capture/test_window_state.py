from contextlib import nullcontext
from copy import deepcopy
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest import mock

import capture
import test_capture
import window_state


def uid():
    return str(uuid.uuid4())


class FakeStateAPI:
    def __init__(self):
        self.state = {"rect": {"x": 10, "y": 20, "width": 800, "height": 600},
                      "minimized": False, "maximized": False, "foreground": False,
                      "move_size": False}
        self.actions = []
        self.reads = 0
        self.before_read = None
        self.owned = False
        self.fail_validation = False
        self.apply = True
        self.throw_after_dispatch = False

    def dpi(self):
        return nullcontext()

    def validate(self, identity):
        if self.fail_validation:
            raise RuntimeError("window changed")

    def read(self, identity):
        self.validate(identity)
        self.reads += 1
        if self.before_read:
            self.before_read(self.reads)
        result = deepcopy(self.state)
        self.validate(identity)
        return result

    def has_owned_window(self, hwnd):
        return self.owned

    def dispatch(self, hwnd, desired):
        self.actions.append((hwnd, deepcopy(desired)))
        if self.apply:
            if "rect" in desired:
                self.state["rect"] = deepcopy(desired["rect"])
            elif "minimized" in desired:
                self.state["minimized"] = desired["minimized"]
                self.state["rect"] = None if desired["minimized"] else {
                    "x": 10, "y": 20, "width": 800, "height": 600}
            elif "normal" in desired:
                self.state["minimized"] = self.state["maximized"] = False
                self.state["rect"] = {"x": 10, "y": 20, "width": 800, "height": 600}
            else:
                self.state["foreground"] = True
        if self.throw_after_dispatch:
            raise RuntimeError("lost response")


class WindowStateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("identities", "window-state", "window-state/snapshots", "window-state/operations"):
            (self.root / name).mkdir()
        self.identity = test_capture.identity()
        self.identity_id = uid()
        capture.atomic_json(self.root / "identities" / (self.identity_id + ".json"),
                            {"identity_id": self.identity_id, "status": "ok", "identity": self.identity})
        self.api = FakeStateAPI()

    def inspect(self):
        snapshot_id = uid()
        result = window_state.snapshot(self.root, self.identity_id, snapshot_id, self.api)
        return snapshot_id, result

    def execute(self, snapshot_id, desired, operation_id=None, **kwargs):
        return window_state.ensure(self.root, self.identity_id, snapshot_id, operation_id or uid(),
                                   desired, self.api, **kwargs)

    def test_rectangle_is_target_only_and_same_id_never_repeats(self):
        snapshot_id, _ = self.inspect()
        op = uid()
        desired = {"rect": {"x": -100, "y": 20, "width": 960, "height": 540}}
        first = self.execute(snapshot_id, desired, op)
        self.assertEqual(first["status"], "ok")
        self.assertEqual(first["code"], "applied")
        self.assertEqual(self.api.actions, [(102, desired)])
        self.api.state["rect"]["x"] = 777
        self.assertEqual(self.execute(snapshot_id, desired, op), first)
        self.assertEqual(len(self.api.actions), 1)
        with self.assertRaisesRegex(RuntimeError, "different request"):
            self.execute(snapshot_id, {"minimized": True}, op)

    def test_already_applied_does_not_dispatch(self):
        snapshot_id, _ = self.inspect()
        result = self.execute(snapshot_id, {"rect": deepcopy(self.api.state["rect"])})
        self.assertEqual(result["code"], "already_applied")
        self.assertFalse(self.api.actions)

    def test_snapshot_reuse_is_immutable_and_cross_identity_rejected(self):
        snapshot_id, first = self.inspect()
        self.api.state["rect"]["x"] = 900
        self.assertEqual(window_state.snapshot(self.root, self.identity_id, snapshot_id, self.api), first)
        with self.assertRaisesRegex(RuntimeError, "different request"):
            window_state.snapshot(self.root, uid(), snapshot_id, self.api)

    def test_foreground_release_is_not_faked_and_does_not_choose_another_window(self):
        self.api.state["foreground"] = True
        snapshot_id, _ = self.inspect()
        result = self.execute(snapshot_id, {"foreground": False})
        self.assertEqual((result["status"], result["code"]), ("error", "foreground_release_unsupported"))
        self.assertFalse(self.api.actions)
        self.api.state["foreground"] = False
        snapshot_id, _ = self.inspect()
        self.assertEqual(self.execute(snapshot_id, {"foreground": False})["code"], "already_applied")
        self.assertFalse(self.api.actions)

    def test_foreground_only_when_explicit(self):
        snapshot_id, _ = self.inspect()
        self.assertEqual(self.execute(snapshot_id, {"foreground": True})["code"], "applied")
        self.assertEqual(self.api.actions, [(102, {"foreground": True})])

    def test_minimize_foreground_or_owner_is_refused(self):
        self.api.state["foreground"] = True
        snapshot_id, _ = self.inspect()
        self.assertEqual(self.execute(snapshot_id, {"minimized": True})["code"],
                         "minimize_foreground_unsupported")
        self.api.state["foreground"] = False; self.api.owned = True
        snapshot_id, _ = self.inspect()
        self.assertEqual(self.execute(snapshot_id, {"minimized": True})["code"],
                         "owned_windows_prevent_operation")
        self.assertFalse(self.api.actions)

    def test_minimize_restore_do_not_request_foreground(self):
        snapshot_id, _ = self.inspect()
        self.assertEqual(self.execute(snapshot_id, {"minimized": True})["status"], "ok")
        self.assertTrue(self.api.state["minimized"])
        snapshot_id, _ = self.inspect()
        self.assertEqual(self.execute(snapshot_id, {"minimized": False})["status"], "ok")
        self.assertFalse(self.api.state["foreground"])
        self.assertEqual(self.api.actions, [(102, {"minimized": True}), (102, {"minimized": False})])

    def test_restore_required_for_minimized_or_maximized_rectangle(self):
        for name in ("minimized", "maximized"):
            self.api.state[name] = True
            snapshot_id, _ = self.inspect()
            result = self.execute(snapshot_id, {"rect": {"x": 0, "y": 0, "width": 800, "height": 600}})
            self.assertEqual(result["code"], "restore_before_setting_rectangle")
            self.api.state[name] = False
        self.assertFalse(self.api.actions)

    def test_normal_restores_a_maximized_window_without_focus(self):
        self.api.state["maximized"] = True
        snapshot_id, _ = self.inspect()
        result = self.execute(snapshot_id, {"normal": True})
        self.assertEqual(result["status"], "ok")
        self.assertFalse(self.api.state["maximized"])
        self.assertFalse(self.api.state["foreground"])
        self.assertEqual(self.api.actions, [(102, {"normal": True})])

    def test_owned_window_blocks_all_mutations(self):
        self.api.owned = True
        for desired in ({"minimized": True}, {"foreground": True},
                        {"rect": {"x": 0, "y": 0, "width": 800, "height": 600}}):
            snapshot_id, _ = self.inspect()
            with self.subTest(desired=desired):
                self.assertEqual(self.execute(snapshot_id, desired)["code"], "owned_windows_prevent_operation")
        self.assertFalse(self.api.actions)

    def test_owned_window_appearing_after_dispatch_marks_unknown(self):
        snapshot_id, _ = self.inspect()
        reads = self.api.reads
        def change(n):
            if n == reads + 3:
                self.api.owned = True
        self.api.before_read = change
        self.assertEqual(self.execute(snapshot_id, {"minimized": True})["status"], "unknown")
        self.assertEqual(len(self.api.actions), 1)

    def test_lost_response_is_unknown_and_never_retried(self):
        snapshot_id, _ = self.inspect()
        op = uid(); self.api.throw_after_dispatch = True
        result = self.execute(snapshot_id, {"minimized": True}, op)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(self.execute(snapshot_id, {"minimized": True}, op), result)
        self.assertEqual(len(self.api.actions), 1)

    def test_interrupted_pending_is_unknown_without_dispatch(self):
        snapshot_id, _ = self.inspect()
        op = uid(); desired = {"minimized": True}
        pending = {"operation_id": op, "request": {"identity_id": self.identity_id,
                   "snapshot_id": snapshot_id, "desired": desired}, "status": "pending"}
        capture.atomic_json(self.root / "window-state" / "operations" / (op + ".json"), pending)
        result = self.execute(snapshot_id, desired, op)
        self.assertEqual((result["status"], result["code"]), ("unknown", "interrupted"))
        self.assertEqual(self.execute(snapshot_id, desired, op), result)
        self.assertFalse(self.api.actions)

    def test_changed_snapshot_and_move_size_are_unknown(self):
        snapshot_id, _ = self.inspect()
        self.api.state["rect"]["x"] = 11
        result = self.execute(snapshot_id, {"minimized": True})
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(self.api.actions)
        self.api.state["move_size"] = True
        snapshot_id, _ = self.inspect()
        result = self.execute(snapshot_id, {"minimized": True})
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(self.api.actions)

    def test_change_just_before_dispatch_is_unknown(self):
        snapshot_id, _ = self.inspect()
        reads = self.api.reads
        def change(n):
            if n == reads + 2:
                self.api.state["rect"]["x"] += 1
        self.api.before_read = change
        result = self.execute(snapshot_id, {"minimized": True})
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(self.api.actions)

    def test_identity_or_process_loss_is_unknown(self):
        snapshot_id, _ = self.inspect()
        self.api.fail_validation = True
        result = self.execute(snapshot_id, {"minimized": True})
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(self.api.actions)

    def test_lost_window_after_dispatch_is_unknown(self):
        snapshot_id, _ = self.inspect()
        reads = self.api.reads
        def disappear(n):
            if n == reads + 3:
                self.api.fail_validation = True
        self.api.before_read = disappear
        result = self.execute(snapshot_id, {"minimized": True})
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(len(self.api.actions), 1)

    def test_queued_action_timeout_is_unknown_and_not_repeated(self):
        with mock.patch.object(window_state.time, "monotonic", return_value=100.0):
            snapshot_id, _ = self.inspect()
        clock = [100.0]
        def sleep(seconds):
            clock[0] += seconds
        op = uid(); self.api.apply = False
        result = self.execute(snapshot_id, {"minimized": True}, op, now=lambda: clock[0], sleep=sleep)
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(self.execute(snapshot_id, {"minimized": True}, op), result)
        self.assertEqual(len(self.api.actions), 1)

    def test_stale_or_future_snapshot_is_unknown(self):
        snapshot_id, _ = self.inspect()
        result = self.execute(snapshot_id, {"minimized": True}, now=lambda: 0)
        self.assertEqual(result["status"], "unknown")
        self.assertFalse(self.api.actions)

    def test_bounds_and_types_are_checked_before_journal(self):
        for desired in ({"rect": {"x": 0, "y": 0, "width": 4097, "height": 600}},
                        {"rect": {"x": 16385, "y": 0, "width": 800, "height": 600}},
                        {"rect": {"x": True, "y": 0, "width": 800, "height": 600}},
                        {"foreground": "true"}, {"minimized": True, "foreground": True}):
            with self.subTest(desired=desired), self.assertRaises(ValueError):
                self.execute(uid(), desired)
        self.assertEqual(list((self.root / "window-state" / "operations").iterdir()), [])

    def test_another_operation_holds_shared_capture_lock(self):
        snapshot_id, _ = self.inspect()
        with capture.directory_lock(self.root):
            with self.assertRaises((RuntimeError, OSError)):
                self.execute(snapshot_id, {"minimized": True})
        self.assertFalse(self.api.actions)

    def test_editor_and_non_mrfps_main_are_rejected(self):
        identity_path = self.root / "identities" / (self.identity_id + ".json")
        for edit in ({"executable": "unrealeditor.exe"}, {"title": "Other"},
                     {"class": "OtherWindow"}):
            item = deepcopy(self.identity)
            item["process" if "executable" in edit else "main"].update(edit)
            capture.atomic_json(identity_path, {"identity_id": self.identity_id, "status": "ok", "identity": item})
            with self.subTest(edit=edit), self.assertRaises(RuntimeError):
                self.inspect()

    def test_position_and_show_commands_have_no_activation_flags(self):
        class User:
            def __init__(self): self.calls = []
            def SetWindowPos(self, *args): self.calls.append(("position", args)); return True
            def ShowWindowAsync(self, *args): self.calls.append(("show", args)); return True
            def SetForegroundWindow(self, *args): self.calls.append(("foreground", args)); return True
        api = window_state.WindowStateAPI.__new__(window_state.WindowStateAPI)
        api.user = User()
        api.dispatch(102, {"rect": {"x": 1, "y": 2, "width": 800, "height": 600}})
        api.dispatch(102, {"minimized": True})
        api.dispatch(102, {"minimized": False})
        api.dispatch(102, {"normal": True})
        self.assertEqual(api.user.calls, [
            ("position", (102, None, 1, 2, 800, 600, window_state.POSITION_FLAGS)),
            ("show", (102, 7)), ("show", (102, 4)), ("show", (102, 4))])
        self.assertEqual(window_state.POSITION_FLAGS & 0x0010, 0x0010)
        self.assertEqual(window_state.POSITION_FLAGS & 0x0004, 0x0004)
        self.assertEqual(window_state.POSITION_FLAGS & 0x4000, 0x4000)

    def test_owned_window_check_follows_owner_chain_without_mutation(self):
        class User:
            def __init__(self): self.candidates = [300]
            def GetWindow(self, hwnd, command):
                self_command = command
                assert self_command == 4
                return {300: 200, 200: 102}.get(hwnd)
            def EnumWindows(self, callback, _):
                return all(callback(hwnd, 0) for hwnd in self.candidates)
        api = window_state.WindowStateAPI.__new__(window_state.WindowStateAPI)
        api.user = User()
        api.identity_api = mock.Mock(callback_type=lambda func: func)
        self.assertTrue(api.has_owned_window(102))
        self.assertFalse(api.has_owned_window(999))


if __name__ == "__main__":
    unittest.main()
