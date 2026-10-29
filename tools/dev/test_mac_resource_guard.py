"""Portable checks for macOS memory reports; no compiler or app is launched."""
import pathlib
import json
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import Mock, call, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import mac_resource_guard as guard


class MemoryReportTests(unittest.TestCase):
    def test_apple_silicon_and_intel_page_sizes(self):
        for page in (4096, 16384):
            report = f"""Mach Virtual Memory Statistics: (page size of {page} bytes)
Pages free:                               100.
Pages active:                             999.
Pages inactive:                           200.
Pages speculative:                         50.
Pages wired down:                         300.
Pages occupied by compressor:             400.
"""
            with self.subTest(page=page), patch.object(guard.subprocess, "check_output", return_value=report):
                self.assertEqual(guard.available_memory(), 350*page)

    def test_missing_measurements_fail_closed(self):
        for report in ("", "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free: 100."):
            with self.subTest(report=report), patch.object(guard.subprocess, "check_output", return_value=report):
                with self.assertRaises(RuntimeError):
                    guard.available_memory()

    def test_stop_request_accepts_only_owner_only_matching_instance(self):
        marker = "a" * 32
        payload = json.dumps({"action": "stop", "instance": marker}).encode("ascii")
        info = SimpleNamespace(st_mode=0o100600, st_uid=123, st_nlink=1, st_size=len(payload))
        with patch.object(guard.os, "open", return_value=9), patch.object(guard.os, "close"), \
             patch.object(guard.os, "getuid", return_value=123, create=True), \
             patch.object(guard.os, "fstat", return_value=info), patch.object(guard.os, "read", return_value=payload):
            self.assertTrue(guard.stop_requested(pathlib.Path("unused"), marker))
            with self.assertRaisesRegex(RuntimeError, "invalid stop"):
                guard.stop_requested(pathlib.Path("unused"), "b" * 32)
            info.st_mode = 0o100644
            with self.assertRaisesRegex(RuntimeError, "unsafe stop"):
                guard.stop_requested(pathlib.Path("unused"), marker)
            info.st_mode = 0o100600
            info.st_uid = 124
            with self.assertRaisesRegex(RuntimeError, "unsafe stop"):
                guard.stop_requested(pathlib.Path("unused"), marker)

    def test_invalid_reserve_never_starts_a_child(self):
        for value in ("nan", "inf", "0", "-1", "16"):
            with self.subTest(value=value), patch.object(guard.sys, "platform", "darwin"), \
                 patch.object(guard.sys, "argv", ["guard", "--reserve-gb", value, "--", "compiler"]), \
                 patch.object(guard.subprocess, "check_output", return_value=str(16*2**30)), \
                 patch.object(guard, "available_memory", return_value=12*2**30), \
                 patch.object(guard.subprocess, "Popen") as launch:
                with self.assertRaises(RuntimeError):
                    guard.main()
                launch.assert_not_called()

    def test_requested_stop_writes_safe_report_and_only_signals_own_group(self):
        with tempfile.TemporaryDirectory(prefix="mrfps-guard-report-") as temp:
            root = pathlib.Path(temp)
            report = root / "guard.json"
            request = root / "stop.json"
            child = Mock(pid=4567, returncode=0)
            child.poll.side_effect = [None, None, 0]
            kill_signal = getattr(guard.signal, "SIGKILL", 9)
            args = ["guard", "--report-file", str(report), "--stop-file", str(request),
                    "--stop-id", "a" * 32, "--", "DO_NOT_LOG_COMMAND"]
            with patch.object(guard.sys, "platform", "darwin"), patch.object(guard.sys, "argv", args), \
                 patch.object(guard.subprocess, "check_output", return_value=str(16*2**30)), \
                 patch.object(guard, "available_memory", return_value=12*2**30), \
                 patch.object(guard, "stop_requested", return_value=True), \
                 patch.object(guard.subprocess, "Popen", return_value=child), \
                 patch.object(guard.signal, "SIGKILL", kill_signal, create=True), \
                 patch.object(guard.os, "killpg", create=True) as killpg, redirect_stdout(io.StringIO()):
                self.assertEqual(guard.main(), 0)
            saved = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(saved["status"], "stopped")
            self.assertEqual(saved["reason"], "requested_stop")
            self.assertEqual(saved["exit_code"], 0)
            self.assertNotIn("DO_NOT_LOG_COMMAND", json.dumps(saved))
            self.assertEqual(killpg.call_args_list, [call(4567, guard.signal.SIGTERM), call(4567, kill_signal)])

    def test_memory_limit_is_recorded_and_does_not_stop_other_groups(self):
        with tempfile.TemporaryDirectory(prefix="mrfps-guard-limit-") as temp:
            report = pathlib.Path(temp) / "guard.json"
            child = Mock(pid=5678, returncode=0)
            child.poll.side_effect = [None, None, 0]
            kill_signal = getattr(guard.signal, "SIGKILL", 9)
            args = ["guard", "--report-file", str(report), "--", "fixture-child"]
            with patch.object(guard.sys, "platform", "darwin"), patch.object(guard.sys, "argv", args), \
                 patch.object(guard.subprocess, "check_output", return_value=str(16*2**30)), \
                 patch.object(guard, "available_memory", side_effect=[12*2**30, 2*2**30]), \
                 patch.object(guard.subprocess, "Popen", return_value=child), \
                 patch.object(guard.signal, "SIGKILL", kill_signal, create=True), \
                 patch.object(guard.os, "killpg", create=True) as killpg, \
                 redirect_stdout(io.StringIO()), patch.object(guard.sys, "stderr", io.StringIO()):
                self.assertEqual(guard.main(), 125)
            saved = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(saved["reason"], "memory_limit")
            self.assertEqual(saved["minimum_available_gb"], 2)
            self.assertEqual(killpg.call_args_list, [call(5678, guard.signal.SIGTERM), call(5678, kill_signal)])

    def test_preflight_failure_is_reported_without_launch(self):
        with tempfile.TemporaryDirectory(prefix="mrfps-guard-preflight-") as temp:
            report = pathlib.Path(temp) / "guard.json"
            with patch.object(guard.sys, "platform", "darwin"), \
                 patch.object(guard.sys, "argv", ["guard", "--report-file", str(report), "--", "fixture-child"]), \
                 patch.object(guard.subprocess, "check_output", return_value=str(16*2**30)), \
                 patch.object(guard, "available_memory", return_value=2*2**30), \
                 patch.object(guard.subprocess, "Popen") as launch:
                with self.assertRaises(RuntimeError):
                    guard.main()
                launch.assert_not_called()
            saved = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(saved["status"], "failed")
            self.assertEqual(saved["reason"], "insufficient_memory")

    def test_cleanup_failure_does_not_report_a_successful_stop(self):
        with tempfile.TemporaryDirectory(prefix="mrfps-guard-cleanup-") as temp:
            report = pathlib.Path(temp) / "guard.json"
            child = Mock(pid=6789, returncode=0)
            child.poll.side_effect = [None, None, 0]
            kill_signal = getattr(guard.signal, "SIGKILL", 9)
            with patch.object(guard.sys, "platform", "darwin"), \
                 patch.object(guard.sys, "argv", ["guard", "--report-file", str(report), "--", "fixture-child"]), \
                 patch.object(guard.subprocess, "check_output", return_value=str(16*2**30)), \
                 patch.object(guard, "available_memory", side_effect=[12*2**30, 2*2**30]), \
                 patch.object(guard.subprocess, "Popen", return_value=child), \
                 patch.object(guard.signal, "SIGKILL", kill_signal, create=True), \
                 patch.object(guard.os, "killpg", side_effect=PermissionError("fixture"), create=True), \
                 redirect_stdout(io.StringIO()), patch.object(guard.sys, "stderr", io.StringIO()):
                self.assertEqual(guard.main(), 125)
            saved = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(saved["reason"], "cleanup_failed")


if __name__ == "__main__":
    unittest.main()
