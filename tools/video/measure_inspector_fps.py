#!/usr/bin/env python3
"""Measure assembled Quest video frames in an already running inspector.

This tool only submits fresh local automation `status` requests. It does not
change a client's view, authentication, capture, or screen-sharing state.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
for directory in (ROOT / "tools" / "automation", ROOT / "tools" / "client",
                  ROOT / "tools" / "window_capture"):
    sys.path.insert(0, str(directory))
import control
import packaged_probe
import win_identity

MAX_SAFE_COUNTER = (1 << 53) - 1  # UE serializes uint64 counters as JSON doubles.
HEX32 = re.compile(r"[0-9a-f]{32}\Z")
COUNTERS = ("completed_frames_lifetime", "expired_frames_lifetime", "recovered_chunks_lifetime")
VIEW_FIELDS = ("admin", "view_slot", "multiview", "reconstructed", "player_window_mask", "inspector_window")


class InvalidMeasurement(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def fail(code: str, message: str):
    raise InvalidMeasurement(code, message)


def exact_integer(value, label: str, maximum: int = MAX_SAFE_COUNTER) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail("invalid_status", f"{label} is not an exact nonnegative integer")
    if value < 0 or value > maximum or not math.isfinite(value) or value != int(value):
        fail("invalid_status", f"{label} is not an exact nonnegative integer")
    return int(value)


def boolean(data: dict, field: str) -> bool:
    value = data.get(field)
    if type(value) is not bool:
        fail("unsupported_status", f"status is missing boolean {field}")
    return value


def parsed_status(response: dict, pid: int, pin: str, slots: tuple[int, ...]) -> dict:
    if not isinstance(response, dict) or response.get("status") != "ok" or response.get("code") != "AUTOMATION_OK":
        fail("status_failed", "the inspector did not return a successful status")
    data = response.get("data")
    if not isinstance(data, dict):
        fail("unsupported_status", "the client does not provide status data")
    if not boolean(data, "inspector") or not boolean(data, "connected"):
        fail("session_stopped", "the inspector is disconnected or no longer in inspector mode")
    if boolean(data, "admin"):
        fail("unsupported_role", "this diagnostic currently supports the inspector role only, not admin")
    identity = data.get("video_measurement_identity")
    if not isinstance(identity, dict):
        fail("unsupported_status", "this client lacks video measurement identity; rebuild it")
    if not boolean(identity, "inspector_session_valid"):
        fail("session_stopped", "the authenticated inspector session is not valid")
    if exact_integer(identity.get("process_id"), "process_id", (1 << 32) - 1) != pid:
        fail("process_changed", "the status was returned by a different process")
    connection = identity.get("connection_id")
    certificate = identity.get("server_certificate_sha256")
    if not isinstance(connection, str) or not HEX32.fullmatch(connection):
        fail("unsupported_status", "the connection identity is unavailable")
    if not isinstance(certificate, str) or not packaged_probe.SHA256.fullmatch(certificate):
        fail("unsupported_status", "the connection certificate identity is unavailable")
    if certificate != pin:
        fail("server_changed", "the connection's expected server pin does not match")

    for field in ("multiview", "reconstructed", "inspector_window"):
        boolean(data, field)
    view_slot = data.get("view_slot")
    if isinstance(view_slot, bool) or not isinstance(view_slot, (int, float)) or view_slot not in (-1, 0, 1):
        fail("invalid_status", "invalid selected view slot")
    view_slot = int(view_slot)
    window_mask = exact_integer(data.get("player_window_mask"), "player_window_mask", 3)
    desired = (0 if data["reconstructed"] else 3 if data["multiview"]
               else (1 << view_slot) if view_slot in (0, 1) else 0) | window_mask
    if any(not desired & (1 << slot) for slot in slots):
        fail("not_subscribed", "a requested slot is not selected by this inspector")

    streams = data.get("video_streams")
    if not isinstance(streams, list):
        fail("unsupported_status", "video stream status is unavailable")
    indexed = {}
    for stream in streams:
        if not isinstance(stream, dict):
            fail("invalid_status", "invalid video stream")
        slot = exact_integer(stream.get("slot"), "slot", 1)
        if slot in indexed:
            fail("invalid_status", "duplicate video stream slot")
        indexed[slot] = stream
    parsed = {}
    for slot in slots:
        stream = indexed.get(slot)
        if stream is None:
            fail("unsupported_status", "the requested video stream slot is unavailable")
        stream_id = exact_integer(stream.get("stream_id"), "stream_id", (1 << 32) - 1)
        if stream_id == 0:
            fail("no_stream", "a requested slot has no active stream generation")
        parsed[slot] = {"stream_id": stream_id,
                        "fresh": boolean(stream, "current_stream_texture_fresh"),
                        **{field: exact_integer(stream.get(field), field) for field in COUNTERS}}
    return {"connection": connection, "certificate": certificate,
            "view": {field: data[field] for field in VIEW_FIELDS}, "streams": parsed}


def evaluate(samples: list[dict], process: dict, server: dict, slots: tuple[int, ...]) -> dict:
    if len(samples) < 2:
        fail("insufficient_samples", "at least two status snapshots are required")
    first = samples[0]
    for i, sample in enumerate(samples):
        start, end = sample["start"], sample["end"]
        if not all(isinstance(x, (int, float)) and math.isfinite(x) for x in (start, end)) or end < start:
            fail("invalid_clock", "the monotonic clock returned an invalid interval")
        if i and start < samples[i-1]["end"]:
            fail("invalid_clock", "status request intervals overlap or the monotonic clock moved backwards")
        value = sample["value"]
        if value["connection"] != first["value"]["connection"] or value["certificate"] != first["value"]["certificate"]:
            fail("connection_changed", "the authenticated connection changed during measurement")
        if value["view"] != first["value"]["view"]:
            fail("view_changed", "the inspector view or window subscription changed")
        if i:
            previous = samples[i-1]["value"]
            for slot in slots:
                if value["streams"][slot]["stream_id"] != first["value"]["streams"][slot]["stream_id"]:
                    fail("stream_changed", "the stream generation changed during measurement")
                if any(value["streams"][slot][field] < previous["streams"][slot][field] for field in COUNTERS):
                    fail("counter_reset", "a lifetime counter decreased or wrapped during measurement")
    last = samples[-1]
    lower = last["start"] - first["end"]
    upper = last["end"] - first["start"]
    elapsed = ((last["start"] + last["end"]) - (first["start"] + first["end"])) / 2
    if not (0 < lower <= elapsed <= upper):
        fail("invalid_clock", "the sampling interval cannot establish a positive elapsed time")
    results = []
    for slot in slots:
        before, after = first["value"]["streams"][slot], last["value"]["streams"][slot]
        complete = after[COUNTERS[0]] - before[COUNTERS[0]]
        results.append({"slot": slot, "stream_id": before["stream_id"],
                        "completed_frames_delta": complete,
                        "assembled_fps_estimate": complete / elapsed,
                        "assembled_fps_bounds": [complete / upper, complete / lower],
                        "expired_frames_delta": after[COUNTERS[1]] - before[COUNTERS[1]],
                        "recovered_chunks_delta": after[COUNTERS[2]] - before[COUNTERS[2]],
                        "texture_fresh_samples": sum(s["value"]["streams"][slot]["fresh"] for s in samples),
                        "texture_fresh_last_sample": after["fresh"]})
    return {"status": "measured" if all(item["completed_frames_delta"] > 0 for item in results) else "no_progress",
            "process": process, "server": server, "sample_count": len(samples),
            "elapsed_seconds_estimate": elapsed, "elapsed_seconds_bounds": [lower, upper],
            "maximum_status_latency_seconds": max(s["end"] - s["start"] for s in samples),
            "streams": results,
            "not_measured": ["display_or_gpu_fps", "game_tick_fps", "cpu_usage", "image_content"],
            "limitations": ["Generation and view continuity are checked at the status snapshots only.",
                            "Assembly counts and texture freshness do not prove what a window displayed.",
                            "The connection reports its configured expected pin, not an independently measured peer certificate.",
                            "Server auth/join evidence is aggregate and cannot identify this inspector.",
                            "Matching pins do not prove the connection terminates at this particular Docker instance.",
                            "Results describe the single responding GameInstance, not all instances in the process."]}


class Runtime:
    def __init__(self, name: str, pid: int, hwnd: int, container: str, pin: str):
        self.name, self.pid, self.hwnd, self.container, self.pin = name, pid, hwnd, container, pin
        self.windows = win_identity.Windows()
        self.directory = control.root_directory() / name
        control.prepare_directory(self.directory)

    def process_identity(self) -> dict:
        snapshot = self.windows.snapshot(self.pid, self.hwnd, self.hwnd)
        if snapshot["main"]["class"] != "UnrealWindow" or snapshot["main"]["title"] != self.name:
            fail("process_changed", "the selected main window does not match the inspector name")
        return snapshot

    def server_identity(self) -> dict:
        identity, private_events = packaged_probe.server_snapshot(self.container, self.pin)
        # Match only inside this process; identifiers and raw event markers
        # must never appear in the report or an on-disk diagnostic artifact.
        accepted = set(re.findall(r"MRFPS_AUTH_ACCEPT role=inspector subject=([A-Za-z0-9._:-]{1,128})(?:\s|$)", private_events))
        joined = set(re.findall(r"MRFPS_JOIN role=inspector slot=-1 name=([A-Za-z0-9._:-]{1,128})(?:\s|$)", private_events))
        if not accepted.intersection(joined):
            fail("server_auth_evidence_missing", "recent local server logs contain no matching inspector auth and join")
        return identity

    def status(self, timeout: float) -> dict:
        operation_id = str(uuid.uuid4())
        request = {"version": 1, "id": operation_id, "operation": "status", "parameters": {}}
        result = control.submit(self.directory, request)
        deadline = time.monotonic() + timeout
        archived = self.directory / "archive" / operation_id / "result.json"
        while True:
            for candidate in (result, archived):
                try:
                    if candidate.is_symlink():
                        fail("invalid_status", "unsafe status result file")
                    with candidate.open("rb") as handle:
                        contents = handle.read(16385)
                    if len(contents) > 16384:
                        fail("invalid_status", "status result exceeds its size limit")
                    response = json.loads(contents)
                    if not isinstance(response, dict) or response.get("id") != operation_id:
                        fail("invalid_status", "status response ID mismatch")
                    return response
                except FileNotFoundError:
                    pass  # May have been atomically moved into archive.
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                fail("status_timeout", "the inspector did not respond before the timeout")
            time.sleep(min(0.05, remaining))


def measure(runtime, pid: int, pin: str, slots: tuple[int, ...], duration: float,
            interval: float, timeout: float, clock=time.monotonic, sleep=time.sleep) -> dict:
    process = runtime.process_identity()
    server = runtime.server_identity()
    samples = []
    end_at = None
    while True:
        if runtime.process_identity() != process:
            fail("process_changed", "the client process or main window changed")
        start = clock()
        response = runtime.status(timeout)
        end = clock()
        if not math.isfinite(start) or not math.isfinite(end) or end < start or (samples and start < samples[-1]["end"]):
            fail("invalid_clock", "the monotonic clock moved backwards or returned an invalid interval")
        if runtime.process_identity() != process:
            fail("process_changed", "the client process or main window changed")
        samples.append({"start": start, "end": end, "value": parsed_status(response, pid, pin, slots)})
        if end_at is None:
            end_at = end + duration
        if end >= end_at:
            break
        if len(samples) > 122:
            fail("invalid_clock", "the sample limit was reached without the expected time passing")
        sleep_for = min(interval, end_at - end)
        if sleep_for > 0:
            sleep(sleep_for)
    if runtime.server_identity() != server:
        fail("server_changed", "the local container changed during measurement")
    if runtime.process_identity() != process:
        fail("process_changed", "the client process or main window changed")
    return evaluate(samples, process["process"], {**server, "expected_server_certificate_sha256": pin,
                    "startup_log_pin_match": True, "recent_inspector_auth_join_evidence": True}, slots)


def bounded_number(raw: str, minimum: float, maximum: float) -> float:
    try:
        value = float(raw)
    except ValueError:
        raise argparse.ArgumentTypeError("expected a number") from None
    if not math.isfinite(value) or not minimum <= value <= maximum:
        raise argparse.ArgumentTypeError(f"must be between {minimum:g} and {maximum:g}")
    return value


def positive(raw: str) -> int:
    try:
        value = int(raw, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("expected a positive integer") from None
    if value <= 0:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="MRFPSWindowName of the running inspector")
    parser.add_argument("--pid", required=True, type=positive, help="actual Windows client PID")
    parser.add_argument("--main-hwnd", required=True, type=positive)
    parser.add_argument("--container", required=True, help="local WSL Ubuntu Docker server container")
    parser.add_argument("--fingerprint", required=True, help="expected public server SHA-256 certificate fingerprint")
    parser.add_argument("--slot", required=True, action="append", type=int, choices=(0, 1))
    parser.add_argument("--duration-seconds", type=lambda x: bounded_number(x, 2, 60), default=15.)
    parser.add_argument("--interval-seconds", type=lambda x: bounded_number(x, .5, 5), default=1.)
    parser.add_argument("--status-timeout-seconds", type=lambda x: bounded_number(x, 1, 10), default=3.)
    args = parser.parse_args(argv)
    try:
        if not control.NAME.fullmatch(args.name):
            fail("invalid_argument", "invalid inspector name")
        if not packaged_probe.CONTAINER.fullmatch(args.container):
            fail("invalid_argument", "invalid local server container name")
        pin = args.fingerprint.lower()
        if not packaged_probe.SHA256.fullmatch(pin):
            fail("invalid_argument", "fingerprint must be 64 hexadecimal characters")
        if len(args.slot) != len(set(args.slot)):
            fail("invalid_argument", "each slot can only be requested once")
        if os.name != "nt":
            fail("unsupported_platform", "the Windows inspector diagnostic requires Windows")
        runtime = Runtime(args.name, args.pid, args.main_hwnd, args.container, pin)
        report = measure(runtime, args.pid, pin, tuple(sorted(args.slot)), args.duration_seconds,
                         args.interval_seconds, args.status_timeout_seconds)
        print(json.dumps(report, ensure_ascii=False, allow_nan=False))
        return 0 if report["status"] == "measured" else 3
    except KeyboardInterrupt:
        print(json.dumps({"status": "invalid", "code": "interrupted", "message": "measurement interrupted"}))
        return 2
    except InvalidMeasurement as error:
        print(json.dumps({"status": "invalid", "code": error.code, "message": str(error)}))
        return 2
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        # Probe and filesystem exceptions can include paths/log content. Avoid
        # reporting them; the code says which verification did not complete.
        print(json.dumps({"status": "invalid", "code": "verification_failed",
                          "message": "client or local server verification did not complete"}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
