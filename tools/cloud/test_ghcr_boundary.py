import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import urllib.request
from unittest.mock import patch

import ghcr_boundary as boundary

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = "mizuamedesu/MRFPS-Server-Registry"
PACKAGE = "mrfps-server-private"
DIGEST = "sha256:" + "a" * 64
LAYER = "sha256:" + "b" * 64
CONFIG = {"config": {"Labels": {"org.opencontainers.image.source": "https://github.com/" + REPOSITORY, "app.mrfps.privacy-fixture": "nonsecret-v1"}}}
CONFIG_DIGEST = "sha256:" + hashlib.sha256(json.dumps(CONFIG).encode()).hexdigest()


def response(value, status=200, headers=None):
    return status, json.dumps(value).encode(), headers or {}


class BoundaryTests(unittest.TestCase):
    def metadata(self, private=True, repository=REPOSITORY):
        return {"id": 123, "created_at": "2026-10-29T18:05:08Z", "name": PACKAGE, "package_type": "container", "owner": {"login": "mizuamedesu"},
                "repository": {"full_name": repository}, "visibility": "private" if private else "public"}

    def replies(self, layer_status=401):
        return [response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response(self.metadata()),
                response({"token": "fixture-auth-token"}),
                response({"config": {"digest": CONFIG_DIGEST}, "layers": [{"digest": LAYER, "size": 1024}]}, headers={"docker-content-digest": DIGEST}),
                response(CONFIG),
                response({}, 403), response({}, 401), response({}, 401), response({}, layer_status)]

    def test_private_metadata_authenticated_manifest_and_all_anonymous_denials(self):
        with patch.object(boundary, "request", side_effect=self.replies()) as request:
            report = boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture-secret", "owner")
        self.assertTrue(report["anonymous_denied"])
        self.assertEqual(report["digest"], DIGEST)
        self.assertNotIn("fixture-secret", json.dumps(report))
        self.assertNotIn("fixture-auth-token", json.dumps(report))
        self.assertTrue(request.call_args.kwargs["one_byte"])

    def test_public_repository_refused_before_package_or_registry_access(self):
        with patch.object(boundary, "request", return_value=response({"full_name": REPOSITORY, "private": False})) as request:
            with self.assertRaisesRegex(ValueError, "PRIVATE_PUBLISHER_REQUIRED"):
                boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", allow_absent=True)
        self.assertEqual(request.call_count, 1)

    def test_public_or_other_repository_package_refused_before_registry_access(self):
        for metadata in (self.metadata(private=False), self.metadata(repository="mizuamedesu/MRFPS")):
            with self.subTest(metadata=metadata), patch.object(boundary, "request", side_effect=[response({"full_name": REPOSITORY, "private": True}), response(metadata)]) as request:
                with self.assertRaisesRegex(ValueError, "OWNERSHIP_OR_VISIBILITY"):
                    boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture", "owner")
                self.assertEqual(request.call_count, 2)

    def test_only_not_found_can_be_accepted_for_first_harmless_fixture(self):
        with patch.object(boundary, "request", side_effect=[response({"full_name": REPOSITORY, "private": True}), response({}, 404)]):
            self.assertTrue(boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", allow_absent=True)["package_absent"])
        with patch.object(boundary, "request", side_effect=[response({"full_name": REPOSITORY, "private": True}), response({}, 403)]):
            with self.assertRaisesRegex(ValueError, "HTTP_403"):
                boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", allow_absent=True)
        with patch.object(boundary, "request", side_effect=[response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response({}, 404)]):
            with self.assertRaisesRegex(ValueError, "PINNED_PRIVATE_PACKAGE_DISAPPEARED"):
                boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", allow_absent=True, identity=self.pin())

    def test_layer_anonymous_access_rejected_even_if_manifests_denied(self):
        for status in (200, 206, 302, 500):
            with self.subTest(status=status), patch.object(boundary, "request", side_effect=self.replies(status)):
                with self.assertRaisesRegex(ValueError, "ANONYMOUS_REGISTRY_ACCESS_NOT_DENIED"):
                    boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture", "owner")

    def test_anonymous_tag_or_digest_access_rejected(self):
        for position in (6, 7):
            replies = self.replies()
            replies[position] = response({}, 200)
            with self.subTest(position=position), patch.object(boundary, "request", side_effect=replies):
                with self.assertRaisesRegex(ValueError, "ANONYMOUS_REGISTRY_ACCESS_NOT_DENIED"):
                    boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture", "owner")

    def pin(self):
        return {"repository": REPOSITORY, "publisher_repository_id": 1438751425, "package": PACKAGE, "package_id": 123,
                "created_at": "2026-10-29T18:05:08Z", "creation_workflow_run_id": 40927353040,
                "creation_preflight_absent": True, "fixture_digest": "sha256:0f94eeae41fcbe2acc2654022fddf033198e116d274f53150ffea6c9437d2993"}

    def test_missing_repository_link_requires_exact_package_creation_pin(self):
        metadata = self.metadata()
        metadata.pop("repository")
        replies = [response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response(metadata)]
        with patch.object(boundary, "request", side_effect=replies), self.assertRaisesRegex(ValueError, "OWNERSHIP_OR_VISIBILITY"):
            boundary.check_private_package(REPOSITORY, PACKAGE, "fixture")
        with patch.object(boundary, "request", side_effect=replies):
            report = boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", identity=self.pin())
            self.assertTrue(report["package_id_pinned"])
            self.assertFalse(report["repository_link_present"])

    def test_pin_cannot_accept_another_id_created_at_source_or_publisher(self):
        for field, value in (("package_id", 124), ("publisher_repository_id", 99), ("created_at", "other"),
                             ("creation_workflow_run_id", 1), ("creation_preflight_absent", False), ("fixture_digest", DIGEST)):
            pin = self.pin()
            pin[field] = value
            replies = [response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response(self.metadata())]
            with self.subTest(field=field), patch.object(boundary, "request", side_effect=replies), self.assertRaisesRegex(ValueError, "ID_PIN_MISMATCH"):
                boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", identity=pin)

    def test_valid_pin_still_refuses_an_explicit_different_repository(self):
        replies = [response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response(self.metadata(repository="mizuamedesu/MRFPS"))]
        with patch.object(boundary, "request", side_effect=replies), self.assertRaisesRegex(ValueError, "OWNERSHIP_OR_VISIBILITY"):
            boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", identity=self.pin())

    def test_registry_image_config_must_have_private_source_and_matching_digest(self):
        replies = self.replies()
        replies[4] = response({"config": {"Labels": {"org.opencontainers.image.source": "https://github.com/mizuamedesu/MRFPS"}}})
        with patch.object(boundary, "request", side_effect=replies), self.assertRaisesRegex(ValueError, "CONFIG_DIGEST_MISMATCH"):
            boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture", "owner")
        public_config = {"config": {"Labels": {"org.opencontainers.image.source": "https://github.com/mizuamedesu/MRFPS"}}}
        replies[3] = response({"config": {"digest": "sha256:" + hashlib.sha256(json.dumps(public_config).encode()).hexdigest()}, "layers": [{"digest": LAYER}]}, headers={"docker-content-digest": DIGEST})
        with patch.object(boundary, "request", side_effect=replies), self.assertRaisesRegex(ValueError, "SOURCE_LABEL_MISMATCH"):
            boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture", "owner")

    def test_inspection_does_not_claim_identity_was_verified(self):
        metadata = self.metadata()
        metadata.pop("repository")
        metadata["version_count"] = 1
        fixture = {"id": 321, "name": self.pin()["fixture_digest"], "metadata": {"container": {"tags": ["privacy-fixture-v1"]}}}
        replies = [response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response(metadata), response([fixture])]
        with patch.object(boundary, "request", side_effect=replies):
            report = boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", inspect_only=True)
        self.assertEqual(report["package_id"], 123)
        self.assertFalse(report["identity_verified"])

    def test_initial_inspection_cannot_adopt_another_or_extra_version(self):
        metadata = self.metadata()
        metadata["version_count"] = 1
        for versions in ([], [{"id": 321, "name": DIGEST}], [{"id": 321}, {"id": 322}]):
            replies = [response({"id": 1438751425, "full_name": REPOSITORY, "private": True}), response(metadata), response(versions)]
            with self.subTest(versions=versions), patch.object(boundary, "request", side_effect=replies), self.assertRaisesRegex(ValueError, "VERSION_HISTORY_MISMATCH"):
                boundary.check_private_package(REPOSITORY, PACKAGE, "fixture", inspect_only=True)

    def test_manifest_digest_header_must_be_present_and_canonical(self):
        replies = self.replies()
        replies[3] = response({"layers": [{"digest": LAYER}]})
        with patch.object(boundary, "request", side_effect=replies):
            with self.assertRaisesRegex(ValueError, "MANIFEST_DIGEST_INVALID"):
                boundary.verify(REPOSITORY, PACKAGE, "fixture", "fixture", "owner")

    def test_cross_origin_redirect_does_not_forward_authorization(self):
        request = urllib.request.Request("https://ghcr.io/v2/fixture/blob", headers={"Authorization": "Bearer fixture"})
        redirected = boundary.StripCrossOriginAuthorization().redirect_request(request, None, 302, "Found", {}, "https://cdn.example/fixture")
        self.assertIsNone(redirected.get_header("Authorization"))

    def test_same_origin_redirect_preserves_authorization(self):
        request = urllib.request.Request("https://ghcr.io/v2/fixture/blob", headers={"Authorization": "Bearer fixture"})
        redirected = boundary.StripCrossOriginAuthorization().redirect_request(request, None, 302, "Found", {}, "https://ghcr.io/v2/fixture/new")
        self.assertEqual(redirected.get_header("Authorization"), "Bearer fixture")

    def test_workflows_guard_before_any_checkout_or_registry_step(self):
        bash = shutil.which("bash") or ("C:/Program Files/Git/bin/bash.exe" if os.name == "nt" else None)
        if not bash:
            self.skipTest("bash unavailable for executing the real guard")
        for path in (ROOT / ".github/workflows/publish-linux-server.yml", ROOT / "Deploy/Cloud/private-publisher.yml"):
            source = path.read_text(encoding="utf-8")
            body = source.split("        run: |\n", 1)[1].split("      - ", 1)[0]
            script = "\n".join(line[10:] if line.startswith("          ") else line for line in body.splitlines())
            environment = os.environ.copy()
            environment.update(PUBLISHER_REPOSITORY_PRIVATE="false", GITHUB_REPOSITORY="mizuamedesu/MRFPS")
            with self.subTest(workflow=path.name):
                result = subprocess.run([bash, "--noprofile", "--norc", "-c", script], env=environment, capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertLess(source.index('"$PUBLISHER_REPOSITORY_PRIVATE"'), source.index("actions/checkout@"))


if __name__ == "__main__":
    unittest.main()
