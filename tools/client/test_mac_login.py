"""POSIX fixture tests; do not start UE, authenticate or access a Mac device."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock
import uuid

sys.path.insert(0, str(Path(__file__).parent))
spec = importlib.util.spec_from_file_location("mac_login", Path(__file__).with_name("mac_login.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


@unittest.skipUnless(os.name == "posix", "POSIX permissions and flock are required")
class MacLoginTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mrfps-mac-login-")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.environment = mock.patch.dict(os.environ, {"HOME": str(self.home)})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.code = self.home / "fixture-code"
        self.code.write_bytes(b"000123\n")
        self.args = argparse.Namespace(name="MRFPS-Fixture", server="127.0.0.1:7777", role="inspector",
                                       code_file=self.code, operation_id=str(uuid.uuid4()))

    def test_queue_permissions_same_operation_and_changed_intent(self):
        with mock.patch.object(module, "login_is_open", return_value=True):
            first = module.queue(self.args)
            self.assertEqual(first["status"], "queued")
            controls, state = module.private_directories()
            target = controls / (self.args.name + ".json")
            self.assertEqual(json.loads(target.read_text())["code"], "000123")
            self.assertEqual(stat.S_IMODE(controls.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE((state / self.args.name).stat().st_mode), 0o700)
            self.assertNotIn("000123", json.dumps(first))
            self.assertEqual(module.queue(self.args)["status"], "already_queued")
            self.code.write_bytes(b"000124")
            with self.assertRaisesRegex(ValueError, "different input"):
                module.queue(self.args)
            self.assertEqual(json.loads(target.read_text())["code"], "000123")

    def test_pending_target_never_overwritten_and_can_retry(self):
        with mock.patch.object(module, "login_is_open", return_value=True):
            module.queue(self.args)
            controls, _ = module.private_directories()
            target = controls / (self.args.name + ".json")
            second = argparse.Namespace(**vars(self.args))
            second.operation_id = str(uuid.uuid4())
            self.code.write_bytes(b"000124")
            with self.assertRaisesRegex(RuntimeError, "still queued"):
                module.queue(second)
            self.assertEqual(json.loads(target.read_text())["code"], "000123")
            target.unlink()  # Fixture emulates the game atomically claiming it.
            self.assertEqual(module.queue(second)["status"], "queued")

    def test_cancel_removes_only_its_own_pending_instruction(self):
        with mock.patch.object(module, "login_is_open", return_value=True):
            module.queue(self.args)
            controls, _ = module.private_directories()
            target = controls / (self.args.name + ".json")
            self.assertEqual(module.cancel(self.args)["status"], "cancelled")
            self.assertFalse(target.exists())
            self.assertEqual(module.cancel(self.args)["status"], "cancelled")
            self.assertEqual(module.queue(self.args)["status"], "cancelled")
            second = argparse.Namespace(**vars(self.args))
            second.operation_id = str(uuid.uuid4())
            self.assertEqual(module.queue(second)["status"], "queued")
            # A cancelled, old UUID must not remove the new owner's instruction.
            self.assertEqual(module.cancel(self.args)["status"], "cancelled")
            self.assertTrue(target.exists())
            target.unlink()  # GUI already claimed the second instruction.
            self.assertEqual(module.cancel(second)["status"], "unknown")

    def test_ambiguous_publish_never_retries(self):
        with mock.patch.object(module, "login_is_open", return_value=True), \
             mock.patch.object(module.os, "link", side_effect=OSError("fixture failure")):
            with self.assertRaises(OSError):
                module.queue(self.args)
        with mock.patch.object(module, "login_is_open") as probe:
            retry = module.queue(self.args)
            self.assertEqual(retry["status"], "unknown")
            probe.assert_not_called()
        controls, _ = module.private_directories()
        self.assertEqual(list(controls.iterdir()), [])
        info = module.status(argparse.Namespace(name=self.args.name, operation_id=self.args.operation_id))
        self.assertEqual(info["status"], "unknown")

    def test_unavailable_login_and_concurrent_lock_fail_closed(self):
        with mock.patch.object(module, "login_is_open", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "login screen"):
                module.queue(self.args)
        _, state = module.private_directories()
        directory = module.client_directory(state, self.args.name)
        with module.state_lock(directory):
            with self.assertRaisesRegex(RuntimeError, "another command"):
                module.queue(self.args)

    def test_symlinked_control_directory_rejected(self):
        base = self.home / "Library/Application Support/MRFPS/DeviceLogin"
        base.mkdir(parents=True)
        (base / "controls").symlink_to(self.home, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "unsafe private directory"):
            module.queue(self.args)

    def test_launch_reuse_unknown_and_recovery(self):
        binary = self.home / "client"
        binary.write_text("fixture", encoding="utf-8")
        binary.chmod(0o700)
        args = argparse.Namespace(name=self.args.name, executable=binary, editor=False)
        child = mock.Mock(pid=12345)
        child.poll.return_value = None
        with mock.patch.object(module.subprocess, "Popen", return_value=child) as popen, \
             mock.patch.object(module, "process_info", return_value="fixture-start"), \
             mock.patch.object(module, "marker_exists", return_value=False), \
             mock.patch.object(module.time, "sleep"):
            self.assertEqual(module.start(args)["status"], "launching")
            self.assertEqual(module.start(args)["status"], "already_running")
            self.assertEqual(popen.call_count, 1)
            command = popen.call_args.args[0]
            self.assertTrue(any("mac_resource_guard.py" in part for part in command))
            self.assertIn("-MRFPSWindowName=" + args.name, command)
            self.assertFalse(any("000123" in part or "MRFPSAuth" in part for part in command))
        _, state = module.private_directories()
        state_file = state / args.name / "process.json"
        saved = module.load_json(state_file)
        saved["status"] = "unknown"
        module.save_json(state_file, saved)
        with mock.patch.object(module, "marker_exists", return_value=True):
            with self.assertRaisesRegex(RuntimeError, "previous client"):
                module.recover(args)
        with mock.patch.object(module, "marker_exists", return_value=False):
            self.assertEqual(module.recover(args)["status"], "recovered")
            self.assertEqual(module.recover(args)["status"], "nothing_to_recover")


if __name__ == "__main__":
    unittest.main()
