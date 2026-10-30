import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import measure_inspector_fps as subject


PIN = "a" * 64
OTHER_PIN = "b" * 64
CONNECTION = "1" * 32
SERVER = {"id": "2" * 64, "started": "2026-10-30T00:00:00Z", "restarts": 0}
PROCESS = {"process": {"pid": 123, "created": 456, "executable": "mrfps.exe"},
           "main": {"hwnd": 77, "thread": 99, "class": "UnrealWindow", "title": "TestInspector"},
           "target": {"hwnd": 77, "thread": 99, "class": "UnrealWindow", "title": "TestInspector"}}


def response(count0=0, count1=0):
    def stream(slot, count):
        return {"slot": slot, "stream_id": 10 + slot, "current_stream_texture_fresh": True,
                "completed_frames_lifetime": count, "expired_frames_lifetime": 2,
                "recovered_chunks_lifetime": 1}
    return {"id": "placeholder", "status": "ok", "code": "AUTOMATION_OK", "data": {
        "inspector": True, "connected": True, "admin": False, "view_slot": -1,
        "multiview": True, "reconstructed": False, "player_window_mask": 0,
        "inspector_window": False,
        "video_measurement_identity": {"process_id": 123, "inspector_session_valid": True,
                                       "connection_id": CONNECTION, "server_certificate_sha256": PIN},
        "video_streams": [stream(0, count0), stream(1, count1)]}}


class Clock:
    def __init__(self): self.now = 0.
    def read(self): return self.now
    def sleep(self, seconds): self.now += seconds


class Runtime:
    def __init__(self, results, clock):
        self.results = list(results)
        self.clock = clock
        self.status_calls = []
        self.processes = []
        self.servers = []

    def process_identity(self):
        return copy.deepcopy(self.processes.pop(0) if self.processes else PROCESS)

    def server_identity(self):
        return copy.deepcopy(self.servers.pop(0) if self.servers else SERVER)

    def status(self, timeout):
        self.status_calls.append(timeout)
        if not self.results:
            raise AssertionError("too many status operations")
        self.clock.now += 0.2
        value = self.results.pop(0)
        if isinstance(value, BaseException):
            raise value
        return copy.deepcopy(value)


def run(results, slots=(0, 1), configure=None):
    clock = Clock()
    runtime = Runtime(results, clock)
    if configure:
        configure(runtime)
    result = subject.measure(runtime, 123, PIN, slots, 2., 1., 3., clock.read, clock.sleep)
    return result, runtime


class InspectorVideoMeasurementTests(unittest.TestCase):
    def assert_failure(self, expected, results, slots=(0, 1), configure=None):
        with self.assertRaises(subject.InvalidMeasurement) as error:
            run(results, slots, configure)
        self.assertEqual(error.exception.code, expected)

    def test_two_generations_count_fps_and_freshness_separately(self):
        a, b, c = response(10, 50), response(16, 55), response(22, 62)
        b["data"]["video_streams"][0]["current_stream_texture_fresh"] = False
        result, runtime = run([a, b, c])
        self.assertEqual(result["status"], "measured")
        self.assertEqual(result["sample_count"], 3)
        self.assertAlmostEqual(result["elapsed_seconds_estimate"], 2.2)
        self.assertAlmostEqual(result["elapsed_seconds_bounds"][0], 2.)
        self.assertAlmostEqual(result["elapsed_seconds_bounds"][1], 2.4)
        self.assertAlmostEqual(result["streams"][0]["assembled_fps_estimate"], 12/2.2)
        self.assertAlmostEqual(result["streams"][0]["assembled_fps_bounds"][0], 5.)
        self.assertAlmostEqual(result["streams"][0]["assembled_fps_bounds"][1], 6.)
        self.assertEqual(result["streams"][0]["texture_fresh_samples"], 2)
        self.assertEqual(result["streams"][1]["texture_fresh_samples"], 3)
        self.assertIn("game_tick_fps", result["not_measured"])
        self.assertIn("cpu_usage", result["not_measured"])
        self.assertEqual(result["server"]["expected_server_certificate_sha256"], PIN)
        self.assertNotIn("certificate_sha256", result["server"])
        self.assertEqual(runtime.status_calls, [3., 3., 3.])

    def test_one_stalled_slot_is_not_success(self):
        result, _ = run([response(10, 50), response(15, 50), response(20, 50)])
        self.assertEqual(result["status"], "no_progress")
        self.assertEqual(result["streams"][1]["assembled_fps_estimate"], 0.)

    def test_no_stream_cannot_be_success(self):
        value = response()
        value["data"]["video_streams"][0]["stream_id"] = 0
        self.assert_failure("no_stream", [value])

    def test_changed_stream_even_if_other_slot_progresses(self):
        changed = response(10, 10)
        changed["data"]["video_streams"][0]["stream_id"] = 12
        self.assert_failure("stream_changed", [response(), changed, response(15, 15)])

    def test_disconnected_or_expired_auth_fails(self):
        for field in ("connected", "inspector"):
            with self.subTest(field=field):
                stopped = response(5, 5)
                stopped["data"][field] = False
                self.assert_failure("session_stopped", [response(), stopped])
        stopped = response(5, 5)
        stopped["data"]["video_measurement_identity"]["inspector_session_valid"] = False
        self.assert_failure("session_stopped", [response(), stopped])
        admin = response()
        admin["data"]["admin"] = True
        self.assert_failure("unsupported_role", [admin])

    def test_old_client_and_missing_connection_identity_fail_closed(self):
        old = response()
        del old["data"]["video_measurement_identity"]
        self.assert_failure("unsupported_status", [old])
        no_connection = response()
        del no_connection["data"]["video_measurement_identity"]["connection_id"]
        self.assert_failure("unsupported_status", [no_connection])

    def test_changed_connection_cert_or_process_is_rejected(self):
        for key, value, code in (("connection_id", "3"*32, "connection_changed"),
                                 ("server_certificate_sha256", OTHER_PIN, "server_changed"),
                                 ("process_id", 124, "process_changed")):
            with self.subTest(key=key):
                changed = response(5, 5)
                changed["data"]["video_measurement_identity"][key] = value
                results = [response(), changed, response(15, 15)]
                self.assert_failure(code, results)

    def test_pid_reuse_with_changed_process_creation_time_is_rejected(self):
        changed = copy.deepcopy(PROCESS)
        changed["process"]["created"] += 1
        self.assert_failure("process_changed", [response()],
                            configure=lambda r: r.processes.extend([PROCESS, changed]))

    def test_local_container_restart_is_rejected(self):
        changed = {**SERVER, "restarts": 1}
        self.assert_failure("server_changed", [response(), response(5, 5), response(10, 10)],
                            configure=lambda r: r.servers.extend([SERVER, changed]))

    def test_selected_view_and_subscriptions_must_stay_stable(self):
        changed = response(5, 5)
        changed["data"]["inspector_window"] = True
        self.assert_failure("view_changed", [response(), changed, response(10, 10)])
        unsubscribed = response()
        unsubscribed["data"]["multiview"] = False
        unsubscribed["data"]["view_slot"] = 0
        self.assert_failure("not_subscribed", [unsubscribed])
        unsubscribed["data"]["view_slot"] = 0.0  # Unreal JSON may emit an integral double.
        single, _ = run([unsubscribed, unsubscribed, unsubscribed], slots=(0,))
        self.assertEqual(single["status"], "no_progress")

    def test_counter_reset_rollover_and_inexact_json_values_are_rejected(self):
        for field in subject.COUNTERS:
            with self.subTest(field=field):
                first, second = response(10, 10), response(20, 20)
                first["data"]["video_streams"][0][field] = 15
                second["data"]["video_streams"][0][field] = 1
                self.assert_failure("counter_reset", [first, second, response(30, 30)])
        for value in (True, -1, 0.5, float("nan"), float("inf"), 1 << 53, 10**1000):
            with self.subTest(value=value):
                bad = response()
                bad["data"]["video_streams"][0]["completed_frames_lifetime"] = value
                self.assert_failure("invalid_status", [bad])

    def test_clock_rollover_overlap_and_no_elapsed_time_are_rejected(self):
        one = subject.parsed_status(response(), 123, PIN, (0, 1))
        two = subject.parsed_status(response(5, 5), 123, PIN, (0, 1))
        for times in (((1, 2), (1.5, 3)), ((5, 5), (5, 5)), ((4, 3), (5, 6))):
            with self.subTest(times=times), self.assertRaises(subject.InvalidMeasurement) as error:
                subject.evaluate([{"start": times[0][0], "end": times[0][1], "value": one},
                                  {"start": times[1][0], "end": times[1][1], "value": two}],
                                 PROCESS["process"], SERVER, (0, 1))
            self.assertEqual(error.exception.code, "invalid_clock")

    def test_timeout_and_interruption_never_produce_a_measurement(self):
        self.assert_failure("status_timeout", [subject.InvalidMeasurement("status_timeout", "timeout")])
        with self.assertRaises(KeyboardInterrupt):
            run([KeyboardInterrupt()])

    def test_runtime_requests_only_new_status_operations(self):
        requests = []
        def submit(directory, request):
            requests.append(copy.deepcopy(request))
            file = directory / (request["id"] + ".json")
            result = response()
            result["id"] = request["id"]
            file.write_text(json.dumps(result), encoding="utf-8")
            return file
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(subject.control, "submit", side_effect=submit):
            runtime = subject.Runtime.__new__(subject.Runtime)
            runtime.directory = Path(folder)
            runtime.status(3.)
            runtime.status(3.)
        self.assertEqual([item["operation"] for item in requests], ["status", "status"])
        self.assertTrue(all(item["parameters"] == {} for item in requests))
        self.assertNotEqual(requests[0]["id"], requests[1]["id"])
        self.assertTrue(all(subject.control.valid_id(item["id"]) for item in requests))

    def test_server_auth_evidence_is_aggregate_and_identifiers_are_not_returned(self):
        runtime = subject.Runtime.__new__(subject.Runtime)
        runtime.container, runtime.pin = "game", PIN
        events = ("MRFPS_AUTH_ACCEPT role=inspector subject=private-identifier\n"
                  "MRFPS_JOIN role=inspector slot=-1 name=private-identifier\n")
        with mock.patch.object(subject.packaged_probe, "server_snapshot", return_value=(SERVER, events)):
            evidence = runtime.server_identity()
        self.assertEqual(evidence, SERVER)
        self.assertNotIn("private-identifier", json.dumps(evidence))
        with mock.patch.object(subject.packaged_probe, "server_snapshot", return_value=(SERVER, events.replace("name=private-identifier", "name=other"))):
            with self.assertRaises(subject.InvalidMeasurement) as error:
                runtime.server_identity()
        self.assertEqual(error.exception.code, "server_auth_evidence_missing")

    def test_real_probe_filters_server_log_evidence_before_diagnostic(self):
        runtime = subject.Runtime.__new__(subject.Runtime)
        runtime.container, runtime.pin = "game", PIN
        started = "2026-10-30T00:00:00Z"
        inspect = f"{'2'*64}|{started}|0|42|true|healthy"
        def logs(*arguments):
            if "--until" in arguments:
                return iter([f"{started} MRFPS_DTLS_SERVER_SHA256={PIN}\n"])
            return iter([f"{started} MRFPS_AUTH_ACCEPT role=inspector subject=private-identifier expires=123\n",
                         f"{started} MRFPS_JOIN role=inspector slot=-1 name=private-identifier\n"])
        with mock.patch.object(subject.packaged_probe, "docker", return_value=inspect), \
             mock.patch.object(subject.packaged_probe, "docker_log_lines", side_effect=logs):
            evidence = runtime.server_identity()
        self.assertEqual(evidence, SERVER)
        self.assertNotIn("private-identifier", json.dumps(evidence))

    def test_archived_status_receipt_and_timeout(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = subject.Runtime.__new__(subject.Runtime)
            runtime.directory = Path(folder)
            def archive_submit(directory, request):
                target = directory / "archive" / request["id"] / "result.json"
                target.parent.mkdir(parents=True)
                result = response()
                result["id"] = request["id"]
                target.write_text(json.dumps(result), encoding="utf-8")
                return directory / request["id"] / "result.json"
            with mock.patch.object(subject.control, "submit", side_effect=archive_submit):
                self.assertEqual(runtime.status(3.)["status"], "ok")
            clock = Clock()
            with mock.patch.object(subject.control, "submit", return_value=Path(folder) / "absent"), \
                 mock.patch.object(subject.time, "monotonic", side_effect=clock.read), \
                 mock.patch.object(subject.time, "sleep", side_effect=clock.sleep):
                with self.assertRaises(subject.InvalidMeasurement) as error:
                    runtime.status(1.)
            self.assertEqual(error.exception.code, "status_timeout")


if __name__ == "__main__":
    unittest.main()
