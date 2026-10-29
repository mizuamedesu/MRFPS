import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("device_login", Path(__file__).with_name("device_login.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DeviceLoginControlTests(unittest.TestCase):
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
                return ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", fake_adb), mock.patch.object(module, "quest_pid", side_effect=["", "4321", "4321"]):
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
                return "/data/user/0/" + module.PACKAGE if cmd == ["shell", "run-as", module.PACKAGE, "pwd"] else ""
            with mock.patch.object(module, "ROOT", root), mock.patch.object(module, "run_adb", fake_adb), mock.patch.object(module, "quest_pid", side_effect=["", "4321", "4321", "4321"]):
                self.assertEqual(module.quest(args)["status"], "launched")
                args.code_file = code
                self.assertEqual(module.quest(args)["status"], "login_queued")
                count = len(calls)
                self.assertEqual(module.quest(args)["status"], "already_running")
                self.assertEqual(len(calls), count)
            self.assertEqual(sum(1 for _cmd, kwargs in calls if kwargs.get("data") is not None), 2)
            self.assertFalse(any("000123" in str(cmd) for cmd, _kwargs in calls))

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
