import datetime as dt
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

import linux_multiplayer_probe as probe


def fixture():
    subjects={name:"fixture-"+name for name in ("player-0","player-1","inspector-0","inspector-1","inspector-2","extra-player")}
    server="\n".join([f"MRFPS_JOIN role=player slot={i} name={subjects['player-'+str(i)]}" for i in range(2)]+
                     [f"MRFPS_JOIN role=inspector slot=-1 name={subjects['inspector-'+str(i)]}" for i in range(3)]+
                     ["MRFPS_CALIBRATED server slot=0 ready=1", "MRFPS_CALIBRATED server slot=1 ready=1",
                      "MRFPS_MATCH playing", "MRFPS_WALL", "MRFPS_DAMAGE", "MRFPS_BLOCK",
                      "MRFPS_ELIMINATION winner=0 victim=1 kills=1", "MRFPS_MATCH winner=0"])
    logs={"extra-player":"ARENA_FULL"}
    for i in range(3):
        logs["inspector-"+str(i)]="\n".join(["MRFPS_SNAPSHOT players=2 role=inspector phase=2 walls=1 p1hp=0.0 kills0=1",
                                               "MRFPS_VIEW inspector=player slot=0", "MRFPS_VIEW inspector=player slot=1", "MRFPS_VIEW inspector=free"])
    return server,logs,subjects


class MultiplayerEvidenceTests(unittest.TestCase):
    def test_all_27_baseline_checks_require_real_marker_inputs(self):
        server,logs,subjects=fixture()
        checks=probe.baseline_checks(server,logs,subjects,True,True)
        self.assertEqual(len(checks),27)
        self.assertTrue(all(checks.values()))

    def test_each_inspector_must_independently_receive_wall_damage_and_views(self):
        server,logs,subjects=fixture()
        logs["inspector-1"]="MRFPS_SNAPSHOT players=2 role=inspector phase=1 walls=0 p1hp=100.0 kills0=0"
        checks=probe.baseline_checks(server,logs,subjects,True,True)
        self.assertFalse(checks["Inspector1ReceivedWall"])
        self.assertFalse(checks["Inspector1ReceivedDamage"])
        self.assertFalse(checks["Inspector1ViewedBothPlayers"])
        self.assertTrue(checks["Inspector0ReceivedWall"])

    def test_extra_join_privacy_and_missing_pin_fail_independently(self):
        server,logs,subjects=fixture()
        checks=probe.baseline_checks(server+"\nMRFPS_JOIN role=player slot=2 name=unexpected",logs,subjects,False,False)
        self.assertFalse(checks["ExactlyTwoPlayerJoins"])
        self.assertFalse(checks["NoCredentialLeak"])
        self.assertFalse(checks["PublicKeyAuthenticatedServer"])

    def test_server_win_alone_is_not_replicated_scoreboard_evidence(self):
        server,logs,_=fixture()
        self.assertTrue(all(probe.victory_checks(server,logs).values()))
        logs["inspector-2"]=logs["inspector-2"].replace("phase=2","phase=1")
        self.assertFalse(probe.victory_checks(server,logs)["Inspector2ReplicatedFinishedScoreboard"])

    def test_latest_snapshot_keeps_role_count_and_true_utc_age(self):
        timestamp=dt.datetime.now(dt.timezone.utc).strftime("%Y.%m.%d-%H.%M.%S:")+"000"
        state=probe.snapshot("["+timestamp+"][ 1]LogMRFPS: MRFPS_SNAPSHOT players=2 role=inspector")
        self.assertEqual(state["players"],2)
        self.assertEqual(state["role"],"inspector")
        self.assertGreaterEqual(state["age_seconds"],0)
        self.assertIsNone(probe.snapshot("not a snapshot"))

    def test_packaged_bootstrap_resolves_existing_native_game(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory)
            outer=root/"MRFPS.exe";outer.write_bytes(b"synthetic-launcher")
            inner=root/"MRFPS/Binaries/Win64/MRFPS.exe";inner.parent.mkdir(parents=True);inner.write_bytes(b"synthetic-native")
            self.assertEqual(probe.native_executable(outer),inner.resolve())

    def test_dry_run_does_not_initialize_keys_docker_or_clients(self):
        with tempfile.TemporaryDirectory() as directory:
            report=pathlib.Path(directory)/"plan"
            arguments=["probe", "--executable", "absent/MRFPS.exe", "--project", "absent/MRFPS.uproject",
                       "--private-key", "absent/private.pem", "--public-key", "absent/public.pem",
                       "--fingerprint", "0"*64, "--report-directory", str(report), "--dry-run", "--packaged"]
            with mock.patch.object(sys,"argv",arguments), mock.patch.object(probe,"Multiplayer",side_effect=AssertionError("NO_RUNTIME")), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main(),0)
            plan=json.loads((report/"plan.json").read_text())
            self.assertFalse(plan["passed"])
            self.assertFalse(plan["issuer_key_read"])
            self.assertFalse(plan["docker_commands_run"])
            self.assertFalse(plan["ue_processes_started"])


if __name__ == "__main__":
    unittest.main()
