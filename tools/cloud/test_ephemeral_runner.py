import io
import json
import tarfile
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

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and symlink semantics")
    def test_artifact_claim_creation_and_owned_cleanup(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"):
            path = runner.prepare_artifact_directory(task, self.root, state)
            self.assertEqual(path, self.root / (runner.ARTIFACT_PREFIX + task))
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)
            self.assertEqual((path / ".mrfps-owner.json").stat().st_mode & 0o777, 0o600)
            (path / "audited-server.bin").write_bytes(b"fixture")
            untouched = self.root / "unrelated"
            untouched.mkdir(); (untouched / "keep").write_text("safe")
            self.assertTrue(runner.remove_owned_artifact_directory(task, state))
            self.assertFalse(path.exists())
            self.assertEqual((untouched / "keep").read_text(), "safe")
            self.assertTrue(runner.remove_owned_artifact_directory(task, state))

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and symlink semantics")
    def test_artifact_collision_is_not_adopted_or_removed(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"):
            path = runner.artifact_path(task); path.mkdir(mode=0o700)
            (path / "other").write_text("preserve")
            with self.assertRaisesRegex(ValueError, "COLLISION"):
                runner.prepare_artifact_directory(task, self.root, state)
            self.assertTrue(runner.remove_owned_artifact_directory(task, state))
            self.assertEqual((path / "other").read_text(), "preserve")

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and symlink semantics")
    def test_artifact_cleanup_refuses_marker_tampering_and_inode_replacement(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"):
            path = runner.prepare_artifact_directory(task, self.root, state)
            marker = path / ".mrfps-owner.json"; original = marker.read_text()
            marker.write_text('{}')
            with self.assertRaisesRegex(ValueError, "MARKER_MISMATCH"):
                runner.remove_owned_artifact_directory(task, state)
            self.assertTrue(path.exists())
            marker.write_text(original)
            state["artifact_claim"]["inode"] += 1
            with self.assertRaisesRegex(ValueError, "IDENTITY_INVALID"):
                runner.remove_owned_artifact_directory(task, state)
            self.assertTrue(path.exists())

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and symlink semantics")
    def test_artifact_interrupted_claim_requires_ownership_marker(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"):
            path = runner.prepare_artifact_directory(task, self.root, state)
            claim = state["artifact_claim"]; claim.pop("device"); claim.pop("inode")
            self.assertTrue(runner.remove_owned_artifact_directory(task, state))
            path = runner.prepare_artifact_directory(task, self.root, state)
            (path / ".mrfps-owner.json").unlink()
            with self.assertRaisesRegex(ValueError, "MARKER_REQUIRED"):
                runner.remove_owned_artifact_directory(task, state)
            self.assertTrue(path.exists())

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and symlink semantics")
    def test_artifact_symlink_swap_is_never_followed(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"):
            path = runner.prepare_artifact_directory(task, self.root, state)
            self.assertTrue(runner.remove_owned_artifact_directory(task, state))
            target = self.root / "unrelated"; target.mkdir(); (target / "keep").write_text("safe")
            path.symlink_to(target, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "SYMLINK"):
                runner.remove_owned_artifact_directory(task, state)
            self.assertEqual((target / "keep").read_text(), "safe")

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and restart recovery")
    def test_recovery_removes_only_explicit_matching_orphan_and_is_idempotent(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        report = self.root / "report.json"
        report.write_text(json.dumps({"taskId": task, "runnerLabel": "mrfps-private-" + task,
                                      "repository": runner.REPOSITORY, "operation": "Publish"}))
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"), \
             patch.object(runner, "task_root", return_value=self.root / "lost-tmpfs-state"), \
             patch.object(runner, "verify_no_recovery_unit"), \
             patch.object(runner, "verify_no_recovery_unit_or_pid") as unit, \
             patch.object(runner, "verify_no_recovery_builder") as builder:
            path = runner.prepare_artifact_directory(task, self.root, state)
            (path / "audited-fixture").write_bytes(b"fixture")
            self.assertEqual(runner.recover_artifact(task, report)["status"], "removed")
            self.assertFalse(path.exists())
            self.assertEqual(runner.recover_artifact(task, report)["status"], "already_removed")
            unit.assert_called_once(); builder.assert_called_once()

    @unittest.skipUnless(os.name == "posix", "Linux owner/mode and restart recovery")
    def test_recovery_refuses_ambiguous_identity_process_or_builder(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        report = self.root / "report.json"
        record = {"taskId": task, "runnerLabel": "mrfps-private-" + task,
                  "repository": runner.REPOSITORY, "operation": "Publish"}
        report.write_text(json.dumps(record))
        with patch.object(runner, "ARTIFACT_BASE", self.root), patch.object(runner, "validate_artifact_storage"):
            path = runner.prepare_artifact_directory(task, self.root, state)
            (path / "keep").write_text("safe")
            wrong = dict(record, taskId="a" * 16); report.write_text(json.dumps(wrong))
            with self.assertRaisesRegex(ValueError, "REPORT_IDENTITY_MISMATCH"):
                runner.recover_artifact(task, report)
            report.write_text(json.dumps(record))
            with patch.object(runner, "task_root", return_value=self.root):
                with self.assertRaisesRegex(ValueError, "TMP_STATE_STILL_PRESENT"):
                    runner.recover_artifact(task, report)
            with patch.object(runner, "task_root", return_value=self.root / "absent"), \
                 patch.object(runner, "verify_no_recovery_unit_or_pid", side_effect=ValueError("RECOVERY_MANAGER_PID_PRESENT")):
                with self.assertRaisesRegex(ValueError, "PID_PRESENT"):
                    runner.recover_artifact(task, report)
            with patch.object(runner, "task_root", return_value=self.root / "absent"), \
                 patch.object(runner, "verify_no_recovery_unit_or_pid"), \
                 patch.object(runner, "verify_no_recovery_builder", side_effect=ValueError("RECOVERY_BUILDER_STILL_PRESENT")):
                with self.assertRaisesRegex(ValueError, "BUILDER_STILL_PRESENT"):
                    runner.recover_artifact(task, report)
            self.assertEqual((path / "keep").read_text(), "safe")

    @unittest.skipUnless(os.name == "posix", "Linux process and systemd checks")
    def test_recovery_rejects_same_name_unit_and_even_reused_manager_pid(self):
        task = uuid.uuid4().hex[:16]
        absent = SimpleNamespace(returncode=0, stdout="LoadState=not-found\nActiveState=inactive\nInvocationID=\nControlGroup=\n")
        active = SimpleNamespace(returncode=0, stdout="LoadState=loaded\nActiveState=active\nInvocationID=abc\nControlGroup=/task\n")
        with patch.object(runner.subprocess, "run", return_value=active):
            with self.assertRaisesRegex(ValueError, "UNIT_STILL_PRESENT"):
                runner.verify_no_recovery_unit_or_pid(task, {"manager_pid": 2000000000})
        with patch.object(runner.subprocess, "run", return_value=absent):
            with self.assertRaisesRegex(ValueError, "PID_PRESENT"):
                runner.verify_no_recovery_unit_or_pid(task, {"manager_pid": os.getpid()})
            runner.verify_no_recovery_unit_or_pid(task, {"manager_pid": 2000000000})

    @unittest.skipUnless(os.name == "posix" and shutil.which("docker") and shutil.which("systemctl"),
                         "live local Docker and systemd metadata required")
    def test_actual_recovery_of_tiny_owned_disk_fixture_keeps_unrelated_process(self):
        task = uuid.uuid4().hex[:16]
        state = {"task_id": task, "manager": {"pid": 2000000000, "pgid": 2000000000, "start_ticks": 1},
                 "manager_invocation_id": "a" * 32}
        report = self.root / "recovery-report.json"
        report.write_text(json.dumps({"taskId": task, "runnerLabel": "mrfps-private-" + task,
                                      "repository": runner.REPOSITORY, "operation": "Publish"}))
        unrelated = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(30)"])
        try:
            self.assertFalse(runner.task_root(task).exists())
            artifact = runner.prepare_artifact_directory(task, self.root, state)
            (artifact / "audited-fixture").write_bytes(b"fixture")
            self.assertEqual(runner.recover_artifact(task, report)["status"], "removed")
            self.assertFalse(artifact.exists())
            self.assertIsNone(unrelated.poll())
            self.assertEqual(runner.recover_artifact(task, report)["status"], "already_removed")
        finally:
            if state.get("artifact_claim"):
                runner.remove_owned_artifact_directory(task, state)
            if unrelated.poll() is None:
                unrelated.terminate()
            unrelated.wait(timeout=5)

    def test_recovery_builder_absence_must_be_unambiguous(self):
        task = "a" * 16; name = "buildx_buildkit_mrfps-private-builder-" + task + "0"
        good_info = SimpleNamespace(returncode=0, stdout="engine-id\n", stderr="")
        absent = SimpleNamespace(returncode=1, stdout="[]\n", stderr="Error response from daemon: No such container: " + name + "\n")
        present = SimpleNamespace(returncode=0, stdout="[{}]", stderr="")
        failed = SimpleNamespace(returncode=1, stdout="", stderr="Cannot connect to Docker daemon")
        with patch.object(runner.subprocess, "run", side_effect=[good_info, absent]):
            runner.verify_no_recovery_builder(task)
        with patch.object(runner.subprocess, "run", side_effect=[good_info, present]):
            with self.assertRaisesRegex(ValueError, "BUILDER_STILL_PRESENT"):
                runner.verify_no_recovery_builder(task)
        with patch.object(runner.subprocess, "run", side_effect=[good_info, failed]):
            with self.assertRaisesRegex(ValueError, "ABSENCE_UNVERIFIED"):
                runner.verify_no_recovery_builder(task)

    def test_artifact_namespace_rejects_invalid_task(self):
        for task in ("../other", "", "a" * 15, "A" * 16):
            with self.subTest(task=task), self.assertRaises(ValueError):
                runner.artifact_path(task)

    @unittest.skipUnless(os.name == "posix", "Linux mount semantics")
    def test_artifact_storage_rejects_tmpfs_and_insufficient_space(self):
        tmpfs = SimpleNamespace(returncode=0, stdout="tmpfs\n")
        ext4 = SimpleNamespace(returncode=0, stdout="ext4\n")
        with patch.object(runner, "ARTIFACT_BASE", self.root), \
             patch.object(runner.subprocess, "run", side_effect=[ext4]):
            with self.assertRaisesRegex(ValueError, "RUNNER_TMPFS_REQUIRED"):
                runner.validate_artifact_storage()
        with patch.object(runner, "ARTIFACT_BASE", self.root), \
             patch.object(runner.subprocess, "run", side_effect=[tmpfs, tmpfs]):
            with self.assertRaisesRegex(ValueError, "EXT4_REQUIRED"):
                runner.validate_artifact_storage()
        with patch.object(runner, "ARTIFACT_BASE", self.root), \
             patch.object(runner.subprocess, "run", side_effect=[tmpfs, ext4]), \
             patch.object(runner.shutil, "disk_usage", return_value=SimpleNamespace(free=1024)):
            with self.assertRaisesRegex(ValueError, "SPACE_INSUFFICIENT"):
                runner.validate_artifact_storage()

    def test_runner_extraction_omits_only_alpine_runtime_copies(self):
        for name in ("externals/node20_alpine", "./externals/node20_alpine/bin/node",
                     "externals/node24_alpine/bin/node"):
            with self.subTest(name=name): self.assertFalse(runner.runner_member_needed(name))
        for name in ("externals/node20/bin/node", "externals/node24/bin/node",
                     "bin/Runner.Listener", "../externals/node20_alpine/bin/node",
                     "externals/node20_alpine_other/bin/node"):
            with self.subTest(name=name): self.assertTrue(runner.runner_member_needed(name))

    @unittest.skipUnless(os.name == "posix" and os.environ.get("MRFPS_TEST_REAL_ARCHIVE"),
                         "explicit approved runner archive required")
    def test_real_pinned_runner_layout_and_small_extraction_fixture(self):
        archive_path = Path(os.environ["MRFPS_TEST_REAL_ARCHIVE"])
        runner.checked_archive(archive_path, runner.RUNNER_HASH)
        required = {"config.sh", "run.sh", "bin/Runner.Listener",
                    "externals/node20/bin/node", "externals/node24/bin/node",
                    "externals/node20_alpine", "externals/node24_alpine"}
        small = {"config.sh", "run.sh", "bin/Runner.Listener"}
        destination = self.root / "real-runner-small"; destination.mkdir()
        with tarfile.open(archive_path) as archive:
            members = archive.getmembers()
            normalized = {runner.PurePosixPath(item.name).as_posix(): item for item in members}
            self.assertTrue(required.issubset(normalized))
            for name in required - {"externals/node20_alpine", "externals/node24_alpine"}:
                self.assertTrue(runner.runner_member_needed(name)); self.assertGreater(normalized[name].size, 0)
            for name in ("externals/node20_alpine", "externals/node24_alpine"):
                self.assertFalse(runner.runner_member_needed(name))
            for item in members:
                if runner.runner_member_needed(item.name) and (item.issym() or item.islnk()):
                    self.assertNotIn("alpine", item.linkname.lower())
            archive.extractall(destination, members=[normalized[name] for name in sorted(small)], filter="data")
        for name in small:
            self.assertTrue((destination / name).is_file())
        self.assertFalse((destination / "externals").exists())

    @unittest.skipUnless(os.name == "posix", "Linux glibc runner extraction")
    def test_runner_extraction_keeps_host_nodes_and_rejects_traversal(self):
        def make_archive(path, names):
            with tarfile.open(path, "w:gz") as archive:
                for name in names:
                    item = tarfile.TarInfo(name); item.size = 1
                    archive.addfile(item, io.BytesIO(b"x"))
        names = ["externals/node20_alpine/bin/node", "externals/node24_alpine/bin/node",
                 "externals/node20/bin/node", "externals/node24/bin/node", "bin/Runner.Listener"]
        archive = self.root / "runner.tar.gz"; destination = self.root / "extract"
        destination.mkdir(); make_archive(archive, names)
        with patch.object(runner.os, "confstr", return_value="glibc 2.43"):
            runner.extract_host_runner(archive, destination)
        self.assertTrue((destination / "externals/node20/bin/node").is_file())
        self.assertTrue((destination / "externals/node24/bin/node").is_file())
        self.assertFalse((destination / "externals/node20_alpine").exists())
        self.assertFalse((destination / "externals/node24_alpine").exists())
        bad = self.root / "bad.tar.gz"; make_archive(bad, names + ["../escape"])
        other = self.root / "bad-extract"; other.mkdir()
        with patch.object(runner.os, "confstr", return_value="glibc 2.43"):
            with self.assertRaises(tarfile.FilterError):
                runner.extract_host_runner(bad, other)
        self.assertFalse((self.root / "escape").exists())

    @unittest.skipUnless(os.name == "posix", "Linux glibc runner extraction")
    def test_runner_extraction_rejects_non_glibc_or_unexpected_archive(self):
        archive = self.root / "empty.tar.gz"
        with tarfile.open(archive, "w:gz"):
            pass
        destination = self.root / "extract"; destination.mkdir()
        with patch.object(runner.os, "confstr", return_value="musl"):
            with self.assertRaisesRegex(ValueError, "GLIBC_HOST_REQUIRED"):
                runner.extract_host_runner(archive, destination)
        with patch.object(runner.os, "confstr", return_value="glibc 2.43"):
            with self.assertRaisesRegex(ValueError, "ALPINE_LAYOUT_CHANGED"):
                runner.extract_host_runner(archive, destination)

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
            state = {"task_id": task, "repository": runner.REPOSITORY, "created_at": time.time(),
                "manager": runner.process_identity(manager.pid), "runtime": runner.process_identity(runtime.pid),
                "manager_invocation_id": "a" * 32, "builder_claimed": False}
            runner.write_json(root / "state.json", state)
            artifact = runner.prepare_artifact_directory(task, root, state)
            (artifact / "audited-fixture").write_bytes(b"fixture")
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
            self.assertTrue(cleanup["artifact_directory_removed"])
            self.assertFalse(artifact.exists())
            self.assertIsNone(unrelated.poll())
        finally:
            if "state" in locals() and state.get("artifact_claim"):
                runner.remove_owned_artifact_directory(task, state)
            for process in (manager, runtime, unrelated):
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main()
