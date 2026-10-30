import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
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

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_never_reuses_code_after_restart_or_another_submission(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            first,second=root/"first.txt",root/"second.txt"
            first.write_text("000123",encoding="ascii"); second.write_text("000456",encoding="ascii")
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=first,show_settings=False,
                replace=False,auth_test_url="")
            state_path=root/"Saved"/"DeviceAuth"/"windows-MRFPS-fixture.json"
            with mock.patch.object(module,"ROOT",root), mock.patch.dict(module.os.environ,{"LOCALAPPDATA":str(root)}), \
                 mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen") as launch:
                launch.return_value.pid=os.getpid()  # Verify an existing process without spawning UE.
                self.assertEqual(module.windows(args)["status"],"launching")
                self.assertEqual(module.windows(args)["status"],"already_running")
                # A changed file with a restored mtime is still the previously
                # submitted marker; it must never trigger another publication.
                timestamp=first.stat()
                first.write_text("000789",encoding="ascii")
                os.utime(first,ns=(timestamp.st_atime_ns,timestamp.st_mtime_ns))
                self.assertEqual(module.windows(args)["status"],"already_running")
                self.assertEqual(control.call_count,1)
                args.replace=True
                with self.assertRaisesRegex(RuntimeError,"already submitted"):
                    module.windows(args)  # Must refuse before opening a handle to terminate.
                args.replace=False
                stopped=module.load_submission_state(state_path,"Windows")
                stopped.update({"pid":None,"created":None})  # A previously stopped guard.
                module.save_state(state_path,stopped)
                with self.assertRaisesRegex(RuntimeError,"already submitted"):
                    module.windows(args)
                self.assertEqual(control.call_count,1)
                args.code_file=None
                self.assertEqual(module.windows(args)["status"],"launching")
                args.code_file=second
                self.assertEqual(module.windows(args)["status"],"login_queued")
                args.code_file=first
                with self.assertRaisesRegex(RuntimeError,"already submitted"):
                    module.windows(args)
                self.assertEqual(control.call_count,3)
            saved=module.load_submission_state(state_path,"Windows")
            self.assertEqual(len(saved["submission_history"]),2)
            self.assertNotIn("000123",json.dumps(saved))
            self.assertNotIn("000456",json.dumps(saved))
            self.assertNotIn("000789",json.dumps(saved))

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_interrupted_code_transfer_never_retries_that_marker(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_text("000123",encoding="ascii")
            replacement=root/"new.txt"; replacement.write_text("000456",encoding="ascii")
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=None,show_settings=True,
                replace=False,auth_test_url="")
            state_path=root/"Saved"/"DeviceAuth"/"windows-MRFPS-fixture.json"
            with mock.patch.object(module,"ROOT",root), mock.patch.dict(module.os.environ,{"LOCALAPPDATA":str(root)}), \
                 mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen") as launch:
                launch.return_value.pid=os.getpid()
                self.assertEqual(module.windows(args)["status"],"launching")
                args.show_settings=False; args.code_file=code
                control.side_effect=TimeoutError("control outcome lost")
                with self.assertRaises(TimeoutError): module.windows(args)
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): module.windows(args)
                self.assertEqual(control.call_count,1)
                saved=module.load_submission_state(state_path,"Windows")
                self.assertEqual(saved["submission_inflight"],saved["submission_history"][0])
                args.code_file=replacement; control.side_effect=None
                self.assertEqual(module.windows(args)["status"],"login_queued")
                args.code_file=code
                with self.assertRaisesRegex(RuntimeError,"already submitted"): module.windows(args)
                self.assertEqual(control.call_count,2)

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_interrupted_launch_never_starts_a_second_untracked_guard(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_text("000123",encoding="ascii")
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=code,show_settings=False,
                replace=False,auth_test_url="")
            with mock.patch.object(module,"ROOT",root), mock.patch.dict(module.os.environ,{"LOCALAPPDATA":str(root)}), \
                 mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen",side_effect=OSError("lost launch")) as launch:
                with self.assertRaises(OSError): module.windows(args)
                with self.assertRaisesRegex(RuntimeError,"launch outcome is unknown"): module.windows(args)
                args.code_file=None
                with self.assertRaisesRegex(RuntimeError,"launch outcome is unknown"): module.windows(args)
                self.assertEqual(control.call_count,1); self.assertEqual(launch.call_count,1)
                self.assertEqual(module.status(argparse.Namespace(name=args.name))["status"],"launch_unknown")
                with self.assertRaisesRegex(RuntimeError,"guard identity is unavailable"):
                    module.stop_windows(argparse.Namespace(name=args.name))

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_corrupt_submission_journal_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_text("000123",encoding="ascii")
            state=root/"Saved"/"DeviceAuth"/"windows-MRFPS-fixture.json"
            state.parent.mkdir(parents=True); state.write_text('{"submission_history":[null]}',encoding="utf-8")
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=code,show_settings=False,
                replace=False,auth_test_url="")
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen") as launch:
                with self.assertRaisesRegex(RuntimeError,"journal"): module.windows(args)
                control.assert_not_called(); launch.assert_not_called()
            state.write_text('{',encoding="utf-8")
            with mock.patch.object(module,"ROOT",root):
                with self.assertRaisesRegex(RuntimeError,"journal"): module.status(argparse.Namespace(name=args.name))
                with self.assertRaisesRegex(RuntimeError,"journal"): module.stop_windows(argparse.Namespace(name=args.name))

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_rejects_changed_expected_code_before_control_or_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_text("000123",encoding="ascii")
            marker,value=module.submission(code)
            code.write_text("000456",encoding="ascii")
            os.utime(code,ns=(code.stat().st_atime_ns,marker["modified_ns"]+1_000_000_000))
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=code,show_settings=False,
                replace=False,auth_test_url="",expected_marker=marker,expected_code=value,before_submit=None)
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen") as launch:
                with self.assertRaisesRegex(RuntimeError,"changed before submission"): module.windows(args)
                control.assert_not_called(); launch.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_rejects_changed_code_even_if_its_mtime_is_restored(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_text("000123",encoding="ascii")
            marker,value=module.submission(code); stat=code.stat()
            code.write_text("000456",encoding="ascii")
            os.utime(code,ns=(stat.st_atime_ns,marker["modified_ns"]))
            self.assertEqual(module.submission(code)[0],marker)
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=code,show_settings=False,
                replace=False,auth_test_url="",expected_marker=marker,expected_code=value,before_submit=None)
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen") as launch:
                with self.assertRaisesRegex(RuntimeError,"changed before submission"): module.windows(args)
                control.assert_not_called(); launch.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "the client launcher uses Windows process handles")
    def test_windows_validator_runs_before_journal_or_control_publication(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_text("000123",encoding="ascii")
            marker,value=module.submission(code)
            def invalidated(): raise RuntimeError("issuer state changed before submission")
            args=argparse.Namespace(server=module.DEFAULT_SERVER,role="player",name="MRFPS-fixture",
                executable=Path(sys.executable),editor=True,code_file=code,show_settings=False,
                replace=False,auth_test_url="",expected_marker=marker,expected_code=value,before_submit=invalidated)
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"windows_control") as control, \
                 mock.patch.object(module.subprocess,"Popen") as launch:
                with self.assertRaisesRegex(RuntimeError,"issuer state changed"): module.windows(args)
                control.assert_not_called(); launch.assert_not_called()
                self.assertFalse((root/"Saved"/"DeviceAuth"/"windows-MRFPS-fixture.json").exists())

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
            for invalid in (b"12345", b"1234567", b"123x56", b"\xef\xbb\xbf123456", b"1"*1024):
                path.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    module.read_code(path)

    def test_code_file_change_during_read_never_reaches_quest_control(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_bytes(b"000123")
            args=argparse.Namespace(server=module.DEFAULT_SERVER,serial="fixture",role="player",
                                    adb="adb",replace=False,code_file=code)
            real_fstat=os.fstat; calls=0
            def mutate_after_first_stat(fd):
                nonlocal calls
                value=real_fstat(fd); calls+=1
                if calls==1:
                    code.write_bytes(b"000456")
                    os.utime(code,ns=(value.st_atime_ns,value.st_mtime_ns+1_000_000_000))
                return value
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module.os,"fstat",side_effect=mutate_after_first_stat), \
                 mock.patch.object(module,"quest_control") as control:
                with self.assertRaisesRegex(RuntimeError,"changed while reading"):
                    module.quest(args)
                control.assert_not_called()

    def test_code_file_replacement_during_read_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"code.txt"; code.write_bytes(b"000123")
            expected=code.resolve(strict=True); real_fstat=os.fstat; real_stat=Path.stat; calls=0
            def count_fstat(fd):
                nonlocal calls
                value=real_fstat(fd); calls+=1
                return value
            def changed_path_identity(path,*args,**kwargs):
                value=real_stat(path,*args,**kwargs)
                if calls>=2 and str(path)==str(expected):
                    from types import SimpleNamespace
                    return SimpleNamespace(st_dev=value.st_dev,st_ino=value.st_ino+1,
                                           st_mtime_ns=value.st_mtime_ns,st_ctime_ns=value.st_ctime_ns,st_size=value.st_size)
                return value
            with mock.patch.object(module.os,"fstat",side_effect=count_fstat), \
                 mock.patch.object(Path,"stat",changed_path_identity):
                with self.assertRaisesRegex(RuntimeError,"changed while reading"):
                    module.submission(code)

    @unittest.skipUnless(os.name == "nt", "Windows exposes different fstat/stat ctime semantics")
    def test_windows_same_file_accepts_distinct_handle_and_path_ctime(self):
        with tempfile.TemporaryDirectory() as folder:
            code=Path(folder)/"code.txt"; code.write_bytes(b"000123")
            expected=code.resolve(strict=True); real_fstat=os.fstat; real_stat=Path.stat; calls=0
            def count_fstat(fd):
                nonlocal calls
                value=real_fstat(fd); calls+=1; return value
            def distinct_path_ctime(path,*args,**kwargs):
                value=real_stat(path,*args,**kwargs)
                if calls>=2 and str(path)==str(expected):
                    from types import SimpleNamespace
                    fields={name:getattr(value,name) for name in ("st_dev","st_ino","st_mtime_ns","st_size")}
                    fields["st_ctime_ns"]=value.st_ctime_ns+1
                    if hasattr(value,"st_birthtime_ns"): fields["st_birthtime_ns"]=value.st_birthtime_ns
                    return SimpleNamespace(**fields)
                return value
            with mock.patch.object(module.os,"fstat",side_effect=count_fstat), \
                 mock.patch.object(Path,"stat",distinct_path_ctime):
                marker,value=module.submission(code)
            self.assertEqual(marker["path"],str(expected))
            self.assertEqual(value,"000123")

    def test_same_mtime_rewrite_during_handle_read_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            code=Path(folder)/"code.txt"; code.write_bytes(b"000123")
            real_fstat=os.fstat; calls=0
            def mutate(fd):
                nonlocal calls
                value=real_fstat(fd); calls+=1
                if calls==1:
                    time.sleep(.01)
                    code.write_bytes(b"000456")
                    os.utime(code,ns=(value.st_atime_ns,value.st_mtime_ns))
                return value
            with mock.patch.object(module.os,"fstat",side_effect=mutate):
                with self.assertRaisesRegex(RuntimeError,"changed while reading"): module.submission(code)

    def test_same_mtime_rewrite_after_path_check_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            code=Path(folder)/"code.txt"; code.write_bytes(b"000123")
            expected=code.resolve(strict=True); real_fstat=os.fstat; real_stat=Path.stat; calls=0; changed=False
            def count_fstat(fd):
                nonlocal calls
                value=real_fstat(fd); calls+=1; return value
            def mutate_after_stat(path,*args,**kwargs):
                nonlocal changed
                value=real_stat(path,*args,**kwargs)
                if not changed and calls>=2 and str(path)==str(expected):
                    changed=True
                    time.sleep(.01)
                    code.write_bytes(b"000456")
                    os.utime(code,ns=(value.st_atime_ns,value.st_mtime_ns))
                return value
            with mock.patch.object(module.os,"fstat",side_effect=count_fstat), \
                 mock.patch.object(Path,"stat",mutate_after_stat):
                with self.assertRaisesRegex(RuntimeError,"changed while reading"): module.submission(code)
            self.assertTrue(changed)

    def test_debug_auth_url_cannot_escape_loopback(self):
        self.assertTrue(module.test_auth_url("http://127.0.0.1:18787/api/device/exchange"))
        self.assertTrue(module.test_auth_url("http://localhost:80"))
        for invalid in ("http://127.0.0.1:1234@evil.test/api", "http://localhost.evil.test:80/", "http://localhost:0/", "http://localhost:65536/", "http://localhost:99/?x=1"):
            self.assertFalse(module.test_auth_url(invalid))

    def test_quest_glove_pin_requires_a_valid_player_relay(self):
        args = argparse.Namespace(role="player", glove_sender="192.168.1.10", glove_device="CG2-1", glove_port=None)
        self.assertEqual(module.quest_glove_config(args), {"sender":"192.168.1.10", "device":"CG2-1", "port":25788})
        for sender in ("0.0.0.0", "255.255.255.255", "224.0.0.1", "::1", "192.168.1.10;echo"):
            with self.subTest(sender=sender):
                args.glove_sender = sender
                with self.assertRaises(ValueError): module.quest_glove_config(args)
        args.glove_sender = "192.168.1.10"
        for device in ("", "a b", "x;echo", "x" * 129):
            with self.subTest(device=device):
                args.glove_device = device
                with self.assertRaises(ValueError): module.quest_glove_config(args)
        args.glove_device = None
        for port in (0, 1023, 65536, True):
            args.glove_port = port
            with self.assertRaises(ValueError): module.quest_glove_config(args)
        args.glove_port = None
        args.role = "inspector"
        with self.assertRaises(ValueError): module.quest_glove_config(args)
        args.role = "player"
        args.glove_sender = None
        args.glove_device = "CG2"
        with self.assertRaises(ValueError): module.quest_glove_config(args)

    def test_quest_glove_pin_is_applied_once_and_not_changed_by_login(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player", adb="adb",
                                      replace=False, code_file=None, glove_sender="192.168.1.10",
                                      glove_port=25788, glove_device="CG2-1", login_input="glove_controller")
            pid = {"value":""}
            calls = []
            def fake_adb(_adb, _serial, cmd, **_kwargs):
                calls.append(cmd)
                if cmd[:3] == ["shell", "am", "start"]: pid["value"] = "4321"
                return ""
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb",fake_adb), \
                 mock.patch.object(module,"quest_pid",side_effect=lambda *_: pid["value"]), \
                 mock.patch.object(module,"assert_quest_share_clear"), mock.patch.object(module,"quest_control") as control:
                self.assertEqual(module.quest(args)["status"],"launched")
                launch = next(cmd for cmd in calls if cmd[:3] == ["shell", "am", "start"])
                self.assertIn("-MRFPSGloveSender=192.168.1.10 -MRFPSGlovePort=25788 -MRFPSGloveDevice=CG2-1", launch[-1])
                self.assertIn("-MRFPSLoginInput=glove_controller", launch[-1])
                args.login_input = None
                args.glove_sender = args.glove_port = args.glove_device = None
                code = root / "code.txt"; code.write_text("000123", encoding="ascii")
                args.code_file = code
                self.assertEqual(module.quest(args)["status"],"login_queued")
                self.assertEqual(control.call_count,2)
                args.login_input = "quest_axis"
                with self.assertRaisesRegex(RuntimeError,"launch setting"):
                    module.quest(args)
                args.login_input = None
                args.glove_sender = "192.168.1.11"
                before = list(calls)
                with self.assertRaisesRegex(RuntimeError,"launch setting"):
                    module.quest(args)
                self.assertEqual(calls,before)
                self.assertEqual(control.call_count,2)
            saved = module.load_quest_state(root / "Saved" / "DeviceAuth" / "quest-fixture.json")
            self.assertEqual(saved["config"]["glove"]["sender"],"192.168.1.10")
            self.assertEqual(saved["config"]["login_input"],"glove_controller")

    def test_quest_show_settings_launches_once_and_can_configure_later(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player", adb="adb",
                                      replace=False, code_file=None, show_settings=True)
            pid = {"value": ""}
            calls = []
            def fake_adb(_adb, _serial, cmd, **_kwargs):
                calls.append(cmd)
                if cmd[:3] == ["shell", "am", "start"]:
                    pid["value"] = "4321"
                return ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", fake_adb), \
                 mock.patch.object(module, "quest_pid", side_effect=lambda *_: pid["value"]), \
                 mock.patch.object(module, "assert_quest_share_clear"), \
                 mock.patch.object(module, "clear_quest_control") as clear, \
                 mock.patch.object(module, "quest_control") as control:
                first = module.quest(args)
                self.assertEqual(first["status"], "launched")
                self.assertFalse(first["serverArgumentApplied"])
                self.assertFalse(first["pageConfirmed"])
                again = module.quest(args)
                self.assertEqual(again["status"], "already_running")
                self.assertFalse(again["serverArgumentApplied"])
                self.assertFalse(again["pageConfirmed"])
                self.assertEqual(clear.call_count, 1)
                control.assert_not_called()
                args.show_settings = False
                self.assertEqual(module.quest(args)["status"], "settings_queued")
                self.assertEqual(module.quest(args)["status"], "already_running")
                self.assertEqual(control.call_count, 1)
                args.show_settings = True
                with self.assertRaisesRegex(RuntimeError, "settings page requires a new instance"):
                    module.quest(args)
                args.code_file = root / "code.txt"
                with self.assertRaisesRegex(ValueError, "cannot include a code"):
                    module.quest(args)
                self.assertEqual(sum(cmd[:3] == ["shell", "am", "start"] for cmd in calls), 1)

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
                self.assertEqual(calls[count:], [(["shell","dumpsys","activity","activities"], {})])
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
                self.assertEqual(calls[count:], [(["shell","dumpsys","activity","activities"], {})])
            self.assertEqual(sum(1 for _cmd, kwargs in calls if kwargs.get("data") is not None), 2)
            self.assertFalse(any("000123" in str(cmd) for cmd, _kwargs in calls))

    def test_quest_never_reuses_a_code_after_stop_or_a_later_submission(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first, second = root / "first.txt", root / "second.txt"
            first.write_text("000123", encoding="ascii")
            second.write_text("000456", encoding="ascii")
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                      adb="adb", replace=False, code_file=first)
            pid = {"value": "", "next": 4321}
            adb_calls = []
            def fake_adb(_adb, _serial, cmd, **_kwargs):
                adb_calls.append(cmd)
                if cmd[:3] == ["shell", "am", "start"]:
                    pid["value"] = str(pid["next"])
                    pid["next"] += 1
                if cmd == ["shell", "am", "force-stop", module.PACKAGE]:
                    pid["value"] = ""
                return ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", fake_adb), \
                 mock.patch.object(module, "quest_pid", side_effect=lambda *_: pid["value"]), \
                 mock.patch.object(module, "assert_quest_share_clear"), \
                 mock.patch.object(module, "quest_control") as control:
                self.assertEqual(module.quest(args)["status"], "launched")
                self.assertEqual(module.stop_quest(args)["status"], "stopped")
                with self.assertRaisesRegex(RuntimeError, "already submitted or its outcome is unknown"):
                    module.quest(args)
                self.assertEqual(control.call_count, 1)
                # A code-less restart must not erase the earlier submission.
                args.code_file = None
                self.assertEqual(module.quest(args)["status"], "launched")
                args.code_file = second
                self.assertEqual(module.quest(args)["status"], "login_queued")
                args.code_file = first
                args.replace = True
                before = list(adb_calls)
                with self.assertRaisesRegex(RuntimeError, "already submitted or its outcome is unknown"):
                    module.quest(args)
                self.assertEqual(adb_calls, before)  # --replace must not stop the live app.
                self.assertEqual(control.call_count, 3)
            state = module.load_quest_state(root / "Saved" / "DeviceAuth" / "quest-fixture.json")
            self.assertEqual(len(state["submission_history"]), 2)
            self.assertNotIn("000123", json.dumps(state))
            self.assertNotIn("000456", json.dumps(state))

    def test_quest_interrupted_submission_is_not_replayed(self):
        for running in (False, True):
            with self.subTest(running=running), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                code = root / "code.txt"
                code.write_text("000123", encoding="ascii")
                args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                          adb="adb", replace=False, code_file=code)
                state_path = root / "Saved" / "DeviceAuth" / "quest-fixture.json"
                if running:
                    module.save_state(state_path, {"pid": "4321", "pending": False,
                                                  "config": {"server": args.server, "role": args.role}})
                with mock.patch.object(module, "ROOT", root), \
                     mock.patch.object(module, "quest_pid", return_value="4321" if running else ""), \
                     mock.patch.object(module, "assert_quest_share_clear"), \
                     mock.patch.object(module, "quest_control", side_effect=TimeoutError("lost result")) as control:
                    with self.assertRaises(TimeoutError):
                        module.quest(args)
                    with self.assertRaisesRegex(RuntimeError, "outcome is unknown"):
                        module.quest(args)
                    self.assertEqual(control.call_count, 1)
                saved = module.load_quest_state(state_path)
                self.assertEqual(saved["submission_inflight"], saved["submission_history"][0])
                self.assertNotIn("000123", json.dumps(saved))

    def test_quest_interrupted_launch_cannot_resend_the_written_code(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code, replacement = root / "code.txt", root / "replacement.txt"
            code.write_text("000123", encoding="ascii")
            replacement.write_text("000456", encoding="ascii")
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                      adb="adb", replace=False, code_file=code)
            def launch_interrupted(_adb, _serial, cmd, **_kwargs):
                if cmd[:3] == ["shell", "am", "start"]:
                    raise TimeoutError("launch result lost")
                return ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", launch_interrupted), \
                 mock.patch.object(module, "quest_pid", return_value=""), \
                 mock.patch.object(module, "assert_quest_share_clear"), \
                 mock.patch.object(module, "quest_control") as control, mock.patch.object(module.time, "sleep"):
                with self.assertRaises(TimeoutError):
                    module.quest(args)
                with self.assertRaisesRegex(RuntimeError, "already submitted"):
                    module.quest(args)
                self.assertEqual(control.call_count, 1)
                # A newly issued code can still be submitted after that failure.
                args.code_file = replacement
                with self.assertRaises(TimeoutError):
                    module.quest(args)
                self.assertEqual(control.call_count, 2)

    def test_quest_pending_launch_may_be_observed_without_resubmitting_code(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            code = root / "code.txt"
            code.write_text("000123", encoding="ascii")
            args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                      adb="adb", replace=False, code_file=code)
            pid = {"value": ""}
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", return_value=""), \
                 mock.patch.object(module, "quest_pid", side_effect=lambda *_: pid["value"]), \
                 mock.patch.object(module, "assert_quest_share_clear"), mock.patch.object(module.time, "sleep"), \
                 mock.patch.object(module, "quest_control") as control:
                self.assertEqual(module.quest(args)["status"], "launch_pending")
                pid["value"] = "4321"
                self.assertEqual(module.quest(args)["status"], "already_running")
                self.assertEqual(control.call_count, 1)
            saved = module.load_quest_state(root / "Saved" / "DeviceAuth" / "quest-fixture.json")
            self.assertFalse(saved["pending"])
            self.assertEqual(saved["submitted_pid"], "4321")

    def test_quest_corrupt_submission_journal_fails_closed(self):
        for contents in ('{', '[]', '{"submission_history":"bad"}', '{"submission_history":[null]}',
                         '{"submission_inflight":{"path":"x","modified_ns":1}}'):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                state = root / "Saved" / "DeviceAuth" / "quest-fixture.json"
                state.parent.mkdir(parents=True)
                state.write_text(contents, encoding="utf-8")
                code = root / "code.txt"
                code.write_text("000123", encoding="ascii")
                args = argparse.Namespace(server=module.DEFAULT_SERVER, serial="fixture", role="player",
                                          adb="adb", replace=False, code_file=code)
                with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "quest_pid", return_value=""), \
                     mock.patch.object(module, "quest_control") as control:
                    with self.assertRaisesRegex(RuntimeError, "journal"):
                        module.quest(args)
                    control.assert_not_called()

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

    def test_permission_inspection_is_read_only_and_fails_closed(self):
        for dump in (ORPHAN_ACTIVITY_DUMP, ORPHAN_ACTIVITY_DUMP.replace("t-1 f}}", "t101}}"),
                     ORPHAN_ACTIVITY_DUMP.replace("org.mrfps.arena", "other.package"),
                     "unparsed MediaProjectionPermissionActivity", "GrantPermissionsActivity"):
            with self.subTest(dump=dump), mock.patch.object(module, "run_adb", return_value=dump) as adb:
                with self.assertRaisesRegex(RuntimeError, "permission prompt"):
                    module.assert_quest_share_clear("adb", "fixture")
                adb.assert_called_once_with("adb", "fixture", ["shell", "dumpsys", "activity", "activities"])
        for dump in ("", "error: device offline", EMPTY_ACTIVITY_DUMP + EMPTY_ACTIVITY_DUMP):
            with self.subTest(dump=dump), mock.patch.object(module, "run_adb", return_value=dump):
                with self.assertRaises(RuntimeError): module.assert_quest_share_clear("adb", "fixture")
        with mock.patch.object(module, "run_adb", return_value=EMPTY_ACTIVITY_DUMP) as adb:
            module.assert_quest_share_clear("adb", "fixture")
            adb.assert_called_once_with("adb", "fixture", ["shell", "dumpsys", "activity", "activities"])

    def test_permission_prompt_blocks_launch_replace_and_stop_before_mutation(self):
        for running,replace,stop in ((False,False,False),(True,True,False),(True,False,True)):
            with self.subTest(running=running,replace=replace,stop=stop), tempfile.TemporaryDirectory() as folder:
                args = argparse.Namespace(server=module.DEFAULT_SERVER,serial="fixture",role="player",
                                          adb="adb",replace=replace,code_file=None)
                calls=[]
                def fake_adb(_adb,_serial,cmd,**_kwargs):
                    calls.append(cmd)
                    return ORPHAN_ACTIVITY_DUMP
                with mock.patch.object(module,"ROOT",Path(folder)), mock.patch.object(module,"run_adb",fake_adb), \
                     mock.patch.object(module,"quest_pid",return_value="111" if running else ""), \
                     mock.patch.object(module,"quest_control") as control:
                    with self.assertRaisesRegex(RuntimeError,"permission prompt"):
                        module.stop_quest(args) if stop else module.quest(args)
                    control.assert_not_called()
                self.assertEqual(calls,[["shell","dumpsys","activity","activities"]])

    def test_permission_prompt_after_stop_blocks_provisioning_and_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            args=argparse.Namespace(server=module.DEFAULT_SERVER,serial="fixture",role="player",
                                    adb="adb",replace=True,code_file=None)
            calls=[]; dumps=iter((EMPTY_ACTIVITY_DUMP,ORPHAN_ACTIVITY_DUMP))
            def fake_adb(_adb,_serial,cmd,**_kwargs):
                calls.append(cmd)
                return next(dumps) if cmd==["shell","dumpsys","activity","activities"] else ""
            with mock.patch.object(module,"ROOT",Path(folder)), mock.patch.object(module,"run_adb",fake_adb), \
                 mock.patch.object(module,"quest_pid",side_effect=["111","",""]), \
                 mock.patch.object(module,"quest_control") as control:
                with self.assertRaisesRegex(RuntimeError,"permission prompt"): module.quest(args)
                control.assert_not_called()
            self.assertIn(["shell","am","force-stop",module.PACKAGE],calls)
            self.assertFalse(any(cmd[:3]==["shell","am","start"] or cmd[:4]==["shell","am","stack","remove"] for cmd in calls))

    def test_permission_prompt_after_provisioning_blocks_launch_without_pending_journal(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            args=argparse.Namespace(server=module.DEFAULT_SERVER,serial="fixture",role="player",
                                    adb="adb",replace=False,code_file=None)
            calls=[]; dumps=iter((EMPTY_ACTIVITY_DUMP,EMPTY_ACTIVITY_DUMP,ORPHAN_ACTIVITY_DUMP))
            def fake_adb(_adb,_serial,cmd,**_kwargs):
                calls.append(cmd)
                return next(dumps) if cmd==["shell","dumpsys","activity","activities"] else ""
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb",fake_adb), \
                 mock.patch.object(module,"quest_pid",return_value=""), \
                 mock.patch.object(module,"quest_control") as control:
                with self.assertRaisesRegex(RuntimeError,"permission prompt"): module.quest(args)
                control.assert_called_once()
            state=module.load_state(root/"Saved"/"DeviceAuth"/"quest-fixture.json")
            self.assertFalse(state.get("pending",False))
            self.assertFalse(any(cmd[:3]==["shell","am","start"] or cmd[:4]==["shell","am","stack","remove"] for cmd in calls))
            self.assertEqual(calls,[["shell","dumpsys","activity","activities"]]*3)

    def test_stop_quest_targets_only_named_device_and_package_and_is_repeatable(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = argparse.Namespace(serial="quest-serial", adb="adb-fixture")
            state_path = root / "Saved" / "DeviceAuth" / "quest-quest-serial.json"
            module.save_state(state_path, {"pid":"4321", "pending":False, "config":{"role":"player"},
                                           "submitted":{"path":"private-code", "modified_ns":123}})
            calls = []
            def fake_adb(adb, serial, cmd, **_kwargs):
                calls.append((adb,serial,cmd))
                return EMPTY_ACTIVITY_DUMP if cmd == ["shell","dumpsys","activity","activities"] else ""
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb",fake_adb), \
                 mock.patch.object(module,"quest_pid",side_effect=["4321","4321","","",""]), \
                 mock.patch.object(module.time,"sleep"):
                first=module.stop_quest(args)
                second=module.stop_quest(args)
            self.assertEqual(first["status"],"stopped")
            self.assertEqual(second["status"],"already_stopped")
            self.assertEqual(calls,[("adb-fixture","quest-serial",cmd) for cmd in
                (["shell","dumpsys","activity","activities"], ["shell","am","force-stop",module.PACKAGE])]*2)
            state=module.load_state(state_path)
            self.assertIsNone(state["pid"])
            self.assertFalse(state["pending"])
            self.assertEqual(state["submitted"]["modified_ns"],123)
            self.assertEqual(state["config"],{"role":"player"})

    def test_stop_quest_cancels_pending_start_even_without_a_process(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            args=argparse.Namespace(serial="fixture",adb="adb")
            state=root/"Saved"/"DeviceAuth"/"quest-fixture.json"
            module.save_state(state,{"pid":None,"pending":True})
            def fake_adb(_adb,_serial,cmd,**_kwargs):
                return EMPTY_ACTIVITY_DUMP if cmd == ["shell","dumpsys","activity","activities"] else ""
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb",side_effect=fake_adb) as adb, \
                 mock.patch.object(module,"quest_pid",return_value=""):
                self.assertEqual(module.stop_quest(args)["status"],"stopped")
            self.assertEqual([call.args[2] for call in adb.call_args_list],
                             [["shell","dumpsys","activity","activities"],["shell","am","force-stop",module.PACKAGE]])
            self.assertFalse(module.load_state(state)["pending"])

    def test_stop_quest_fails_closed_for_invalid_serial_adb_error_or_unstopped_app(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            args=argparse.Namespace(serial="invalid;device",adb="adb")
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb") as adb:
                with self.assertRaises(ValueError): module.stop_quest(args)
                adb.assert_not_called()
            args.serial="fixture"
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb") as adb, \
                 mock.patch.object(module,"quest_pid",side_effect=RuntimeError("cannot inspect")):
                with self.assertRaises(RuntimeError): module.stop_quest(args)
                adb.assert_not_called()
            state=root/"Saved"/"DeviceAuth"/"quest-fixture.json"
            module.save_state(state,{"pid":"123", "pending":True})
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb",side_effect=RuntimeError("adb failed")), \
                 mock.patch.object(module,"quest_pid",return_value="123"):
                with self.assertRaisesRegex(RuntimeError,"adb failed"): module.stop_quest(args)
            self.assertEqual(module.load_state(state)["pid"],"123")
            def fake_adb(_adb,_serial,cmd,**_kwargs):
                return EMPTY_ACTIVITY_DUMP if cmd == ["shell","dumpsys","activity","activities"] else ""
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb",side_effect=fake_adb), \
                 mock.patch.object(module,"quest_pid",return_value="123"), mock.patch.object(module.time,"sleep"):
                with self.assertRaisesRegex(RuntimeError,"still has a process"): module.stop_quest(args)
            self.assertEqual(module.load_state(state)["pid"],"123")

    def test_stop_quest_shares_the_launch_lock(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            args=argparse.Namespace(serial="fixture",adb="adb")
            path=root/"Saved"/"DeviceAuth"/"quest-fixture.json"
            with mock.patch.object(module,"ROOT",root), mock.patch.object(module,"run_adb") as adb, \
                 mock.patch.object(module,"quest_pid") as pid, module.state_lock(path):
                with self.assertRaisesRegex(RuntimeError,"another command"): module.stop_quest(args)
                adb.assert_not_called()
                pid.assert_not_called()

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
