import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("device_login", Path(__file__).with_name("device_login.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

EMPTY_ACTIVITY_DUMP = """ACTIVITY MANAGER ACTIVITIES (dumpsys activity activities)
  * Task{abcd #100 type=home sz=1}
    * Hist  #0: ActivityRecord{beef u0 com.example/.Home t100}
"""

ORPHAN_ACTIVITY_DUMP = """ACTIVITY MANAGER ACTIVITIES (dumpsys activity activities)
  * Task{abcd #484 type=standard sz=1}
    * Task{abcd #485 type=standard rootTaskId=484 sz=1}
      * Hist  #0: ActivityRecord{dead u0 com.meta.systemui/.media.MediaProjectionPermissionActivity t485}
        launchedFromUid=10172 launchedFromPackage=org.mrfps.arena launchedFromFeature=null userId=0
        resultTo=ActivityRecord{dead u0 org.mrfps.arena/com.epicgames.unreal.GameActivity t-1 f}} resultCode=58481
"""


class DeviceLoginControlTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_show_settings_then_configure_once(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = argparse.Namespace(server=module.DEFAULT_SERVER, role="inspector", name="MRFPS-fixture",
                executable=Path(sys.executable), editor=True, code_file=None, show_settings=True,
                replace=False, auth_test_url="")
            code = root / "code.txt"
            code.write_text("000123", encoding="ascii")
            with mock.patch.object(module, "ROOT", root), mock.patch.dict(module.os.environ, {"LOCALAPPDATA": str(root)}), \
                 mock.patch.object(module, "windows_control") as control, \
                 mock.patch.object(module.subprocess, "Popen") as launch:
                # Use this test process solely as a verifiable live guard identity;
                # no Unreal process or external window is created.
                launch.return_value.pid = os.getpid()
                self.assertEqual(module.windows(args)["status"], "launching")
                state_path = root / "Saved" / "DeviceAuth" / "windows-MRFPS-fixture.json"
                self.assertFalse(module.load_state(state_path)["configured"])
                self.assertEqual(module.windows(args)["status"], "already_running")
                control.assert_not_called()
                args.show_settings = False
                self.assertEqual(module.windows(args)["status"], "settings_queued")
                control.assert_called_once_with(args.name, args.server, args.role, None)
                self.assertTrue(module.load_state(state_path)["configured"])
                self.assertEqual(module.windows(args)["status"], "already_running")
                args.code_file = code
                self.assertEqual(module.windows(args)["status"], "login_queued")
                self.assertEqual(module.windows(args)["status"], "already_running")
                self.assertEqual(control.call_count, 2)
                control.assert_called_with(args.name, args.server, args.role, "000123")
                args.show_settings = True
                args.code_file = None
                with self.assertRaisesRegex(RuntimeError, "settings page requires a new instance"):
                    module.windows(args)
                self.assertEqual(launch.call_count, 1)

    def test_endpoint_and_code_validation(self):
        self.assertEqual(module.endpoint("mrfps.mizuame.app:7777"), "mrfps.mizuame.app:7777")
        self.assertEqual(module.endpoint("[2001:db8::1]:7777"), "[2001:db8::1]:7777")
        for invalid in ("https://example.com:7777", "host:0", "host:65536", "host:12;rm", "host:"):
            with self.assertRaises(ValueError):
                module.endpoint(invalid)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "code.txt"
            path.write_bytes(b"000123\n")
            self.assertEqual(module.read_code(path), b"000123")
            for invalid in (b"12345", b"1234567", b"123x56", b"\xef\xbb\xbf123456"):
                path.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    module.read_code(path)

    def test_debug_auth_url_cannot_escape_loopback(self):
        self.assertTrue(module.test_auth_url("http://127.0.0.1:18787/api/device/exchange"))
        self.assertTrue(module.test_auth_url("http://localhost:80"))
        for invalid in ("http://127.0.0.1:1234@evil.test/api", "http://localhost.evil.test:80/", "http://localhost:0/", "http://localhost:65536/", "http://localhost:99/?x=1"):
            self.assertFalse(module.test_auth_url(invalid))

    def test_quest_repeated_configuration_does_not_restart_or_redeem_again(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code = root / "code.txt"
            code.write_text("000123", encoding="ascii")
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player", adb="adb", replace=False, code_file=code)
            calls = []
            def fake_adb(_adb, _serial, cmd, **kwargs):
                calls.append((cmd, kwargs))
                if cmd == ["shell", "run-as", module.PACKAGE, "pwd"]:
                    return "/data/user/0/" + module.PACKAGE
                if cmd == ["shell", "dumpsys", "activity", "activities"]:
                    return EMPTY_ACTIVITY_DUMP
                return ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", fake_adb), mock.patch.object(module, "quest_pid", side_effect=["", "", "", "", "4321", "4321"]):
                self.assertEqual(module.quest(args)["status"], "launched")
                count = len(calls)
                self.assertEqual(module.quest(args)["status"], "already_running")
                self.assertEqual(len(calls), count)
            transfers = [kwargs.get("data") for _cmd, kwargs in calls if kwargs.get("data") is not None]
            self.assertEqual(len(transfers), 1)
            self.assertEqual(__import__("json").loads(transfers[0])["code"], "000123")
            self.assertFalse(any("000123" in str(cmd) for cmd, _kwargs in calls))

    def test_running_quest_accepts_new_code_once_without_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code = root / "code.txt"
            code.write_text("000123", encoding="ascii")
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player", adb="adb", replace=False, code_file=None)
            calls = []
            def fake_adb(_adb, _serial, cmd, **kwargs):
                calls.append((cmd, kwargs))
                if cmd == ["shell", "dumpsys", "activity", "activities"]:
                    return EMPTY_ACTIVITY_DUMP
                return "/data/user/0/" + module.PACKAGE if cmd == ["shell", "run-as", module.PACKAGE, "pwd"] else ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", fake_adb), mock.patch.object(module, "quest_pid", side_effect=["", "", "", "", "4321", "4321", "4321"]):
                self.assertEqual(module.quest(args)["status"], "launched")
                args.code_file = code
                self.assertEqual(module.quest(args)["status"], "login_queued")
                count = len(calls)
                self.assertEqual(module.quest(args)["status"], "already_running")
                self.assertEqual(len(calls), count)
            self.assertEqual(sum(1 for _cmd, kwargs in calls if kwargs.get("data") is not None), 2)
            self.assertFalse(any("000123" in str(cmd) for cmd, _kwargs in calls))

    def test_quest_pid_distinguishes_absence_from_adb_failure(self):
        def result(code, out=b"", err=b""):
            return __import__("subprocess").CompletedProcess([], code, out, err)
        for response, expected in ((result(1), ""), (result(0, b"12 34\n"), "12 34")):
            with self.subTest(response=response), mock.patch.object(module, "adb_result", return_value=response):
                self.assertEqual(module.quest_pid("adb", "fixture"), expected)
        for response in (result(1, err=b"device offline"), result(2), result(0),
                         result(0, b"12 invalid"), result(1, b"12"), result(0, b"12", b"error")):
            with self.subTest(response=response), mock.patch.object(module, "adb_result", return_value=response):
                with self.assertRaisesRegex(RuntimeError, "Cannot reliably inspect"):
                    module.quest_pid("adb", "fixture")

    def test_orphan_task_requires_the_exact_single_owned_finishing_activity(self):
        for dump in (ORPHAN_ACTIVITY_DUMP, ORPHAN_ACTIVITY_DUMP.replace("\n", "\r\n")):
            self.assertEqual(module.quest_share_tasks(dump), ([484], {484}))
        other = ORPHAN_ACTIVITY_DUMP.replace("org.mrfps.arena", "other.package")
        self.assertEqual(module.quest_share_tasks(other), ([], {484}))
        ambiguous = (
            ORPHAN_ACTIVITY_DUMP.replace("t-1 f}}", "t101}}"),
            ORPHAN_ACTIVITY_DUMP.replace("sz=1}", "sz=2}"),
            ORPHAN_ACTIVITY_DUMP.replace("launchedFromPackage=org.mrfps.arena", "launchedFromPackage="),
            ORPHAN_ACTIVITY_DUMP.replace("MediaProjectionPermissionActivity ", "MediaProjectionPermissionActivityAlias "),
            ORPHAN_ACTIVITY_DUMP.replace("MediaProjectionPermissionActivity t485}", "MediaProjectionPermissionActivity t999}"),
            ORPHAN_ACTIVITY_DUMP.replace("      * Hist", "    * Hist"),
            ORPHAN_ACTIVITY_DUMP + "      * Hist  #1: ActivityRecord{beef u0 com.other/.Activity t485}\n",
            ORPHAN_ACTIVITY_DUMP + "    * Task{eeee #486 type=standard rootTaskId=484 sz=1}\n",
            ORPHAN_ACTIVITY_DUMP + "      * Hist  #1: ActivityRecord{beef u0 com.other/.Activity t485}",
            ORPHAN_ACTIVITY_DUMP.replace("        resultTo=", "        launchedFromUid=2 launchedFromPackage=other.package\n        resultTo="),
            "", "error: device offline", "unparsed " + module._QUEST_SHARE_NAME,
        )
        for dump in ambiguous:
            with self.subTest(dump=dump), self.assertRaises(RuntimeError):
                module.quest_share_tasks(dump)

    def test_quest_orphan_cleanup_requires_two_consistent_checks_and_rechecks_removal(self):
        calls = []
        dumps = iter((ORPHAN_ACTIVITY_DUMP, ORPHAN_ACTIVITY_DUMP, EMPTY_ACTIVITY_DUMP))
        def fake_adb(_adb, _serial, cmd, **_kwargs):
            calls.append(cmd)
            if cmd == ["shell", "dumpsys", "activity", "activities"]:
                return next(dumps)
            return ""
        with mock.patch.object(module, "run_adb", fake_adb), mock.patch.object(module, "quest_pid", return_value=""):
            module.clear_orphaned_quest_share("adb", "fixture")
        self.assertEqual(calls, [["shell", "dumpsys", "activity", "activities"],
                                 ["shell", "dumpsys", "activity", "activities"],
                                 ["shell", "am", "stack", "remove", "484"],
                                 ["shell", "dumpsys", "activity", "activities"]])
        with mock.patch.object(module, "run_adb", return_value=EMPTY_ACTIVITY_DUMP) as adb, \
             mock.patch.object(module, "quest_pid", return_value=""):
            module.clear_orphaned_quest_share("adb", "fixture")
            adb.assert_called_once_with("adb", "fixture", ["shell", "dumpsys", "activity", "activities"])

    def test_quest_orphan_cleanup_never_removes_ambiguous_or_changed_tasks(self):
        multiple = ORPHAN_ACTIVITY_DUMP + ORPHAN_ACTIVITY_DUMP.replace("#484", "#488")
        changed = ORPHAN_ACTIVITY_DUMP.replace("#484", "#488")
        scenarios = (
            ([ORPHAN_ACTIVITY_DUMP], ["7"]),
            ([multiple], [""]),
            ([ORPHAN_ACTIVITY_DUMP, changed], ["", ""]),
            ([ORPHAN_ACTIVITY_DUMP, ORPHAN_ACTIVITY_DUMP], ["", "", "7"]),
            ([ORPHAN_ACTIVITY_DUMP.replace("sz=1}", "sz=2}")], [""]),
            (["unrecognized dump"], [""]),
        )
        for dump_sequence, pid_sequence in scenarios:
            calls = []
            dumps = iter(dump_sequence)
            def fake_adb(_adb, _serial, cmd, **_kwargs):
                calls.append(cmd)
                return next(dumps)
            with self.subTest(dump_sequence=dump_sequence, pid_sequence=pid_sequence), \
                 mock.patch.object(module, "run_adb", fake_adb), \
                 mock.patch.object(module, "quest_pid", side_effect=pid_sequence):
                with self.assertRaises(RuntimeError):
                    module.clear_orphaned_quest_share("adb", "fixture")
            self.assertFalse(any(cmd[:4] == ["shell", "am", "stack", "remove"] for cmd in calls))

    def test_quest_cleanup_refuses_to_launch_if_removal_is_unverified(self):
        with tempfile.TemporaryDirectory() as folder:
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                      adb="adb", replace=False, code_file=None)
            calls = []
            def fake_adb(_adb, _serial, cmd, **_kwargs):
                calls.append(cmd)
                return ORPHAN_ACTIVITY_DUMP if cmd == ["shell", "dumpsys", "activity", "activities"] else ""
            with mock.patch.object(module, "ROOT", Path(folder)), \
                 mock.patch.object(module, "run_adb", fake_adb), \
                 mock.patch.object(module, "quest_pid", return_value=""):
                with self.assertRaisesRegex(RuntimeError, "could not be cleared"):
                    module.quest(args)
            self.assertEqual(sum(cmd[:4] == ["shell", "am", "stack", "remove"] for cmd in calls), 1)
            self.assertFalse(any(cmd[:3] == ["shell", "am", "start"] for cmd in calls))

    def test_quest_replace_only_cleans_a_proven_orphan_after_the_app_stops(self):
        with tempfile.TemporaryDirectory() as folder:
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                      adb="adb", replace=True, code_file=None)
            calls = []
            dumps = iter((ORPHAN_ACTIVITY_DUMP, ORPHAN_ACTIVITY_DUMP, EMPTY_ACTIVITY_DUMP))
            def fake_adb(_adb, _serial, cmd, **_kwargs):
                calls.append(cmd)
                if cmd == ["shell", "dumpsys", "activity", "activities"]:
                    return next(dumps)
                if cmd == ["shell", "run-as", module.PACKAGE, "pwd"]:
                    return "/data/user/0/" + module.PACKAGE
                return ""
            # Running, still stopping, stopped, three deletion checks, then a new PID.
            pids = ["111", "111", "", "", "", "", "", "", "", "222"]
            with mock.patch.object(module, "ROOT", Path(folder)), \
                 mock.patch.object(module, "run_adb", fake_adb), \
                 mock.patch.object(module, "quest_pid", side_effect=pids), \
                 mock.patch.object(module.time, "sleep"):
                self.assertEqual(module.quest(args)["status"], "launched")
            force = ["shell", "am", "force-stop", module.PACKAGE]
            remove = ["shell", "am", "stack", "remove", "484"]
            self.assertIn(force, calls)
            self.assertIn(remove, calls)
            self.assertLess(calls.index(force), calls.index(remove))
            self.assertLess(calls.index(remove), next(i for i, cmd in enumerate(calls) if cmd[:3] == ["shell", "am", "start"]))

    def test_key_reset_is_idempotent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            key = root / "MRFPS" / "Auth" / "issuer-public.pem"
            key.parent.mkdir(parents=True)
            key.write_text("public", encoding="ascii")
            args = argparse.Namespace(platform="windows")
            with mock.patch.dict(module.os.environ, {"LOCALAPPDATA": str(root)}):
                self.assertEqual(module.reset(args)["status"], "trust_cleared")
                self.assertEqual(module.reset(args)["status"], "trust_cleared")
            self.assertFalse(key.exists())

    def test_concurrent_command_does_not_run_twice(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "state.json"
            with module.state_lock(path):
                with self.assertRaises(RuntimeError):
                    with module.state_lock(path):
                        pass
            with module.state_lock(path):
                pass

    def test_status_exports_only_fixed_safe_event_fields(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            state = root / "Saved" / "DeviceAuth" / "windows-fixture.json"
            report = root / "evidence" / "report.json"
            state.parent.mkdir(parents=True)
            report.parent.mkdir(parents=True)
            state.write_text(json.dumps({"pid": 12, "report": str(report)}), encoding="utf-8")
            report.write_text('{"status":"running"}', encoding="utf-8")
            (report.parent / "engine.log").write_text("MRFPS_DEVICE_EXCHANGE_START role=player secret-123456\nHTTP secret-123456\n", encoding="utf-8")
            with mock.patch.object(module, "ROOT", root):
                result = module.status(argparse.Namespace(name="fixture"))
            self.assertEqual(result["events"], ["MRFPS_DEVICE_EXCHANGE_START role=player"])
            self.assertNotIn("123456", json.dumps(result))


if __name__ == "__main__":
    unittest.main()
