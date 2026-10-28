import contextlib
import io
import json
import pathlib
import socket
import struct
import sys
import tempfile
import threading
import types
import unittest
from unittest.mock import patch

import linux_security_probe as probe


def framed(data, shift):
    value = int.from_bytes(data, "little") << shift
    return value.to_bytes(len(data) + 1, "little")


class PacketTests(unittest.TestCase):
    def test_dtls_application_record_under_all_ue_bit_offsets(self):
        record = b"\x17\xfe\xfd\0\x01" + b"\0" * 6 + struct.pack(">H", 32) + b"x" * 32
        for shift in range(8):
            records = probe.dtls_records(b"hdr" + framed(record, shift))
            self.assertTrue(any(item[2] == 23 and item[3] == 1 and item[4] == 32 for item in records))

    def test_rejects_truncated_or_unencrypted_application_headers(self):
        for epoch, length, payload in ((0, 32, b"x"*32), (1, 200, b"x"*32), (1, 4, b"x"*4)):
            data = b"\x17\xfe\xfd" + struct.pack(">H", epoch) + b"\0"*6 + struct.pack(">H", length) + payload
            self.assertFalse(probe.dtls_records(data))

    def test_plaintext_detection_covers_bit_offsets_and_wide_strings(self):
        token = "eyJ" + "A"*40 + "." + "B"*50 + "." + "C"*90
        with probe.PacketProxy("127.0.0.1", 12345, token) as proxy:
            for shift in range(8):
                self.assertFalse(proxy._inspect(framed(token.encode("ascii"), shift)))
                self.assertFalse(proxy._inspect(framed(token.encode("utf-16-le"), shift)))
            self.assertEqual(proxy.stats["plaintext_exact_token_matches"], 16)
            self.assertFalse(proxy.stats["packet_payloads_persisted"])

    def test_proxy_forwards_actual_udp_without_payload_recording(self):
        upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        upstream.bind(("127.0.0.1", 0))
        upstream.settimeout(2)
        def echo():
            data, endpoint = upstream.recvfrom(100)
            upstream.sendto(data, endpoint)
        worker = threading.Thread(target=echo)
        worker.start()
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.settimeout(2)
        try:
            with probe.PacketProxy("127.0.0.1", upstream.getsockname()[1], "") as proxy:
                client.sendto(b"synthetic-fixture", ("127.0.0.1", proxy.port))
                self.assertEqual(client.recv(100), b"synthetic-fixture")
                self.assertEqual(proxy.stats["client_datagrams"], 1)
                self.assertEqual(proxy.stats["server_datagrams"], 1)
        finally:
            client.close(); upstream.close(); worker.join(2)

    def test_json_replace_failure_does_not_destroy_previous_report(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory)/"report.json"
            target.write_text("previous", encoding="utf-8")
            with patch.object(probe.os, "replace", side_effect=PermissionError()), patch.object(probe.time, "sleep"):
                with self.assertRaises(PermissionError):
                    probe.save_json(target, {"safe": True})
            self.assertEqual(target.read_text("utf-8"), "previous")
            self.assertEqual(list(target.parent.iterdir()), [target])


class IssuerPolicyTests(unittest.TestCase):
    def arguments(self, directory, *, encrypted=False):
        key = probe.Ed25519PrivateKey.generate()
        private = directory / "issuer-private.pem"
        public = directory / "issuer-public.pem"
        passphrase = directory / "passphrase.txt"
        password = b"isolated-unit-fixture-passphrase"
        serialization = probe.auth.serialization
        private.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.BestAvailableEncryption(password) if encrypted else serialization.NoEncryption()))
        public.write_bytes(probe.auth.public_pem(key.public_key()))
        passphrase.write_bytes(password)
        return types.SimpleNamespace(report_directory=str(directory/"report"), private_key=str(private), public_key=str(public),
                                     passphrase_file=str(passphrase) if encrypted else None, dev_unencrypted=False,
                                     legacy_protocol2=False, phase="lifecycle", server="127.0.0.1", port=1,
                                     fingerprint="a"*64, container="unit-only", packaged=False,
                                     executable="unlaunched-unit.exe", project="unopened-unit.uproject")

    def test_unencrypted_key_needs_explicit_fixture_opt_in_before_network(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(pathlib.Path(directory))
            with patch.object(probe.Probe, "docker", side_effect=AssertionError("No network expected")):
                with self.assertRaises(probe.auth.TokenError):
                    probe.Probe(args)

    def test_encrypted_key_passphrase_file_works_without_unencrypted_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(pathlib.Path(directory), encrypted=True)
            state={"running": True, "health": "healthy"}
            with patch.object(probe.Probe, "state", return_value=state), patch.object(probe.Probe, "server_logs", return_value="MRFPS_AUTH_TRANSPORT_READY fingerprint="+"a"*64):
                instance = probe.Probe(args)
            self.assertEqual(instance.report["status"], "prepared")
            self.assertFalse(instance.report["disabled_plugins"])


class LaunchModeTests(unittest.TestCase):
    def arguments(self, directory, packaged):
        return types.SimpleNamespace(executable=str(directory/"MRFPS.exe"), project=str(directory/"MRFPS.uproject"),
                                     packaged=packaged, legacy_protocol2=False)

    def test_packaged_command_uses_inner_game_and_file_credentials_only(self):
        with tempfile.TemporaryDirectory() as value:
            directory=pathlib.Path(value)
            inner=directory/"MRFPS/Binaries/Win64/MRFPS.exe"
            inner.parent.mkdir(parents=True);inner.write_bytes(b"unlaunched synthetic executable")
            token=directory/"private-fixture.jwt"
            raw=b"SYNTHETIC_CREDENTIAL_MUST_NEVER_ENTER_ARGV"
            token.write_bytes(raw)
            args=self.arguments(directory,True)
            command=probe.client_command(args,token,directory/"issuer-public.pem","inspector","mrfps",12345,directory/"client.log")
            self.assertEqual(command[0],str(inner.resolve()))
            self.assertNotIn(args.project,command)
            self.assertNotIn("-game",command)
            self.assertIn("-MRFPSInspector",command)
            self.assertIn("-MRFPSAuthTokenFile="+str(token),command)
            self.assertNotIn(raw.decode()," ".join(command))
            self.assertFalse(any(item.startswith("-DisablePlugins") for item in command))

    def test_editor_mode_keeps_project_and_game_without_inspector_role(self):
        with tempfile.TemporaryDirectory() as value:
            directory=pathlib.Path(value)
            args=self.arguments(directory,False)
            command=probe.client_command(args,directory/"access.jwt",directory/"issuer-public.pem","player","mrfps",12345,directory/"client.log")
            self.assertEqual(command[1:3],[str(pathlib.Path(args.project).resolve()),"-game"])
            self.assertNotIn("-MRFPSInspector",command)

    def test_packaged_cannot_omit_legacy_visualization_plugin(self):
        with tempfile.TemporaryDirectory() as value:
            directory=pathlib.Path(value)
            args=self.arguments(directory,True);args.legacy_protocol2=True
            with self.assertRaisesRegex(RuntimeError,"LEGACY_PROTOCOL2_REQUIRES_EDITOR_MODE"):
                probe.client_command(args,directory/"access.jwt",directory/"public.pem","player","mrfps",1,directory/"client.log")

    def test_full_packaged_plan_never_initializes_signer_docker_or_ue(self):
        with tempfile.TemporaryDirectory() as value:
            directory=pathlib.Path(value)/"plan"
            argv=["probe", "--executable", "absent/MRFPS.exe", "--project", "absent/project.uproject",
                  "--private-key", "absent/private.pem", "--public-key", "absent/public.pem", "--fingerprint", "a"*64,
                  "--report-directory", str(directory), "--phase", "all", "--packaged", "--dry-run"]
            with patch.object(sys,"argv",argv),patch.object(probe,"Probe",side_effect=AssertionError("NO_RUNTIME")),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main(),0)
            result=json.loads((directory/"plan.json").read_text())
            self.assertEqual(result["expected_case_count"],33)
            self.assertEqual(result["maximum_simultaneous_ue_clients"],1)
            for field in ("issuer_key_read","docker_commands_run","ue_processes_started","passed","local_rejection_is_server_evidence","native_crypto_disabled"):
                self.assertFalse(result[field])


if __name__ == "__main__":
    unittest.main()
