import datetime as dt
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock

import linux_multiplayer_probe as probe


def fixture():
    subjects={name:"fixture-"+name for name in ("player-0","player-1","inspector-0","inspector-1","inspector-2","extra-player")}
    server="\n".join([f"MRFPS_JOIN role=player slot={i} name={subjects['player-'+str(i)]}" for i in range(2)]+
                     [f"MRFPS_JOIN role=inspector slot=-1 name={subjects['inspector-'+str(i)]}" for i in range(3)]+
                     ["MRFPS_CALIBRATED server slot=0 ready=1", "MRFPS_CALIBRATED server slot=1 ready=1",
                      "MRFPS_MATCH playing", "MRFPS_WALL", "MRFPS_DAMAGE", "MRFPS_BLOCK",
                      "MRFPS_ELIMINATION winner=0 victim=1 kills=1"])
    logs={"extra-player":"ARENA_FULL"}
    for i in range(3):
        logs["inspector-"+str(i)]="\n".join(["MRFPS_SNAPSHOT players=2 role=inspector phase=1 walls=1 p1hp=0.0 kills0=1",
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

    def test_elimination_must_be_replicated_during_play_by_each_inspector(self):
        server,logs,subjects=fixture()
        self.assertTrue(all(probe.playing_score_checks(server,logs,subjects).values()))
        logs["inspector-2"]=logs["inspector-2"].replace("phase=1","phase=2")
        result=probe.playing_score_checks(server,logs,subjects)
        self.assertTrue(result["ServerElimination"])
        self.assertFalse(result["Inspector2ReplicatedPlayingScore"])
        logs["inspector-2"]=logs["inspector-2"].replace("phase=2", "phase=1").replace("kills0=1", "kills0=2")
        self.assertFalse(probe.playing_score_checks(server,logs,subjects)["Inspector2ReplicatedPlayingScore"])
        server=server.replace("MRFPS_ELIMINATION winner=0 victim=1 kills=1", "MRFPS_MATCH winner=0")
        self.assertFalse(any(probe.playing_score_checks(server,logs,subjects).values()))

    def test_historical_bot_match_cannot_satisfy_two_human_combat_checks(self):
        server,logs,subjects=fixture()
        stale="MRFPS_MATCH playing\nMRFPS_WALL\nMRFPS_DAMAGE\nMRFPS_BLOCK\nMRFPS_ELIMINATION winner=0 victim=1 kills=1\n"
        current=server.replace("MRFPS_MATCH playing", "").replace("MRFPS_WALL", "").replace("MRFPS_DAMAGE", "")
        current=current.replace("MRFPS_BLOCK", "").replace("MRFPS_ELIMINATION winner=0 victim=1 kills=1", "")
        result=probe.baseline_checks(stale+current,logs,subjects,True,True)
        self.assertFalse(result["MatchStarted"])
        self.assertFalse(result["WallSpawned"])
        self.assertFalse(result["AuthoritativeDamage"])
        self.assertFalse(result["ShieldBlockedShot"])
        self.assertFalse(any(probe.playing_score_checks(stale+current,logs,subjects).values()))
        # Even if the two slots join eventually, a match before the second join
        # is still a one-human/Bot match and cannot become current evidence.
        first=f"MRFPS_JOIN role=player slot=0 name={subjects['player-0']}\n"
        second=f"MRFPS_JOIN role=player slot=1 name={subjects['player-1']}\n"
        self.assertEqual(probe.combat_scope(first+stale+second,subjects)[1], "")

    def test_only_snapshots_after_observed_elimination_can_prove_replication(self):
        server,logs,subjects=fixture()
        old=logs["inspector-0"]
        new="\nMRFPS_SNAPSHOT players=2 role=inspector phase=1 walls=1 p1hp=100.0 kills0=0\n"
        logs["inspector-0"]=old+new
        scoped=probe.scoped_logs(logs,{"inspector-0":len(old)})
        self.assertFalse(probe.playing_score_checks(server,scoped,subjects)["Inspector0ReplicatedPlayingScore"])
        logs["inspector-0"] += "MRFPS_SNAPSHOT players=2 role=inspector phase=1 walls=1 p1hp=100.0 kills0=1\n"
        scoped=probe.scoped_logs(logs,{"inspector-0":len(old)})
        self.assertTrue(probe.playing_score_checks(server,scoped,subjects)["Inspector0ReplicatedPlayingScore"])
        with self.assertRaisesRegex(RuntimeError,"CLIENT_LOG_REPLACED_OR_TRUNCATED"):
            probe.scoped_logs({"inspector-0":"short"},{"inspector-0":100})

    def test_calibration_pending_reuses_the_same_operation_id(self):
        instance=object.__new__(probe.Multiplayer)
        instance.instances=[{"name":"player-0","control_name":"MRFPS-LinuxSmoke-fixture-player-0"}]
        seen=[]
        def fake_run(command, **_kwargs):
            operation=command[command.index("--operation-id")+1]
            seen.append(operation)
            status="pending" if len(seen)==1 else "submitted"
            return __import__("subprocess").CompletedProcess(command,2,json.dumps({"id":operation,"status":status}).encode(),b"")
        def fake_wait(_stage, condition):
            self.assertFalse(condition())
            self.assertTrue(condition())
        instance.wait=fake_wait
        with mock.patch.object(probe.subprocess,"run",side_effect=fake_run):
            instance.calibrate("player-0")
        self.assertEqual(len(seen),2)
        self.assertEqual(seen[0],seen[1])

    def test_both_fighters_join_before_either_calibration(self):
        with tempfile.TemporaryDirectory() as directory:
            instance=object.__new__(probe.Multiplayer)
            instance.args=types.SimpleNamespace(stable_seconds=0)
            instance.directory=pathlib.Path(directory)
            instance.initial={"id":"fixture"}
            instance.report={}
            instance.instances=[]
            instance.subjects={}
            events=[]
            class Process:
                def __init__(self): self.alive=True
                def poll(self): return None if self.alive else 0
                def kill(self): self.alive=False
                def wait(self,timeout): return 0
            def start(name):
                events.append("start:"+name)
                instance.subjects[name]="fixture-"+name
                instance.instances.append({"name":name,"process":Process(),"log":instance.directory/(name+".log")})
            instance.start=start
            instance.wait=lambda stage,_condition,timeout=None: events.append("wait:"+stage)
            instance.calibrate=lambda name: events.append("calibrate:"+name)
            instance.text=lambda _path: ""
            instance.evaluate=lambda: ("",{},True)
            instance.persist=lambda: None
            instance.state=lambda: instance.initial
            instance.execute_multiplayer()
            first=events.index("calibrate:player-0")
            self.assertLess(events.index("start:player-1"),first)
            self.assertLess(events.index("wait:slot1_joined"),first)
            self.assertLess(events.index("wait:both_fighters_replicated"),first)
            self.assertLess(first,events.index("calibrate:player-1"))
            self.assertTrue(instance.report["passed"])

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
            self.assertFalse(plan["round_result_verified"])
            self.assertFalse(plan["match_result_verified"])
            self.assertEqual(plan["expected_round_duration_seconds"],180)
            self.assertEqual(plan["stability_seconds"],30)
            self.assertEqual(plan["required_qa_server_arguments"],[probe.QA_AUTO_START_ARGUMENT])
            self.assertNotIn("ScoreToWin",json.dumps(plan))


if __name__ == "__main__":
    unittest.main()
