import copy
import json
from pathlib import Path
import tempfile
import unittest

import flash_service as flash
import test_server_bundle as bundle_tests
from server_bundle import flash_manifest


class FakeCLI:
    def __init__(self):
        self.items = []
        self.mutations = []

    def call(self, *arguments, payload=None):
        if arguments[:2] == ("flash", "list"):
            return {"items": copy.deepcopy(self.items)}
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


if __name__ == "__main__":
    unittest.main()
