import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from flash_service import digest
from update_owned_flash import cloudflare_cidrs, update

PROJECT = "019fbdaa-c85e-7950-b0f1-356e78da4b6e"
SERVICE = "01a18a87-c459-7563-bc3b-397306404251"
DEPLOYMENT = "fc005ce6-d957-4d6e-90b9-8935ff3f1749"
IMAGE = "ghcr.io/example/server@sha256:" + "a" * 64
KEY = "b" * 64
CIDRS = ["104.16.0.0/13", "2606:4700::/32"]


class FakeFlash:
    def __init__(self):
        self.service = {"id": SERVICE, "project_id": PROJECT, "name": "mrfps-server", "generation": 4,
                        "spec": {"image": IMAGE, "env": {"EXISTING": "keep"},
                                 "exposure": {"type": "public", "traffic_mode": "direct"},
                                 "egress": {"mode": "disabled", "allowed_destination_cidrs": [], "denied_destination_cidrs": []},
                                 "metadata": {"managed_by": "MRFPS-cloud-deploy", "service_name": "mrfps-server", "deployment_id": DEPLOYMENT},
                                 "ports": [{"container_port": 7777, "protocol": "udp", "service_port": 30000}]}}
        self.updates = 0
        self.ambiguous = False

    def call(self, *args, payload=None):
        if args[:2] == ("flash", "list"):
            return [copy.deepcopy(self.service)]
        if args[:2] == ("flash", "update"):
            self.updates += 1
            self.service["spec"] = copy.deepcopy(payload["spec"])
            self.service["generation"] += 1
            if self.ambiguous:
                raise ValueError("HETEROCLOUD_COMMAND_FAILED_AUTH_OR_OPERATION_REQUIRED")
            return {}
        raise AssertionError(args)


class OwnedFlashUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "state.json"
        self.client = FakeFlash()
        self.state = {"project_id": PROJECT, "service_name": "mrfps-server", "deployment_id": DEPLOYMENT,
                      "service_id": SERVICE, "observed_spec_sha256": digest(self.client.service["spec"])}
        self.path.write_text(json.dumps(self.state), encoding="utf-8")

    def test_updates_only_owned_direct_service_and_is_idempotent(self):
        first = update(self.client, self.path, IMAGE, KEY, CIDRS)
        self.assertEqual(first["operation"], "update-accepted")
        self.assertEqual(self.client.service["spec"]["env"], {"EXISTING": "keep", "MRFPS_ROSTER_HMAC_KEY": KEY})
        self.assertEqual(self.client.service["spec"]["exposure"]["traffic_mode"], "direct")
        self.assertEqual(self.client.service["spec"]["egress"]["mode"], "restricted")
        self.assertEqual(self.client.service["spec"]["egress"]["allowed_destination_cidrs"], CIDRS)
        self.assertEqual(self.client.service["spec"]["ports"][0]["service_port"], 30000)
        self.assertEqual(update(self.client, self.path, IMAGE, KEY, CIDRS)["operation"], "unchanged")
        self.assertEqual(self.client.updates, 1)
        self.assertNotIn(KEY, self.path.read_text(encoding="utf-8"))

    def test_ambiguous_response_is_reconciled_without_repeating_update(self):
        self.client.ambiguous = True
        with self.assertRaisesRegex(ValueError, "HETEROCLOUD_COMMAND_FAILED"):
            update(self.client, self.path, IMAGE, KEY, CIDRS)
        self.assertEqual(update(self.client, self.path, IMAGE, KEY, CIDRS)["operation"], "pending-reconciled")
        self.assertEqual(self.client.updates, 1)

    def test_existing_drift_or_non_direct_mode_fails_closed(self):
        self.client.service["spec"]["env"]["OTHER"] = "changed"
        with self.assertRaisesRegex(ValueError, "SPEC_DRIFT"):
            update(self.client, self.path, IMAGE, KEY, CIDRS)
        self.client.service["spec"]["exposure"]["traffic_mode"] = "forward"
        with self.assertRaisesRegex(ValueError, "DIRECT_PUBLIC"):
            update(self.client, self.path, IMAGE, KEY, CIDRS)
        self.assertEqual(self.client.updates, 0)

    def test_rejects_unowned_service_and_invalid_inputs(self):
        self.client.service["spec"]["metadata"]["deployment_id"] = "different"
        with self.assertRaisesRegex(ValueError, "OWNERSHIP_MISMATCH"):
            update(self.client, self.path, IMAGE, KEY, CIDRS)
        with self.assertRaisesRegex(ValueError, "IMMUTABLE_IMAGE"):
            update(self.client, self.path, "ghcr.io/example/server:latest", KEY, CIDRS)
        self.assertEqual(self.client.updates, 0)

    def test_existing_explicit_deny_is_not_bypassed(self):
        self.client.service["spec"]["egress"]["denied_destination_cidrs"] = ["104.16.0.0/13"]
        self.state["observed_spec_sha256"] = digest(self.client.service["spec"])
        self.path.write_text(json.dumps(self.state), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "EGRESS_DENIED"):
            update(self.client, self.path, IMAGE, KEY, CIDRS)
        self.assertEqual(self.client.updates, 0)

    def test_official_ranges_must_cover_admin_dns(self):
        class Response:
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self, amount):
                return json.dumps({"success": True, "result": {"ipv4_cidrs": [CIDRS[0]], "ipv6_cidrs": [CIDRS[1]]}}).encode()
        with patch("update_owned_flash.urlopen", return_value=Response()), patch("update_owned_flash.socket.getaddrinfo", return_value=[(2, 1, 6, "", ("104.16.1.2", 443))]):
            self.assertEqual(cloudflare_cidrs(), CIDRS)
        with patch("update_owned_flash.urlopen", return_value=Response()), patch("update_owned_flash.socket.getaddrinfo", return_value=[(2, 1, 6, "", ("203.0.113.10", 443))]):
            with self.assertRaisesRegex(ValueError, "OUTSIDE_CLOUDFLARE"):
                cloudflare_cidrs()


if __name__ == "__main__":
    unittest.main()
