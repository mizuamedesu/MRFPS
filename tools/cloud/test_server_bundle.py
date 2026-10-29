import base64
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import subprocess
import unittest
from unittest.mock import patch

import server_bundle as cloud


class CloudTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / "package"
        server = self.package / cloud.SERVER
        server.parent.mkdir(parents=True)
        header = bytearray(64)
        header[:6] = b"\x7fELF\x02\x01"
        header[18:20] = (62).to_bytes(2, "little")
        server.write_bytes(header + b"non-executable test fixture")
        paks = self.package / "MRFPS/Content/Paks"
        paks.mkdir(parents=True)
        (paks / "fixture.pak").write_bytes(b"fixture")
        self.server_hash = cloud.sha256(server)
        self.commit = "a" * 40

    def pack(self, directory="bundle"):
        return cloud.bundle(self.package, self.server_hash, self.commit, self.root / directory)

    def archive(self, members):
        path = self.root / "malformed.tar.gz"
        with tarfile.open(path, "w:gz") as tar:
            for name, kind, payload in members:
                info = tarfile.TarInfo(name)
                info.type = kind
                if kind == tarfile.SYMTYPE:
                    info.linkname = "../../outside"
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        return path

    def test_archive_is_deterministic_and_symbols_stay_on_host(self):
        binary = self.package / cloud.SERVER
        binary.with_suffix(".debug").write_bytes(b"host debug")
        binary.with_suffix(".sym").write_bytes(b"host symbols")
        saved = self.package / "MRFPS/Saved"
        saved.mkdir()
        (saved / "access.jwt").write_text("not a credential, still must not ship")
        first, second = self.pack("one"), self.pack("two")
        self.assertEqual(first["bundle_sha256"], second["bundle_sha256"])
        with tarfile.open(self.root / "one/linux-server.tar.gz") as archive:
            self.assertEqual(len(archive.getnames()), 2)
        self.assertTrue(binary.with_suffix(".debug").exists())
        self.assertTrue(binary.with_suffix(".sym").exists())

    def test_roundtrip_verifies_approved_executable_and_archive(self):
        manifest = self.pack()
        cloud.verify_archive(self.root / "bundle/linux-server.tar.gz", manifest["bundle_sha256"], self.server_hash, self.root / "extract")
        self.assertEqual(cloud.sha256(self.root / "extract" / cloud.PREFIX / cloud.SERVER), self.server_hash)

    def test_wrong_approved_hash_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "APPROVED_BUILD"):
            cloud.bundle(self.package, "0" * 64, self.commit, self.root / "output")
        self.assertFalse((self.root / "output").exists())

    @unittest.skipUnless(os.name == "nt", "actual PowerShell publication gate")
    def test_publisher_rejects_different_source_commit_without_network(self):
        self.pack()
        script = Path(__file__).resolve().parents[2] / "Scripts/Cloud/Publish-GhcrServer.ps1"
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(script),
            "-BundleDirectory", str(self.root / "bundle"), "-ExpectedServerSha256", self.server_hash,
            "-SourceCommit", "b" * 40, "-Mode", "Plan"], capture_output=True, text=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("BUNDLE_SOURCE_COMMIT_MISMATCH", result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt", "actual PowerShell publication plan")
    def test_matching_publisher_plan_preserves_private_visibility(self):
        self.pack()
        script = Path(__file__).resolve().parents[2] / "Scripts/Cloud/Publish-GhcrServer.ps1"
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", str(script),
            "-BundleDirectory", str(self.root / "bundle"), "-ExpectedServerSha256", self.server_hash,
            "-SourceCommit", self.commit, "-Mode", "Plan"], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        self.assertTrue(plan["bundleSourceCommitMatched"])
        self.assertFalse(plan["packageVisibilityChange"])
        self.assertFalse(plan["compilationFreshnessProvenByHash"])

    def test_credential_file_rejected_before_archive_creation(self):
        (self.package / "issuer-private.pem").write_text("fixture")
        with self.assertRaisesRegex(ValueError, "CREDENTIAL_FILE"):
            self.pack()
        self.assertFalse((self.root / "bundle/linux-server.tar.gz").exists())

    def test_archive_digest_mismatch_is_rejected_before_extraction(self):
        self.pack()
        with self.assertRaisesRegex(ValueError, "BUNDLE_SHA256"):
            cloud.verify_archive(self.root / "bundle/linux-server.tar.gz", "0" * 64, self.server_hash, self.root / "extract")
        self.assertFalse((self.root / "extract").exists())

    def test_traversal_links_aliases_and_other_context_paths_rejected(self):
        for name, kind in [
            (cloud.PREFIX + "../../escape", tarfile.REGTYPE),
            (cloud.PREFIX + "MRFPS//duplicate", tarfile.REGTYPE),
            (cloud.PREFIX + "link", tarfile.SYMTYPE),
            ("Deploy/Linux/entrypoint.sh", tarfile.REGTYPE),
            (cloud.PREFIX + "access.jwt", tarfile.REGTYPE),
        ]:
            with self.subTest(name=name):
                archive = self.archive([(name, kind, b"fixture")])
                with self.assertRaises(ValueError):
                    cloud.verify_archive(archive, cloud.sha256(archive), self.server_hash, self.root / "extract")
                self.assertFalse((self.root / "extract").exists())

    def test_member_count_is_bounded(self):
        archive = self.archive([(cloud.PREFIX + "one", tarfile.REGTYPE, b"a"), (cloud.PREFIX + "two", tarfile.REGTYPE, b"b")])
        with patch.object(cloud, "MAX_FILES", 1), self.assertRaisesRegex(ValueError, "SIZE_LIMIT"):
            cloud.verify_archive(archive, cloud.sha256(archive), self.server_hash, self.root / "extract")

    def test_duplicate_paths_are_rejected(self):
        archive = self.archive([(cloud.PREFIX + "same", tarfile.REGTYPE, b"a")] * 2)
        with self.assertRaisesRegex(ValueError, "ENTRY_INVALID"):
            cloud.verify_archive(archive, cloud.sha256(archive), self.server_hash, self.root / "extract")

    def test_foreign_architecture_is_rejected(self):
        server = self.package / cloud.SERVER
        value = bytearray(server.read_bytes())
        value[18:20] = (183).to_bytes(2, "little")
        server.write_bytes(value)
        with self.assertRaisesRegex(ValueError, "AMD64_ELF"):
            cloud.bundle(self.package, cloud.sha256(server), self.commit, self.root / "output")

    @staticmethod
    def public_fixture():
        der = bytes.fromhex("302a300506032b6570032100") + bytes([17]) * 32
        return b"-----BEGIN PUBLIC KEY-----\n" + base64.b64encode(der) + b"\n-----END PUBLIC KEY-----\n"

    def manifest(self, key=None, image=None):
        return cloud.flash_manifest(image or "ghcr.io/mizuamedesu/mrfps-server@sha256:" + "a" * 64, key or self.public_fixture(), "00000000-0000-4000-8000-000000000001", "mrfps-server", "heteronet-global")

    def test_flash_has_public_key_only_one_replica_and_bounded_resources(self):
        manifest = self.manifest()
        spec = manifest["spec"]
        self.assertEqual(set(spec["env"]), {"MRFPS_PORT", "MRFPS_AUTH_PUBLIC_KEY_B64", "MRFPS_AUTH_AUDIENCE"})
        self.assertEqual(base64.b64decode(spec["env"]["MRFPS_AUTH_PUBLIC_KEY_B64"]), self.public_fixture())
        self.assertEqual((spec["replicas"], spec["cpu_millis"], spec["memory_mib"]), (1, 2000, 1024))
        self.assertEqual(spec["ports"][0]["container_port"], 7777)
        self.assertNotIn("service_port", spec["ports"][0])
        self.assertNotIn("PRIVATE", json.dumps(manifest))

    def test_private_or_bom_key_and_mutable_image_rejected(self):
        for key in [b"-----BEGIN PRIVATE KEY-----\nfixture", b"\xef\xbb\xbf" + self.public_fixture()]:
            with self.assertRaises(ValueError):
                self.manifest(key=key)
        with self.assertRaisesRegex(ValueError, "IMMUTABLE"):
            self.manifest(image="ghcr.io/mizuamedesu/mrfps-server:latest")


if __name__ == "__main__":
    unittest.main()
