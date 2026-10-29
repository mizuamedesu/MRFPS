import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock

import packaged_probe as probe


class PackagedProbeTests(unittest.TestCase):
    def test_join_requires_the_same_issued_subject_for_both_server_markers(self):
        subject="inspector-12345678-1234-4234-8234-123456789abc"
        other="inspector-22345678-1234-4234-8234-123456789abc"
        digest=hashlib.sha256(subject.encode()).hexdigest()
        accept=f"MRFPS_AUTH_ACCEPT role=inspector subject={subject} expires=123\n"
        self.assertFalse(probe.server_join_matches(accept,digest))
        self.assertFalse(probe.server_join_matches(accept+f"MRFPS_JOIN role=inspector slot=-1 name={other}\n",digest))
        self.assertTrue(probe.server_join_matches(accept+f"MRFPS_JOIN role=inspector slot=-1 name={subject}\n",digest))
        player=subject.replace("inspector-","player-")
        player_hash=hashlib.sha256(player.encode()).hexdigest()
        player_log=f"MRFPS_AUTH_ACCEPT role=player subject={player} expires=123\nMRFPS_JOIN role=player slot=0 name={player}\n"
        self.assertTrue(probe.server_join_matches(player_log,player_hash,"player"))
        self.assertFalse(probe.server_join_matches(player_log,player_hash,"inspector"))
        self.assertFalse(probe.server_join_matches(player_log.replace("slot=0","slot=-1"),player_hash,"player"))

    def test_issuer_must_match_the_exact_fresh_code_and_server(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); code=root/"inspector.code"; code.write_text("000123",encoding="ascii")
            subject="inspector-12345678-1234-4234-8234-123456789abc"
            state={"fingerprint":"a"*64,"stopped":False,"roles":{"inspector":{
                "code":"000123","subject":subject,"expires_at":int(time.time())+30}}}
            (root/"state.json").write_text(json.dumps(state),encoding="utf-8")
            self.assertEqual(probe.issuer_identity(root,code,"a"*64),hashlib.sha256(subject.encode()).hexdigest())
            with self.assertRaises(RuntimeError): probe.issuer_identity(root,code,"b"*64)
            code.write_text("000124",encoding="ascii")
            with self.assertRaises(RuntimeError): probe.issuer_identity(root,code,"a"*64)
            player=subject.replace("inspector-","player-")
            state["roles"]["player"]={"code":"000124","subject":player,"expires_at":int(time.time())+30}
            (root/"state.json").write_text(json.dumps(state),encoding="utf-8")
            self.assertEqual(probe.issuer_identity(root,code,"a"*64,"player"),hashlib.sha256(player.encode()).hexdigest())

    def test_package_selection_checks_native_location_content_and_digest(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/"Windows"/"MRFPS"; binary=root/"Binaries"/"Win64"/"MRFPS.exe"
            binary.parent.mkdir(parents=True); binary.write_bytes(b"native client")
            paks=root/"Content"/"Paks"; paks.mkdir(parents=True)
            (paks/"game.pak").write_bytes(b"pak"); (paks/"game.utoc").write_bytes(b"utoc")
            digest=hashlib.sha256(binary.read_bytes()).hexdigest()
            self.assertEqual(probe.verify_packaged_binary(binary,digest),binary.resolve())
            with self.assertRaises(RuntimeError): probe.verify_packaged_binary(binary,"0"*64)
            with self.assertRaises(ValueError): probe.verify_packaged_binary(paks/"game.pak",digest)

    def test_start_reuses_the_same_guarded_run(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); run=root/"run"; run.mkdir()
            binary=root/"Windows"/"MRFPS"/"Binaries"/"Win64"/"MRFPS.exe"; binary.parent.mkdir(parents=True); binary.write_bytes(b"exe")
            paks=binary.parents[2]/"Content"/"Paks"; paks.mkdir(parents=True)
            (paks/"game.pak").write_bytes(b"x"); (paks/"game.utoc").write_bytes(b"x")
            admin=root/"admin"; admin.mkdir()
            for name in ("issuer.encrypted.json","issuer-passphrase.txt","basic-credentials.json"): (admin/name).write_text("fixture")
            args=argparse.Namespace(executable=str(binary),sha256=hashlib.sha256(b"exe").hexdigest(),server="localhost:7777",
                                    container="fixture",fingerprint="a"*64,admin_dir=str(admin))
            with mock.patch.object(probe,"server_snapshot",return_value=({"id":"fixture"},"")), \
                 mock.patch.object(probe,"memory_snapshot",return_value={"free_gib":16,"commit_percent":50}), \
                 mock.patch.object(probe.client,"windows",return_value={"status":"launching","guard_pid":42}) as launch:
                self.assertEqual(probe.start(run,args,{})["status"],"launching")
                self.assertEqual(probe.start(run,args,probe.client.load_state(run/"state.json"))["status"],"already_started")
                launch.assert_called_once()

    def test_low_memory_stops_before_client_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); run=root/"run"; run.mkdir()
            binary=root/"Windows"/"MRFPS"/"Binaries"/"Win64"/"MRFPS.exe"; binary.parent.mkdir(parents=True); binary.write_bytes(b"exe")
            paks=binary.parents[2]/"Content"/"Paks"; paks.mkdir(parents=True)
            (paks/"game.pak").write_bytes(b"x"); (paks/"game.utoc").write_bytes(b"x")
            admin=root/"admin"; admin.mkdir()
            for name in ("issuer.encrypted.json","issuer-passphrase.txt","basic-credentials.json"): (admin/name).write_text("fixture")
            args=argparse.Namespace(executable=str(binary),sha256=hashlib.sha256(b"exe").hexdigest(),server="localhost:7777",
                                    container="fixture",fingerprint="a"*64,admin_dir=str(admin))
            with mock.patch.object(probe,"server_snapshot",return_value=({"id":"fixture"},"")), \
                 mock.patch.object(probe,"memory_snapshot",return_value={"free_gib":12.4,"commit_percent":50}), \
                 mock.patch.object(probe.client,"windows") as launch:
                with self.assertRaisesRegex(RuntimeError,"memory headroom"): probe.start(run,args,{})
                launch.assert_not_called()

    def test_observe_only_reports_matches_and_omits_subject(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder); binary=run/"MRFPS.exe"; binary.write_bytes(b"binary")
            (run/"engine.log").write_text("UPendingNetGame::TravelCompleted Pending net game travel completed\n",encoding="utf-8")
            subject="inspector-12345678-1234-4234-8234-123456789abc"
            digest=hashlib.sha256(subject.encode()).hexdigest()
            server_log=f"MRFPS_AUTH_ACCEPT role=inspector subject={subject} expires=123\nMRFPS_JOIN role=inspector slot=-1 name={subject}\n"
            identity={"id":"server"}
            state={"name":"MRFPS-Package-fixture","executable":str(binary),"sha256":hashlib.sha256(b"binary").hexdigest(),
                   "container":"fixture","fingerprint":"a"*64,"server_identity":identity,"subject_hash":digest,
                   "checks":{"initial_window_title":True},"actions":{}}
            status={"status":"running","guard_report":str(run/"report-guard.json"),"events":["MRFPS_DEVICE_EXCHANGE_VERIFIED role=inspector","MRFPS_DEVICE_CONNECT role=inspector"]}
            with mock.patch.object(probe.client,"status",return_value=status), mock.patch.object(probe.client,"load_state",return_value={"pid":42,"created":99}), \
                 mock.patch.object(probe,"window_process",return_value={"pid":50,"image":str(binary),"title":state["name"]}), \
                 mock.patch.object(probe,"server_snapshot",return_value=(identity,server_log)):
                result=probe.inspect(run,state)
            self.assertTrue(result["checks"]["server_auth_join_matches_issued_subject"])
            self.assertTrue(result["checks"]["connected_window_title"])
            report=(run/"report.json").read_text(encoding="utf-8")
            self.assertNotIn(subject,report)
            self.assertNotIn(digest,report)

    def test_ambiguous_code_submission_is_not_retried(self):
        with tempfile.TemporaryDirectory() as folder:
            state={"name":"fixture","login_intent":True,"code_marker":{"path":"code","modified_ns":1},"checks":{},"actions":{}}
            with mock.patch.object(probe.client,"load_state",return_value={}), mock.patch.object(probe.client,"windows") as windows:
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(Path(folder),state)
                windows.assert_not_called()

    def test_inflight_even_with_matching_old_marker_is_not_claimed_submitted(self):
        with tempfile.TemporaryDirectory() as folder:
            marker={"path":"code","modified_ns":1}
            state={"name":"fixture","login_intent":True,"code_marker":marker,"checks":{},"actions":{}}
            with mock.patch.object(probe.client,"load_state",return_value={"submitted":marker,"submission_inflight":marker}), \
                 mock.patch.object(probe.client,"windows") as windows:
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(Path(folder),state)
                windows.assert_not_called()

    def test_stop_records_a_clean_close_and_a_safe_repeat(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder); state={"name":"fixture","issuer_dir":str(run/"unused"),"checks":{},"actions":{}}
            status={"guard_report":str(run/"guard.json")}
            guard={"status":"completed","childExitCode":0}
            with mock.patch.object(probe.client,"stop_windows",side_effect=[{"status":"stopped"},{"status":"already_stopped"}]), \
                 mock.patch.object(probe.client,"status",return_value=status), mock.patch.object(probe.client,"load_state",return_value=guard):
                self.assertEqual(probe.stop(run,state)["status"],"stopped")
                self.assertEqual(probe.stop(run,state)["status"],"already_stopped")
            self.assertEqual(state["actions"]["stop"],"stopped")
            self.assertTrue(state["actions"]["stop_repeated"])
            self.assertTrue(state["checks"]["graceful_guard_exit"])

    def test_preexisting_exit_is_not_misreported_as_a_graceful_stop(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder); state={"name":"fixture","issuer_dir":str(run/"unused"),"checks":{},"actions":{}}
            with mock.patch.object(probe.client,"stop_windows",return_value={"status":"already_stopped"}), \
                 mock.patch.object(probe.client,"status",return_value={"guard_report":str(run/"guard.json")}), \
                 mock.patch.object(probe.client,"load_state",return_value={"status":"completed","childExitCode":0}):
                probe.stop(run,state)
            self.assertFalse(state["checks"]["graceful_guard_exit"])


if __name__=="__main__": unittest.main()
