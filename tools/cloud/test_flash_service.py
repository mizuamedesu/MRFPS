import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import flash_service as flash
import test_server_bundle as bundle_tests
from server_bundle import flash_manifest


class FakeCLI:
    def __init__(self):
        self.items = []
        self.mutations = []

    def call(self, *arguments, payload=None):
        if arguments[:2] == ("flash", "list"):
            # This is the CLI's output, not the API collection envelope.
            return copy.deepcopy(self.items)
        self.mutations.append(arguments[:2])
        if arguments[:2] == ("flash", "create"):
            value = copy.deepcopy(payload)
            value.update(id="00000000-0000-4000-8000-000000000009", state="running", generation=1, status={"status": {"ready_replicas": 1}})
            value["spec"]["ports"][0]["service_port"] = 31000
            self.items.append(value)
        elif arguments[:2] == ("flash", "update"):
            self.items[0]["spec"] = copy.deepcopy(payload["spec"])
            self.items[0]["spec"]["ports"][0]["service_port"] = 31000
            self.items[0]["generation"] += 1
        else:
            raise AssertionError("unexpected command")
        return {}


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.state = Path(self.temporary.name) / "state.json"
        self.manifest = flash_manifest("ghcr.io/mizuamedesu/mrfps-server-private@sha256:" + "a" * 64,
            bundle_tests.CloudTests.public_fixture(), "00000000-0000-4000-8000-000000000001", "mrfps-server", "heteronet-global")
        self.client = FakeCLI()

    def apply(self):
        return flash.reconcile(self.client, self.manifest, self.state, validation_allocated_port=True, operator_private_pull_ready=True)

    def test_fixed_port_and_missing_private_pull_evidence_stop_before_any_cli(self):
        with self.assertRaisesRegex(ValueError, "UDP7777_UNSUPPORTED"):
            flash.reconcile(self.client, self.manifest, self.state)
        with self.assertRaisesRegex(ValueError, "PULL_SECRET_REQUIRED"):
            flash.reconcile(self.client, self.manifest, self.state, validation_allocated_port=True)
        self.assertEqual(self.client.mutations, [])
        self.assertFalse(self.state.exists())

    def test_create_then_repeat_is_unchanged_and_does_not_claim_7777_or_game_health(self):
        first = self.apply()
        second = self.apply()
        self.assertEqual(first["operation"], "create-accepted")
        self.assertEqual(second["operation"], "unchanged")
        self.assertEqual(self.client.mutations, [("flash", "create")])
        self.assertEqual(second["assigned_udp_ports"], [31000])
        for field in ("fixed_public_udp7777_satisfied", "game_health_verified", "internet_udp_verified", "runtime_fingerprint_verified", "runtime_private_pull_verified"):
            self.assertFalse(second[field])

    def test_another_service_with_the_same_name_is_never_adopted(self):
        item = copy.deepcopy(self.manifest)
        item["id"] = "00000000-0000-4000-8000-000000000009"
        self.client.items.append(item)
        with self.assertRaisesRegex(ValueError, "COLLISION_OR_OWNERSHIP"):
            self.apply()
        self.assertEqual(self.client.mutations, [])
        self.assertFalse(self.state.exists())

    def test_unverified_or_paged_collection_never_creates_a_service(self):
        original = self.client.call
        for result in ({"items": []}, {"items": [], "next_page": "another-page"}, None):
            with self.subTest(result=result):
                def call(*arguments, **kwargs):
                    if arguments[:2] == ("flash", "list"):
                        return result
                    return original(*arguments, **kwargs)
                self.client.call = call
                with self.assertRaisesRegex(ValueError, "LIST_RESPONSE_SHAPE_UNVERIFIED"):
                    self.apply()
                self.assertEqual(self.client.mutations, [])
                self.assertFalse(self.state.exists())

    def test_incomplete_service_record_never_allows_create(self):
        self.client.items.append({"id": "00000000-0000-4000-8000-000000000009"})
        with self.assertRaisesRegex(ValueError, "SERVICE_NAME_RESPONSE_UNVERIFIED"):
            self.apply()
        self.assertEqual(self.client.mutations, [])
        self.assertFalse(self.state.exists())

    def test_duplicate_matching_names_never_allow_update(self):
        self.apply()
        self.client.items.append(copy.deepcopy(self.client.items[0]))
        with self.assertRaisesRegex(ValueError, "SERVICE_NAME_AMBIGUOUS"):
            self.apply()
        self.assertEqual(self.client.mutations, [("flash", "create")])

    def test_only_owned_service_updates_to_new_immutable_image(self):
        self.apply()
        self.manifest["spec"]["image"] = self.manifest["spec"]["image"].replace("a" * 64, "b" * 64)
        self.assertEqual(self.apply()["operation"], "update-accepted")
        self.assertEqual(self.apply()["operation"], "unchanged")
        self.assertEqual(self.client.mutations, [("flash", "create"), ("flash", "update")])

    def test_server_side_spec_drift_or_owner_marker_change_stops_updates(self):
        self.apply()
        self.client.items[0]["spec"]["memory_mib"] = 4096
        with self.assertRaisesRegex(ValueError, "SPEC_DRIFT"):
            self.apply()
        self.assertEqual(len(self.client.mutations), 1)
        self.client.items[0]["spec"]["metadata"]["deployment_id"] = "other"
        with self.assertRaisesRegex(ValueError, "OWNERSHIP_MISMATCH"):
            self.apply()

    def test_created_request_timeout_can_be_reconciled_without_duplicate_create(self):
        original = self.client.call
        failed_once = False
        def call(*arguments, **kwargs):
            nonlocal failed_once
            value = original(*arguments, **kwargs)
            if arguments[:2] == ("flash", "create") and not failed_once:
                failed_once = True
                raise TimeoutError("fixture transport timeout after server acceptance")
            return value
        self.client.call = call
        with self.assertRaises(TimeoutError):
            self.apply()
        self.assertTrue(json.loads(self.state.read_text())["deployment_id"])
        self.assertEqual(self.apply()["operation"], "unchanged")
        self.assertEqual(self.client.mutations, [("flash", "create")])


class InstalledHeteroCloudCliTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("heterocloud"), "HeteroCloud CLI is not installed")
    def test_list_unwraps_api_envelope_and_sends_project_filter(self):
        # Real CLI, synthetic local HTTP endpoint and fake credentials only.
        organization = "00000000-0000-4000-8000-000000000001"
        project = "00000000-0000-4000-8000-000000000002"
        service = {
            "id": "00000000-0000-4000-8000-000000000003",
            "organization_id": organization, "project_id": project, "provider": "flash",
            "name": "fixture-service", "generation": 1, "state": "ready", "spec": {}, "status": {},
            "created_at": "2026-10-30T00:00:00Z", "updated_at": "2026-10-30T00:00:00Z",
        }
        paths = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                paths.append(self.path)
                data = json.dumps({"items": [service]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        env = os.environ.copy()
        env["HETEROCLOUD_API_KEY"] = "hc_fixture_only"
        env.pop("HETEROCLOUD_API_KEY_FILE", None)
        try:
            result = subprocess.run([
                shutil.which("heterocloud"), "--endpoint", f"http://127.0.0.1:{server.server_port}",
                "--allow-insecure-http", "--organization-id", organization, "--output", "json",
                "flash", "list", "--project-id", project,
            ], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, "fixture CLI invocation failed")
            self.assertEqual(json.loads(result.stdout), [service])
            self.assertEqual(paths, [f"/api/v1/organizations/{organization}/flash/services?project_id={project}"])
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
