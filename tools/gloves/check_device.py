"""Collect real DivingStation OSC for a bounded, reproducible two-hand input check.

Does not install drivers, send vibration, inject test data, or infer a wrist position.
Exit 0 means both hands are reported connected with fresh validated flex; --require-motion also requires
measured flex changes on both hands. This is not an end-to-end Quest/MR test.
"""
from __future__ import annotations

import argparse
import datetime
import ipaddress
import json
import math
import select
import socket
import sys
import time
from pathlib import Path
from typing import Any

from contactglove_bridge import MAX_PACKET, decode_packet, encode_message, is_glove_message, parse_device_status


class HandObservation:
    def __init__(self) -> None:
        self.counts: dict[str, int] = {}
        self.last_received: dict[str, float] = {}
        self.last_curls: list[float] | None = None
        self.minimum: list[float] | None = None
        self.maximum: list[float] | None = None
        self.connected = False
        self.battery = -1
        self.valid_samples = 0

    def observe_connection(self, connected: bool, battery: int, now: float) -> None:
        if connected != self.connected:
            self.minimum = self.maximum = None
            self.valid_samples = 0
        self.connected = connected
        self.battery = battery
        self.counts["Device"] = self.counts.get("Device", 0) + 1
        self.last_received["Device"] = now

    def is_connected(self, now: float, stale_after: float) -> bool:
        return self.connected and "Device" in self.last_received and now - self.last_received["Device"] <= stale_after

    def observe(self, address: str, args: list[Any], now: float, stale_after: float = 0.15) -> None:
        stream = address.rsplit("/", 1)[-1]
        self.counts[stream] = self.counts.get(stream, 0) + 1
        self.last_received[stream] = now
        if stream == "Hand":
            curls = [sum(max(0.0, min(1.0, args[3 + finger * 4 + joint])) for joint in range(3)) / 3 for finger in range(5)]
            self.last_curls = curls
            if self.is_connected(now, stale_after):
                self.valid_samples += 1
                self.minimum = [min(a, b) for a, b in zip(self.minimum, curls)] if self.minimum is not None else list(curls)
                self.maximum = [max(a, b) for a, b in zip(self.maximum, curls)] if self.maximum is not None else list(curls)

    def report(self, now: float, stale_after: float) -> dict[str, Any]:
        spans = [high - low for low, high in zip(self.minimum, self.maximum)] if self.minimum is not None else None
        ages = {name: max(0.0, now - received) for name, received in self.last_received.items()}
        raw_fresh = "Hand" in ages and ages["Hand"] <= stale_after
        connected = self.is_connected(now, stale_after)
        return {
            "packet_counts": self.counts,
            "stream_age_seconds": ages,
            "flex_stream_fresh": raw_fresh,
            "device_status_fresh": "Device" in ages and ages["Device"] <= stale_after,
            "device_reports_connected": self.connected,
            "connected": connected,
            "battery_percent": self.battery,
            "flex_fresh": raw_fresh and connected and self.valid_samples > 0,
            "connected_flex_samples": self.valid_samples,
            "last_received_curls_thumb_to_little": self.last_curls,
            "measured_curl_spans": spans,
            "measured_motion": connected and raw_fresh and spans is not None and max(spans) >= 0.08,
            "positional_tracking_verified": False,
        }


def collect(station_host: str, station_port: int, listen_port: int, duration: float,
            stale_after: float, device_id: str, require_motion: bool) -> dict[str, Any]:
    if str(ipaddress.IPv4Address(station_host)) != "127.0.0.1":
        raise ValueError("run this check on the PC hosting DivingStation (127.0.0.1)")
    if not 0.1 <= duration <= 60 or not 0.02 <= stale_after <= 1:
        raise ValueError("duration must be 0.1..60 s and stale-after 0.02..1 s")
    if any(not 1024 <= p <= 65535 for p in (station_port, listen_port)):
        raise ValueError("ports must be 1024..65535")
    station = (station_host, station_port)
    observations = [HandObservation(), HandObservation()]
    selected_id = device_id
    ignored_devices: set[str] = set()
    received = rejected = ignored_routes = resets = 0
    sample_start = time.monotonic()
    deadline = sample_start + duration
    next_connect = 0.0
    last_station_packet = -1000.0
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        receiver.bind(("127.0.0.1", listen_port))
        receiver.setblocking(False)
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            receiver.ioctl(socket.SIO_UDP_CONNRESET, False)
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_connect and now - last_station_packet > 0.5:
                receiver.sendto(encode_message("/DS/HC/Connect", listen_port), station)
                next_connect = now + 2
            ready, _, _ = select.select([receiver], [], [], min(0.02, max(0.0, deadline - now)))
            if not ready:
                continue
            try:
                data, source = receiver.recvfrom(65535)
            except ConnectionResetError:
                resets += 1
                continue
            if source[0] != station_host or len(data) > MAX_PACKET:
                rejected += 1
                continue
            received += 1
            try:
                for address, tags, values in decode_packet(data):
                    if address == "/DS/HC/Device":
                        devices = parse_device_status(tags, values)
                        last_station_packet = time.monotonic()
                        if not selected_id:
                            selected = next((d for d in devices if d["main"]), devices[0] if devices else None)
                            if selected:
                                selected_id = selected["id"]
                        status = next((d for d in devices if d["id"] == selected_id), None)
                        received_at = time.monotonic()
                        for observation, side in zip(observations, ("left", "right")):
                            observation.observe_connection(bool(status and status[side + "_connected"]), status[side + "_battery"] if status else -1, received_at)
                        continue
                    if not is_glove_message(address, tags, values):
                        ignored_routes += 1
                        continue
                    last_station_packet = time.monotonic()
                    if selected_id and values[1] != selected_id:
                        ignored_devices.add(values[1])
                        continue
                    if not selected_id:
                        selected_id = values[1]
                    observations[0 if values[2] == 1 else 1].observe(address, values, time.monotonic(), stale_after)
            except (ValueError, UnicodeError):
                rejected += 1
        now = time.monotonic()
        hands = {side: observation.report(now, stale_after) for side, observation in zip(("left", "right"), observations)}
        fresh = all(hand["flex_fresh"] for hand in hands.values())
        moving = all(hand["measured_motion"] for hand in hands.values())
        passed = fresh and (moving or not require_motion)
        disconnected = [side for side, hand in hands.items() if hand["device_status_fresh"] and not hand["device_reports_connected"]]
        failure = None if passed else "DivingStation reports disconnected hands: " + ", ".join(disconnected) if disconnected else "No fresh validated connected two-hand flex stream" if not fresh else "Move fingers on both connected hands during the sampling interval"
        return {
            "check": "real_divingstation_osc_two_hand_input",
            "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "station": f"{station_host}:{station_port}",
            "listen_port": listen_port,
            "duration_seconds": now - sample_start,
            "stale_after_seconds": stale_after,
            "selected_device_id": selected_id or None,
            "ignored_device_ids": sorted(ignored_devices),
            "udp_datagrams_received": received,
            "rejected_datagrams": rejected,
            "ignored_routes": ignored_routes,
            "station_connection_resets": resets,
            "both_hands_fresh": fresh,
            "both_hands_measured_motion": moving,
            "required_motion": require_motion,
            "input_check_passed": passed,
            "quest_end_to_end_verified": False,
            "hands": hands,
            "failure_reason": failure,
        }
    finally:
        try:
            receiver.sendto(encode_message("/DS/HC/Disconnect", listen_port), station)
        except OSError:
            pass
        receiver.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--station", default="127.0.0.1")
    parser.add_argument("--station-port", type=int, default=25790)
    parser.add_argument("--listen-port", type=int, default=25787, help="separate from game25788/relay25789")
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--stale-after", type=float, default=0.15)
    parser.add_argument("--device-id", default="")
    parser.add_argument("--require-motion", action="store_true")
    parser.add_argument("--output", help="JSON evidence path; stdout also prints the report")
    args = parser.parse_args()
    try:
        report = collect(args.station, args.station_port, args.listen_port, args.duration,
                         args.stale_after, args.device_id, args.require_motion)
    except (ValueError, OSError) as error:
        report = {"check": "real_divingstation_osc_two_hand_input", "input_check_passed": False,
                  "quest_end_to_end_verified": False, "failure_reason": str(error)}
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        destination = Path(args.output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if report["input_check_passed"] else 2


if __name__ == "__main__":
    sys.exit(main())
