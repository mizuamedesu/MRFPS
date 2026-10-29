import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock

import packaged_probe as probe


class PackagedProbeTests(unittest.TestCase):
    def make_issuer(self,root,role="inspector"):
        issuer=root/"issuer"; issuer.mkdir()
        code=issuer/(role+".code"); code.write_text("000123",encoding="ascii")
        subject=role+"-12345678-1234-4234-8234-123456789abc"
        issued={"version":1,"registered":True,"owner":"12345678-1234-4234-8234-123456789abc",
                "fingerprint":"a"*64,"stopped":False,"roles":{role:{
                    "id":"32345678-1234-4234-8234-123456789abc","code":"000123",
                    "subject":subject,"expires_at":int(time.time())+30}}}
        (issuer/"state.json").write_text(json.dumps(issued),encoding="utf-8")
        return issuer,code,issued

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
            state={"version":1,"registered":True,"owner":"12345678-1234-4234-8234-123456789abc",
                   "fingerprint":"a"*64,"stopped":False,"roles":{"inspector":{
                "id":"32345678-1234-4234-8234-123456789abc","code":"000123","subject":subject,"expires_at":int(time.time())+30}}}
            (root/"state.json").write_text(json.dumps(state),encoding="utf-8")
            self.assertEqual(probe.issuer_identity(root,code,"a"*64),hashlib.sha256(subject.encode()).hexdigest())
            state["roles"]["inspector"]["pending"]={}
            (root/"state.json").write_text(json.dumps(state),encoding="utf-8")
            with self.assertRaises(RuntimeError): probe.issuer_identity(root,code,"a"*64)
            del state["roles"]["inspector"]["pending"]
            (root/"state.json").write_text(json.dumps(state),encoding="utf-8")
            with self.assertRaises(RuntimeError): probe.issuer_identity(root,code,"b"*64)
            code.write_text("000124",encoding="ascii")
            with self.assertRaises(RuntimeError): probe.issuer_identity(root,code,"a"*64)
            player=subject.replace("inspector-","player-")
            state["roles"]["player"]={"id":"42345678-1234-4234-8234-123456789abc","code":"000124","subject":player,"expires_at":int(time.time())+30}
            (root/"state.json").write_text(json.dumps(state),encoding="utf-8")
            self.assertEqual(probe.issuer_identity(root,code,"a"*64,"player"),hashlib.sha256(player.encode()).hexdigest())

    def test_issuer_lock_preserves_an_existing_owner(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            with probe.issuer_lock(root):
                self.assertEqual((root/"lock").read_text(),str(os.getpid()))
                with self.assertRaisesRegex(RuntimeError,"issuer state is locked"):
                    with probe.issuer_lock(root,timeout=0): pass
                self.assertEqual((root/"lock").read_text(),str(os.getpid()))
            self.assertFalse((root/"lock").exists())
            with self.assertRaisesRegex(RuntimeError,"fixture failure"):
                with probe.issuer_lock(root): raise RuntimeError("fixture failure")
            self.assertFalse((root/"lock").exists())

    @unittest.skipUnless(shutil.which("node"),"Node.js is required for the actual issuer lock")
    def test_python_issuer_lock_excludes_actual_node_admin_cli(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); issuer,_code,_state=self.make_issuer(root)
            command=[shutil.which("node"),str(probe.ADMIN_CLI),"local-status","--state-dir",str(issuer)]
            with probe.issuer_lock(issuer):
                blocked=subprocess.run(command,capture_output=True,timeout=15,creationflags=probe.CREATE_FLAGS)
                self.assertNotEqual(blocked.returncode,0)
                self.assertTrue((issuer/"lock").exists())
            available=subprocess.run(command,capture_output=True,timeout=15,creationflags=probe.CREATE_FLAGS)
            self.assertEqual(available.returncode,0)

    def test_packaged_login_records_only_the_snapshot_actually_published(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); issuer,code,issued=self.make_issuer(root); run=root/"run"; run.mkdir()
            state={"name":"fixture","server":"host:7777","role":"inspector","executable":str(root/"MRFPS.exe"),
                   "admin_dir":str(root/"admin"),"issuer_dir":str(issuer),"fingerprint":"a"*64,"checks":{},"actions":{}}
            def publish(args):
                self.assertEqual((issuer/"lock").read_text(),str(os.getpid()))
                self.assertEqual(args.expected_code,"000123")
                args.before_submit()
                probe.client.save_state(root/"Saved"/"DeviceAuth"/"windows-fixture.json",
                                        {"submitted":args.expected_marker,"submission_inflight":None})
                return {"status":"login_queued"}
            with mock.patch.object(probe,"ROOT",root), mock.patch.object(probe,"inspect",return_value={"checks":{"settings_applied":True}}), \
                 mock.patch.object(probe,"run_safe",return_value=""), mock.patch("shutil.which",return_value="node"), \
                 mock.patch.object(probe.client,"windows",side_effect=publish) as windows:
                self.assertEqual(probe.login(run,state)["status"],"login_queued")
                self.assertEqual(probe.login(run,state)["status"],"already_submitted")
                windows.assert_called_once()
            report=(run/"report.json").read_text(encoding="utf-8")
            self.assertNotIn("000123",report)
            self.assertNotIn(issued["roles"]["inspector"]["subject"],report)

    def test_changed_issuer_snapshot_is_rejected_before_submission(self):
        for changed in ("code","subject","id"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as folder:
                root=Path(folder); issuer,code,issued=self.make_issuer(root); run=root/"run"; run.mkdir()
                state={"name":"fixture","server":"host:7777","role":"inspector","executable":str(root/"MRFPS.exe"),
                       "admin_dir":str(root/"admin"),"issuer_dir":str(issuer),"fingerprint":"a"*64,"checks":{},"actions":{}}
                def rotate(args):
                    if changed=="code":
                        code.write_text("000456",encoding="ascii")
                        issued["roles"]["inspector"]["code"]="000456"
                    elif changed=="subject":
                        issued["roles"]["inspector"]["subject"]="inspector-22345678-1234-4234-8234-123456789abc"
                    else:
                        issued["roles"]["inspector"]["id"]="42345678-1234-4234-8234-123456789abc"
                    (issuer/"state.json").write_text(json.dumps(issued),encoding="utf-8")
                    args.before_submit()  # Real launcher runs this before writing its journal or control file.
                    self.fail("changed snapshot must not be submitted")
                with mock.patch.object(probe,"ROOT",root), mock.patch.object(probe,"inspect",return_value={"checks":{"settings_applied":True}}), \
                     mock.patch.object(probe,"run_safe",return_value=""), mock.patch("shutil.which",return_value="node"), \
                     mock.patch.object(probe.client,"windows",side_effect=rotate), \
                     mock.patch.object(probe.client,"windows_control") as control:
                    with self.assertRaisesRegex(RuntimeError,"issuer state changed"): probe.login(run,state)
                    control.assert_not_called()
                self.assertTrue(state["login_intent"])
                self.assertFalse(state.get("login_submitted",False))

    def test_mismatched_launcher_receipt_is_unknown_not_a_success(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); issuer,_code,_issued=self.make_issuer(root); run=root/"run"; run.mkdir()
            state={"name":"fixture","server":"host:7777","role":"inspector","executable":str(root/"MRFPS.exe"),
                   "admin_dir":str(root/"admin"),"issuer_dir":str(issuer),"fingerprint":"a"*64,"checks":{},"actions":{}}
            def publish_other(args):
                args.before_submit()
                probe.client.save_state(root/"Saved"/"DeviceAuth"/"windows-fixture.json",
                                        {"submitted":{"path":"different","modified_ns":1}})
                return {"status":"login_queued"}
            with mock.patch.object(probe,"ROOT",root), mock.patch.object(probe,"inspect",return_value={"checks":{"settings_applied":True}}), \
                 mock.patch.object(probe,"run_safe",return_value=""), mock.patch("shutil.which",return_value="node"), \
                 mock.patch.object(probe.client,"windows",side_effect=publish_other) as windows:
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(run,state)
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(run,state)
                windows.assert_called_once()  # A retry must not send a second code.
            self.assertFalse(state.get("login_submitted",False))

    def test_issuer_rotation_after_submission_is_unknown_and_not_retried(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); issuer,_code,issued=self.make_issuer(root); run=root/"run"; run.mkdir()
            state={"name":"fixture","server":"host:7777","role":"inspector","executable":str(root/"MRFPS.exe"),
                   "admin_dir":str(root/"admin"),"issuer_dir":str(issuer),"fingerprint":"a"*64,"checks":{},"actions":{}}
            def publish_then_rotate(args):
                args.before_submit()
                probe.client.save_state(root/"Saved"/"DeviceAuth"/"windows-fixture.json",
                                        {"submitted":args.expected_marker,"submission_inflight":None})
                issued["roles"]["inspector"]["subject"]="inspector-22345678-1234-4234-8234-123456789abc"
                (issuer/"state.json").write_text(json.dumps(issued),encoding="utf-8")
                return {"status":"login_queued"}
            with mock.patch.object(probe,"ROOT",root), mock.patch.object(probe,"inspect",return_value={"checks":{"settings_applied":True}}), \
                 mock.patch.object(probe,"run_safe",return_value=""), mock.patch("shutil.which",return_value="node"), \
                 mock.patch.object(probe.client,"windows",side_effect=publish_then_rotate) as windows:
                with self.assertRaisesRegex(RuntimeError,"after submission; outcome is unknown"): probe.login(run,state)
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(run,state)
                windows.assert_called_once()
            self.assertFalse(state.get("login_submitted",False))

    def test_interrupted_probe_with_matching_launcher_receipt_stays_unknown(self):
        with tempfile.TemporaryDirectory() as folder:
            marker={"path":"code","modified_ns":1}
            state={"name":"fixture","login_intent":True,"code_marker":marker,"checks":{},"actions":{}}
            with mock.patch.object(probe.client,"load_state",return_value={"submitted":marker}), \
                 mock.patch.object(probe.client,"windows") as windows:
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(Path(folder),state)
                windows.assert_not_called()

    def test_issuer_lock_tampering_cannot_be_recorded_as_completed(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); issuer,_code,_issued=self.make_issuer(root); run=root/"run"; run.mkdir()
            state={"name":"fixture","server":"host:7777","role":"inspector","executable":str(root/"MRFPS.exe"),
                   "admin_dir":str(root/"admin"),"issuer_dir":str(issuer),"fingerprint":"a"*64,"checks":{},"actions":{}}
            def publish_then_change_lock(args):
                args.before_submit()
                probe.client.save_state(root/"Saved"/"DeviceAuth"/"windows-fixture.json",
                                        {"submitted":args.expected_marker,"submission_inflight":None})
                (issuer/"lock").write_text("0",encoding="ascii")
                return {"status":"login_queued"}
            with mock.patch.object(probe,"ROOT",root), mock.patch.object(probe,"inspect",return_value={"checks":{"settings_applied":True}}), \
                 mock.patch.object(probe,"run_safe",return_value=""), mock.patch("shutil.which",return_value="node"), \
                 mock.patch.object(probe.client,"windows",side_effect=publish_then_change_lock) as windows:
                with self.assertRaisesRegex(RuntimeError,"issuer lock changed"): probe.login(run,state)
                with self.assertRaisesRegex(RuntimeError,"outcome is unknown"): probe.login(run,state)
                windows.assert_called_once()
            self.assertFalse(state.get("login_submitted",False))

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
