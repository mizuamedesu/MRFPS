import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import shutil
from types import SimpleNamespace
from unittest.mock import patch

import ephemeral_runner as runner


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.guard = self.root / "guard.json"

    def report(self, free=10, commit=75, status="running"):
        self.guard.write_text(json.dumps({"status": status, "lastSample": {"availablePhysicalGB": free, "commitPercent": commit}}))

    def test_exact_guard_boundaries_allow_but_pressure_and_terminal_do_not(self):
        self.report()
        self.assertIsNone(runner.guard_reason(self.guard))
        for free, commit in ((9.99, 75), (10, 75.01)):
            self.report(free, commit)
            self.assertEqual(runner.guard_reason(self.guard), "host_memory_pressure")
        self.report(status="memory_limit_exceeded")
        self.assertEqual(runner.guard_reason(self.guard), "guard_not_running")

    def test_stale_missing_and_partial_guard_are_fail_closed(self):
        self.assertEqual(runner.guard_reason(self.guard), "guard_report_unreadable")
        self.guard.write_text("{")
        self.assertEqual(runner.guard_reason(self.guard), "guard_report_unreadable")
        self.report()
        os.utime(self.guard, (time.time() - 16, time.time() - 16))
        self.assertEqual(runner.guard_reason(self.guard), "guard_heartbeat_stale")

    def test_transient_atomic_replace_read_error_recovers_with_bounded_retry(self):
        self.report()
        report = self.guard.read_text()
        for failure in (FileNotFoundError(2, "fixture"), PermissionError(13, "fixture"), "{"):
            reads = [failure, report]
            def read(*_args, **_kwargs):
                value = reads.pop(0)
                if isinstance(value, Exception):
                    raise value
                return value
            diagnostic = {}
            with self.subTest(failure=type(failure).__name__), patch.object(Path, "read_text", side_effect=read), patch.object(runner.time, "sleep") as sleep:
                self.assertIsNone(runner.guard_reason(self.guard, diagnostic=diagnostic))
                self.assertEqual(diagnostic["read_attempts"], 2)
                sleep.assert_called_once_with(0.025)

    def test_unreadable_guard_remains_fail_closed_after_exactly_eight_reads(self):
        self.report()
        diagnostic = {}
        with patch.object(Path, "read_text", side_effect=PermissionError(13, "fixture")) as read, patch.object(runner.time, "sleep") as sleep:
            self.assertEqual(runner.guard_reason(self.guard, diagnostic=diagnostic), "guard_report_unreadable")
        self.assertEqual(read.call_count, 8)
        self.assertEqual(sleep.call_count, 7)
        self.assertEqual(diagnostic["failures"][-1], {"type": "PermissionError", "errno": 13})

    def test_retry_does_not_extend_heartbeat_or_ignore_pressure(self):
        self.report()
        current = time.time()
        with patch.object(Path, "stat", side_effect=[SimpleNamespace(st_mtime=current), SimpleNamespace(st_mtime=current - 16)]), patch.object(Path, "read_text", side_effect=PermissionError(13, "fixture")), patch.object(runner.time, "sleep"):
            self.assertEqual(runner.guard_reason(self.guard, now=current), "guard_heartbeat_stale")
        pressure = json.dumps({"status": "running", "lastSample": {"availablePhysicalGB": 9.9, "commitPercent": 70}})
        with patch.object(Path, "read_text", side_effect=[PermissionError(13, "fixture"), pressure]), patch.object(runner.time, "sleep"):
            self.assertEqual(runner.guard_reason(self.guard), "host_memory_pressure")

    def test_terminal_or_invalid_samples_cannot_be_accepted(self):
        self.report()
        for payload in ([], {"status": "running", "lastSample": []}, {"status": "running", "lastSample": {"availablePhysicalGB": float("nan"), "commitPercent": 50}}):
            with self.subTest(payload=payload), patch.object(Path, "read_text", return_value=json.dumps(payload)), patch.object(runner.time, "sleep"):
                self.assertEqual(runner.guard_reason(self.guard), "guard_report_unreadable")
        with patch.object(Path, "read_text", return_value=json.dumps({"status": "completed"})):
            self.assertEqual(runner.guard_reason(self.guard), "guard_not_running")

    def test_host_raw_pressure_flag_wins_over_rounded_display_values(self):
        self.report()
        payload = {"status": "running", "lastSample": {"availablePhysicalGB": 10.0, "commitPercent": 75.0,
                   "pressureReason": "available_physical_below_minimum"}}
        with patch.object(Path, "read_text", return_value=json.dumps(payload)):
            self.assertEqual(runner.guard_reason(self.guard), "host_memory_pressure")

    def test_cleanup_rejects_arbitrary_task_paths(self):
        for task in ("../other", "A" * 16, "a" * 15, "a" * 17):
            with self.subTest(task=task), self.assertRaisesRegex(ValueError, "TASK_INVALID"):
                runner.task_root(task)

    def test_starttime_or_process_group_mismatch_cannot_be_killed(self):
        expected = {"pid": 222, "pgid": 222, "start_ticks": 100}
        for actual in ({"pid": 222, "pgid": 222, "start_ticks": 101}, {"pid": 222, "pgid": 200, "start_ticks": 100}, None):
            with self.subTest(actual=actual), patch.object(runner, "process_identity", return_value=actual), patch.object(runner.os, "killpg", create=True) as kill:
                self.assertFalse(runner.stop_group(expected))
                kill.assert_not_called()

    def test_watchdog_cleans_after_guard_lease_expires_without_windows_parent(self):
        task = "a" * 16
        (self.root / "state.json").write_text(json.dumps({"manager": {}, "created_at": time.time()}))
        with patch.object(runner, "task_root", return_value=self.root), patch.object(runner, "guard_reason", return_value="guard_heartbeat_stale"), patch.object(runner, "same_process", return_value=True), patch.object(runner, "cleanup") as cleanup:
            runner.watchdog(task, self.guard)
        cleanup.assert_called_once_with(task, "guard_heartbeat_stale")

    def test_pins_are_complete_sha256_digests(self):
        for value in (runner.RUNNER_HASH, runner.GH_HASH, runner.BUILDKIT.split("@sha256:")[1]):
            self.assertRegex(value, r"^[0-9a-f]{64}$")

    @unittest.skipUnless(os.name == "posix", "actual owned Linux process group")
    def test_actual_own_sleeping_grandchild_is_stopped_and_unrelated_process_survives(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"])
        child_file = self.root / "child.pid"
        owner = subprocess.Popen([sys.executable, "-c", "import subprocess,sys,time,pathlib;p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)']);pathlib.Path(sys.argv[1]).write_text(str(p.pid));time.sleep(30)", str(child_file)], start_new_session=True)
        try:
            for _ in range(100):
                if child_file.exists() and child_file.read_text():
                    break
                time.sleep(0.01)
            child_pid = int(child_file.read_text())
            identity = runner.process_identity(owner.pid)
            self.assertIsNotNone(identity)
            self.assertTrue(runner.stop_group(identity))
            owner.wait(timeout=5)
            child = runner.process_identity(child_pid)
            self.assertTrue(child is None or child["state"] == "Z", "owned grandchild remains running")
            self.assertIsNone(unrelated.poll())
        finally:
            if owner.poll() is None:
                os.killpg(owner.pid, 9)
            unrelated.terminate()
            unrelated.wait(timeout=5)

    @unittest.skipUnless(os.name == "posix", "actual independent Linux watchdog")
    def test_actual_watchdog_reclaims_owned_group_after_guard_heartbeat_stops(self):
        task = uuid.uuid4().hex[:16]
        root = runner.task_root(task)
        root.mkdir(mode=0o700)
        manager = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"], start_new_session=True)
        runtime = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"], start_new_session=True)
        unrelated = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"])
        try:
            fake_bin = self.root / "bin"
            fake_bin.mkdir()
            docker = fake_bin / "docker"
            docker.write_text('#!/bin/sh\nif [ "$1" = inspect ]; then exit 1; fi\nexit 0\n')
            docker.chmod(0o755)
            (root / "runner").mkdir()
            (root / "runner/.credentials").write_text("non-secret credential-removal fixture")
            runner.write_json(root / "state.json", {"task_id": task, "repository": runner.REPOSITORY, "created_at": time.time(),
                "manager": runner.process_identity(manager.pid), "runtime": runner.process_identity(runtime.pid), "builder_claimed": False})
            self.report()
            os.utime(self.guard, (time.time() - 16, time.time() - 16))
            environment = os.environ.copy()
            environment["PATH"] = str(fake_bin) + ":" + environment.get("PATH", "/usr/bin:/bin")
            result = subprocess.run([sys.executable, str(Path(runner.__file__).resolve()), "watch", "--task", task, "--guard", str(self.guard)], env=environment, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            cleanup = json.loads((root / "cleanup.json").read_text())
            self.assertTrue(cleanup["passed"])
            self.assertEqual(cleanup["reason"], "guard_heartbeat_stale")
            manager.wait(timeout=5)
            runtime.wait(timeout=5)
            self.assertFalse((root / "runner").exists())
            self.assertIsNone(unrelated.poll())
        finally:
            for process in (manager, runtime, unrelated):
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main()
