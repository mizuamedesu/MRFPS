"""Validate endpoint restrictions and inspect the actual ADB calls without hardware."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from quest_bridge import BridgeError, QuestBridge, validate_discovery


def session(port=41234, token="a" * 64):
    return dict(discovery_version=1, protocol_version=2, app_version="0.1.13",
                session_id="12345678-abcd-1234-abcd-123456789abc", token=token,
                control_url=f"ws://127.0.0.1:{port}/mobits/v2/control",
                motion_url=f"ws://127.0.0.1:{port}/mobits/v2/motion")


def encoded(value):
    return json.dumps(value).encode()


def adb_success(command, **kwargs):
    output = b""
    if command[-1] == "pwd":
        package = command[command.index("run-as") + 1]
        output = f"/data/user/0/{package}\n".encode()
    return subprocess.CompletedProcess(command, 0, stdout=output, stderr=b"")


class QuestBridgeTests(unittest.TestCase):
    def test_reject_remote_endpoints_and_header_injection(self):
        for url in ["ws://192.168.1.1:41234/mobits/v2/control", "ws://localhost:41234/mobits/v2/control",
                    "ws://127.0.0.1:041234/mobits/v2/control", "ws://127.0.0.1:0/mobits/v2/control",
                    "ws://127.0.0.1:65536/mobits/v2/control", "ws://127.0.0.1:41234/mobits/v2/control?token=x",
                    "ws://127.0.0.1:41234/mobits/v2/control\r\nX: value"]:
            value = session()
            value["control_url"] = url
            with self.subTest(url=url), self.assertRaises(BridgeError):
                validate_discovery(encoded(value))

    def test_reject_mismatched_ports_and_invalid_auth_without_echoing_secrets(self):
        value = session()
        value["motion_url"] = "ws://127.0.0.1:12345/mobits/v2/motion"
        with self.assertRaises(BridgeError):
            validate_discovery(encoded(value))
        value = session(token="SECRET\r\nheader")
        with self.assertRaises(BridgeError) as caught:
            validate_discovery(encoded(value))
        self.assertNotIn("SECRET", str(caught.exception))
        self.assertNotIn("header", str(caught.exception))

    def test_invalid_discovery_shapes_and_versions(self):
        for raw in [b"[]", b"null", b"\xff", b"{}", b"x" * 4097]:
            with self.subTest(raw=raw[:20]), self.assertRaises(BridgeError):
                validate_discovery(raw)
        for key, invalid in [("protocol_version", 1), ("discovery_version", True), ("app_version", ""),
                             ("session_id", "123\r\nX: injected")]:
            value = session()
            value[key] = invalid
            with self.subTest(key=key), self.assertRaises(BridgeError):
                validate_discovery(encoded(value))

    def test_reverse_and_private_atomic_stdin_transfer_without_credentials_in_commands(self):
        calls = []
        raw = encoded(session())
        def run(command, **kwargs):
            calls.append((command, kwargs))
            return adb_success(command, **kwargs)
        with patch("quest_bridge.subprocess.run", side_effect=run):
            bridge = QuestBridge("adb", "com.mrfps.game", "TEST_SERIAL")
            self.assertTrue(bridge.install(raw))
            self.assertFalse(bridge.install(raw))
            self.assertEqual(bridge.directory, "/data/user/0/com.mrfps.game/files/MoBits/SDK")
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[0][0], ["adb", "-s", "TEST_SERIAL", "shell", "run-as", "com.mrfps.game", "pwd"])
        self.assertEqual(calls[1][0], ["adb", "-s", "TEST_SERIAL", "reverse", "tcp:41234", "tcp:41234"])
        command, options = calls[2]
        self.assertEqual(options["input"], raw)
        self.assertNotIn("stdin", options)
        self.assertEqual(options["stdout"], subprocess.DEVNULL)
        self.assertEqual(options["stderr"], subprocess.PIPE)
        self.assertEqual(command[-3:-1], ["shell", "-T"])
        script = command[-1]
        self.assertIn("umask 077", script)
        self.assertIn("chmod 700 files/MoBits files/MoBits/SDK", script)
        self.assertLess(script.index("cat >"), script.index("chmod 600"))
        self.assertLess(script.index("chmod 600"), script.index("mv -f"))
        for command, options in calls:
            self.assertNotIn("a" * 64, " ".join(command))
            self.assertNotIn(session()["session_id"], " ".join(command))
            self.assertNotIn("ARM_HAPTICS", " ".join(command))
            self.assertNotIn("/sdcard", " ".join(command))
            self.assertNotIn("push", command)
            self.assertNotIn("env", options)

    def test_rotation_removes_only_previously_owned_port(self):
        with patch("quest_bridge.subprocess.run", side_effect=adb_success) as run:
            bridge = QuestBridge("adb", "com.mrfps.game")
            bridge.install(encoded(session(30000)))
            bridge.install(encoded(session(30001, "b" * 64)))
            self.assertEqual(run.call_args_list[-1].args[0], ["adb", "reverse", "--remove", "tcp:30000"])
            bridge.disconnect()
            self.assertEqual(run.call_args_list[-1].args[0], ["adb", "reverse", "--remove", "tcp:30001"])

    def test_no_adb_call_for_invalid_input(self):
        with patch("quest_bridge.subprocess.run") as run:
            with self.assertRaises(BridgeError):
                QuestBridge("adb", "com.mrfps.game;id")
            with self.assertRaises(BridgeError):
                QuestBridge("adb", "com.mrfps.game").install(b"not-json")
            run.assert_not_called()

    def test_adb_reconnect_reinstalls_lost_reverse(self):
        state = {"reverse": b"UsbFfs tcp:41234 tcp:41234\n"}
        def run(command, **kwargs):
            if command[-2:] == ["reverse", "--list"]:
                return subprocess.CompletedProcess(command, 0, stdout=state["reverse"])
            return adb_success(command, **kwargs)
        with patch("quest_bridge.subprocess.run", side_effect=run):
            bridge = QuestBridge("adb", "com.mrfps.game")
            raw = encoded(session())
            bridge.install(raw)
            bridge.refresh_if_transport_lost()
            self.assertFalse(bridge.install(raw))
            state["reverse"] = b""
            bridge.refresh_if_transport_lost()
            self.assertTrue(bridge.install(raw))

    def test_invalid_private_root_is_rejected_before_reverse_or_stdin(self):
        for root in [b"/sdcard/Android/data/com.mrfps.game\n", b"/data/user/0/com.other.game\n",
                     b"/data/user/0/com.mrfps.game;id\n", b"/data/user/0/com.mrfps.game/../other\n",
                     b"/data/user/0/com.mrfps.game\nINJECTED", b"\xff", b"x" * 513]:
            with self.subTest(root=root[:40]), patch("quest_bridge.subprocess.run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, stdout=root)
                with self.assertRaises(BridgeError):
                    QuestBridge("adb", "com.mrfps.game").install(encoded(session()))
                self.assertEqual(run.call_count, 1)
                self.assertNotIn("input", run.call_args.kwargs)

    def test_internal_data_alias_and_other_android_user_are_supported(self):
        for root in ["/data/data/com.mrfps.game", "/data/user/10/com.mrfps.game"]:
            def run(command, **kwargs):
                if command[-1] == "pwd":
                    return subprocess.CompletedProcess(command, 0, stdout=(root + "\n").encode())
                return adb_success(command, **kwargs)
            with self.subTest(root=root), patch("quest_bridge.subprocess.run", side_effect=run):
                bridge = QuestBridge("adb", "com.mrfps.game")
                self.assertTrue(bridge.install(encoded(session())))
                self.assertEqual(bridge.directory, root + "/files/MoBits/SDK")

    def test_run_as_denied_does_not_leak_stderr_or_install_reverse(self):
        failure = subprocess.CalledProcessError(1, ["adb"], stderr=b"PRIVATE_SESSION_INFORMATION")
        with patch("quest_bridge.subprocess.run", side_effect=failure) as run:
            with self.assertRaises(BridgeError) as caught:
                QuestBridge("adb", "com.mrfps.game").install(encoded(session()))
            self.assertNotIn("PRIVATE_SESSION_INFORMATION", str(caught.exception))
            self.assertEqual(run.call_count, 1)

    def test_failed_rotation_preserves_old_session_and_rolls_back_new_reverse(self):
        state = {"fail_transfer": False}
        def run(command, **kwargs):
            if state["fail_transfer"] and "input" in kwargs:
                raise subprocess.CalledProcessError(1, command, stderr=b"PRIVATE_SESSION_INFORMATION")
            return adb_success(command, **kwargs)
        with patch("quest_bridge.subprocess.run", side_effect=run) as adb:
            bridge = QuestBridge("adb", "com.mrfps.game")
            old = encoded(session(30000))
            bridge.install(old)
            state["fail_transfer"] = True
            with self.assertRaises(BridgeError) as caught:
                bridge.install(encoded(session(30001, "b" * 64)))
            self.assertNotIn("PRIVATE_SESSION_INFORMATION", str(caught.exception))
            self.assertEqual(adb.call_args_list[-1].args[0], ["adb", "reverse", "--remove", "tcp:30001"])
            self.assertEqual(bridge.port, 30000)
            self.assertFalse(bridge.install(old))


if __name__ == "__main__":
    unittest.main()
