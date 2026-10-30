import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
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
        self.timeout_after_accept = False
        self.list_calls = 0

    def call(self, *args, payload=None):
        if args[:2] == ("flash", "list"):
            self.list_calls += 1
            return [copy.deepcopy(self.service)]
        if args[:2] == ("flash", "update"):
            self.updates += 1
            self.service["spec"] = copy.deepcopy(payload["spec"])
            self.service["generation"] += 1
            if self.timeout_after_accept:
                raise subprocess.TimeoutExpired("fixture", 1)
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

    def test_timeout_after_accept_is_sanitized_and_reconciled_without_retry(self):
        self.client.timeout_after_accept = True
        with self.assertRaisesRegex(ValueError, "FLASH_UPDATE_OUTCOME_UNKNOWN"):
            update(self.client, self.path, IMAGE, KEY, CIDRS)
        self.assertEqual(update(self.client, self.path, IMAGE, KEY, CIDRS)["operation"], "pending-reconciled")
        self.assertEqual(self.client.updates, 1)

    def test_simultaneous_calls_are_locked_and_do_not_send_twice(self):
        entered, release = threading.Event(), threading.Event()
        original = self.client.call
        def slow(*args, **kwargs):
            if args[:2] == ("flash", "list") and not entered.is_set():
                entered.set()
                if not release.wait(5): raise RuntimeError("fixture wait timed out")
            return original(*args, **kwargs)
        self.client.call = slow
        results = []
        def first():
            try: results.append(update(self.client, self.path, IMAGE, KEY, CIDRS))
            except Exception as exc: results.append(exc)
        thread = threading.Thread(target=first)
        thread.start()
        try:
            self.assertTrue(entered.wait(5))
            with self.assertRaisesRegex(ValueError, "FLASH_STATE_LOCK_HELD"):
                update(self.client, self.path, IMAGE, KEY, CIDRS)
        finally:
            release.set()
            thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(results), 1)
        self.assertIsInstance(results[0], dict)
        self.assertEqual(results[0]["operation"], "update-accepted")
        self.assertEqual(self.client.updates, 1)

    def test_lock_is_exclusive_across_processes_and_released_on_exit(self):
        code = ("import sys; from pathlib import Path; from update_owned_flash import state_lock; "
                "lock=state_lock(Path(sys.argv[1])); lock.__enter__(); "
                "print('ready',flush=True); sys.stdin.readline(); lock.__exit__(None,None,None)")
        child = subprocess.Popen([sys._base_executable, "-c", code, str(self.path)],
                                 cwd=Path(__file__).resolve().parent,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            ready=child.stdout.readline().strip()
            if ready != "ready":
                _, stderr=child.communicate(timeout=5)
                self.fail("lock fixture failed to start: " + stderr[-500:])
            with self.assertRaisesRegex(ValueError, "FLASH_STATE_LOCK_HELD"):
                update(self.client, self.path, IMAGE, KEY, CIDRS)
            self.assertEqual(self.client.updates, 0)
        finally:
            try:
                child.communicate(input="\n" if child.poll() is None else None, timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate(timeout=5)
        self.assertEqual(child.returncode, 0)
        self.assertEqual(update(self.client, self.path, IMAGE, KEY, CIDRS)["operation"], "update-accepted")
        self.assertEqual(self.client.updates, 1)

    def test_drift_or_generation_change_between_reads_prevents_put(self):
        for change in ("spec", "generation"):
            with self.subTest(change=change):
                client=FakeFlash()
                state={**self.state, "observed_spec_sha256":digest(client.service["spec"])}
                self.path.write_text(json.dumps(state), encoding="utf-8")
                original=client.call
                def mutate(*args, **kwargs):
                    if args[:2] == ("flash", "list") and client.list_calls == 1:
                        if change == "spec": client.service["spec"]["env"]["RACE"] = "changed"
                        else: client.service["generation"] += 1
                    return original(*args, **kwargs)
                client.call=mutate
                with self.assertRaisesRegex(ValueError, "FLASH_DRIFT_BEFORE_UPDATE"):
                    update(client, self.path, IMAGE, KEY, CIDRS)
                self.assertEqual(client.updates,0)
                self.assertNotIn("pending_spec_sha256",json.loads(self.path.read_text(encoding="utf-8")))

    def test_service_and_deployment_identity_and_generation_are_required(self):
        for field,value in (("service_id",None),("service_id","not-uuid"),("deployment_id",None),("project_id",None)):
            with self.subTest(field=field,value=value):
                self.path.write_text(json.dumps({**self.state,field:value}),encoding="utf-8")
                with self.assertRaisesRegex(ValueError,"OWNED_FLASH_IDENTITY_INVALID"):
                    update(self.client,self.path,IMAGE,KEY,CIDRS)
                self.assertEqual(self.client.updates,0)
        self.path.write_text(json.dumps(self.state),encoding="utf-8")
        self.client.service["generation"]=None
        with self.assertRaisesRegex(ValueError,"FLASH_GENERATION_UNVERIFIED"):
            update(self.client,self.path,IMAGE,KEY,CIDRS)

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
