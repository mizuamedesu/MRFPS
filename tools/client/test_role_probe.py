import base64
import datetime as dt
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec=importlib.util.spec_from_file_location("role_probe",Path(__file__).with_name("role_probe.py"))
probe=importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)


class RoleProbeTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows image identity is queried with Win32")
    def test_running_process_image_matches_actual_python_executable(self):
        self.assertTrue(os.path.samefile(probe.process_image(os.getpid()),sys.executable))

    def test_executable_digest_changes_with_contents(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"fixture.bin"
            path.write_bytes(b"first executable")
            before=probe.file_sha256(path)
            path.write_bytes(b"second executable")
            self.assertEqual(len(before),64)
            self.assertNotEqual(before,probe.file_sha256(path))

    def test_solo_status_requires_actual_bot_round_and_explicit_missing_scan(self):
        data={"connected":True,"solo_bot":True,"phase":1,"round":1,"round_seconds_remaining":179,
              "solo_measured_geometry_received":False,"solo_obstacle_data":"not_received"}
        self.assertTrue(probe.solo_status_ready(data))
        self.assertTrue(probe.solo_status_ready({**data,"round_seconds_remaining":179.0}))
        for key,value in (("solo_bot",False),("phase",0),("phase",True),("round",0),("round_seconds_remaining",0),
                          ("round_seconds_remaining",True),("solo_measured_geometry_received",True),
                          ("solo_obstacle_data","received")):
            changed={**data,key:value}
            self.assertFalse(probe.solo_status_ready(changed),key)

    def test_solo_replacement_requires_waiting_and_clean_scores_and_registration(self):
        data={"connected":True,"solo_bot":False,"phase":0,"round":0,"completed_rounds":0,
              "round_wins_0":0,"round_wins_1":0,"shared_floor_confirmed":False,"layout_calibrated":False}
        self.assertTrue(probe.solo_replacement_ready(data))
        for key,value in (("solo_bot",True),("phase",1),("phase",False),("round",1),("completed_rounds",1),
                          ("round_wins_0",1),("round_wins_1",1),("shared_floor_confirmed",True),("layout_calibrated",True)):
            changed={**data,key:value}
            self.assertFalse(probe.solo_replacement_ready(changed),key)

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
