import base64
import datetime as dt
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec=importlib.util.spec_from_file_location("role_probe",Path(__file__).with_name("role_probe.py"))
probe=importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)


class RoleProbeTests(unittest.TestCase):
    def test_subject_role_and_server_binding_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"token"
            payload=base64.urlsafe_b64encode(json.dumps({"sub":"player-fixture","role":"player","dtls_fp":"a"*64}).encode()).decode().rstrip("=")
            path.write_text("eyJ"+"a"*20+"."+payload+"."+"b"*86,encoding="ascii")
            self.assertEqual(probe.safe_subject(path,"player","a"*64),"player-fixture")
            with self.assertRaises(RuntimeError): probe.safe_subject(path,"admin","a"*64)
            with self.assertRaises(RuntimeError): probe.safe_subject(path,"player","b"*64)

    def test_acceptance_needs_matching_authentication_and_join(self):
        instance=probe.Probe.__new__(probe.Probe)
        item={"role":"inspector","subject":"inspector-abc"}
        with mock.patch.object(instance,"logs",return_value="MRFPS_AUTH_ACCEPT role=inspector subject=inspector-abc expires=5\n"):
            self.assertFalse(instance.accepted(item))
        with mock.patch.object(instance,"logs",return_value="MRFPS_AUTH_ACCEPT role=inspector subject=inspector-abc expires=5\nMRFPS_JOIN role=inspector slot=-1 name=inspector-abc\n"):
            self.assertTrue(instance.accepted(item))

    def test_snapshot_must_be_recent_and_role_scoped(self):
        instance=probe.Probe.__new__(probe.Probe)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"client.log"
            now=dt.datetime.now(dt.timezone.utc).strftime("%Y.%m.%d-%H.%M.%S:%f")[:-3]
            path.write_text(f"[{now}][  7]LogMRFPS: MRFPS_SNAPSHOT players=2 role=inspector\n",encoding="utf-8")
            self.assertEqual(instance.snapshot_players({"log":path}),2)
            path.write_text("[2000.01.01-00.00.00:000][  7]LogMRFPS: MRFPS_SNAPSHOT players=2 role=inspector\n",encoding="utf-8")
            self.assertIsNone(instance.snapshot_players({"log":path}))

    def test_stop_command_is_idempotent(self):
        with tempfile.TemporaryDirectory() as folder:
            for _ in range(2):
                result=subprocess.run([sys.executable,str(Path(__file__).with_name("role_probe.py")),"stop","--run-dir",folder],capture_output=True,text=True,check=True)
                self.assertEqual(json.loads(result.stdout)["status"],"stop_requested")
            self.assertTrue((Path(folder)/"STOP").exists())


if __name__=="__main__": unittest.main()
