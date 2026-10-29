"""Relay real DivingStation OSC and optionally tracked SteamVR wrists to a Quest.

Standard-library only unless --trackers is enabled (then install 'openvr').
No recorded, synthetic, inferred positional, or fallback tracking is generated.
"""
from __future__ import annotations

import argparse
import importlib
import ipaddress
import json
import logging
import math
import select
import socket
import struct
import time
from pathlib import Path
from typing import Any

PROTOCOL_VERSION = 1
MAX_PACKET = 8192
LOG = logging.getLogger("contactglove")


def osc_string(value: str) -> bytes:
    raw = value.encode("ascii")
    if b"\0" in raw or len(raw) > 512:
        raise ValueError("invalid OSC string")
    return raw + b"\0" * (4 - len(raw) % 4)


def encode_message(address: str, *args: int | float | str) -> bytes:
    if not address.startswith("/"):
        raise ValueError("OSC address must start with /")
    tags = ","
    payload = b""
    for value in args:
        if isinstance(value, str):
            tags += "s"
            payload += osc_string(value)
        elif isinstance(value, int):
            tags += "i"
            payload += struct.pack(">i", value)
        elif isinstance(value, float) and math.isfinite(value):
            tags += "f"
            payload += struct.pack(">f", value)
        else:
            raise ValueError("unsupported OSC argument")
    return osc_string(address) + osc_string(tags) + payload


def _string(data: bytes, cursor: int) -> tuple[str, int]:
    end = data.find(b"\0", cursor)
    if end < cursor or end - cursor > 512:
        raise ValueError("unterminated OSC string")
    after = (end + 4) & ~3
    if after > len(data) or any(data[end:after]):
        raise ValueError("invalid OSC padding")
    if any(value < 32 or value > 127 for value in data[cursor:end]):
        raise ValueError("unsupported OSC string character")
    return data[cursor:end].decode("ascii"), after


def decode_packet(data: bytes, depth: int = 0) -> list[tuple[str, str, list[Any]]]:
    if depth > 4 or len(data) < 8 or len(data) > MAX_PACKET or len(data) % 4:
        raise ValueError("invalid OSC packet length or nesting")
    if data.startswith(b"#bundle\0"):
        if len(data) < 16:
            raise ValueError("truncated OSC bundle")
        cursor, messages = 16, []
        while cursor < len(data):
            if cursor + 4 > len(data):
                raise ValueError("truncated bundle size")
            length = struct.unpack_from(">I", data, cursor)[0]
            cursor += 4
            if length < 8 or length > len(data) - cursor:
                raise ValueError("truncated bundle member")
            messages.extend(decode_packet(data[cursor:cursor + length], depth + 1))
            cursor += length
        return messages
    address, cursor = _string(data, 0)
    tags, cursor = _string(data, cursor)
    if not address.startswith("/") or not tags.startswith(",") or len(tags) > 80:
        raise ValueError("invalid OSC address/types")
    values: list[Any] = []
    for tag in tags[1:]:
        if tag == "s":
            value, cursor = _string(data, cursor)
        elif tag in "if":
            if cursor + 4 > len(data):
                raise ValueError("truncated OSC number")
            value = struct.unpack_from(">" + tag, data, cursor)[0]
            cursor += 4
            if tag == "f" and not math.isfinite(value):
                raise ValueError("non-finite OSC float")
        else:
            raise ValueError("unsupported OSC tag")
        values.append(value)
    if cursor != len(data):
        raise ValueError("trailing OSC bytes")
    return [(address, tags[1:], values)]


def is_glove_message(address: str, types: str, args: list[Any]) -> bool:
    if len(args) < 3 or types[:3] != "isi" or args[0] != 1 or not args[1] or args[2] not in (0, 1):
        return False
    expected = {
        "/DS/HC/Hand": "isi" + "f" * 20,
        "/DS/HC/HandQuat": "isi" + "f" * 60,
        "/DS/HC/Wrist": "isi" + "f" * 4,
    }
    if address in expected:
        return types == expected[address]
    if address == "/DS/HC/Controller":
        return 12 <= len(args) <= 16 and types == "isi" + "i" * 6 + "f" * (len(args) - 9) and all(0 <= x <= 2 for x in args[3:9])
    return False


def parse_device_status(types: str, args: list[Any]) -> list[dict[str, Any]]:
    """Decode SDK v1 /DS/HC/Device; connection is not implied by Hand packets."""
    unit_types = "siisifiifiif"
    if not args or types != "i" + unit_types * ((len(args) - 1) // 12) or (len(args) - 1) % 12 or args[0] != 1:
        raise ValueError("invalid DivingStation device status schema")
    devices = []
    for offset in range(1, len(args), 12):
        unit = args[offset:offset + 12]
        if not unit[0] or any(unit[i] not in (0, 1) for i in (1, 6, 9)):
            raise ValueError("invalid DivingStation connected flags")
        devices.append({"id": unit[0], "main": unit[1] == 1, "type": unit[2], "name": unit[3],
                        "left_connected": unit[6] == 1, "left_battery": unit[7], "left_ping": unit[8],
                        "right_connected": unit[9] == 1, "right_battery": unit[10], "right_ping": unit[11]})
    return devices


class PendingRelay:
    """Coalesce only newly received packets, never refresh old input by retransmission."""
    def __init__(self) -> None:
        self.pending: dict[tuple[str, int], bytes] = {}

    def add(self, address: str, args: list[Any]) -> None:
        side = args[2] if address != "/DS/HC/Device" else -1
        self.pending[(address, side)] = encode_message(address, *args)

    def drain(self) -> list[bytes]:
        # The connection snapshot must precede the hand samples it validates.
        ordered = sorted(self.pending.items(), key=lambda item: item[0][0] != "/DS/HC/Device")
        self.pending.clear()
        return [packet for _, packet in ordered]

    def discard_hand(self, side: int) -> None:
        self.pending = {key: packet for key, packet in self.pending.items() if key[1] != side}


def quaternion(matrix: list[list[float]]) -> tuple[float, float, float, float]:
    """Unit (x,y,z,w) quaternion of a 3x3 orthonormal rotation matrix."""
    trace = matrix[0][0] + matrix[1][1] + matrix[2][2]
    if trace > 0:
        scale = 2 * math.sqrt(trace + 1)
        result = ((matrix[2][1] - matrix[1][2]) / scale,
                  (matrix[0][2] - matrix[2][0]) / scale,
                  (matrix[1][0] - matrix[0][1]) / scale, scale / 4)
    else:
        i = max(range(3), key=lambda index: matrix[index][index])
        j, k = (i + 1) % 3, (i + 2) % 3
        scale = 2 * math.sqrt(max(0.0, 1 + matrix[i][i] - matrix[j][j] - matrix[k][k]))
        if scale < 1e-8:
            raise ValueError("degenerate tracker rotation")
        xyz = [0.0, 0.0, 0.0]
        xyz[i] = scale / 4
        xyz[j] = (matrix[j][i] + matrix[i][j]) / scale
        xyz[k] = (matrix[k][i] + matrix[i][k]) / scale
        result = (*xyz, (matrix[k][j] - matrix[j][k]) / scale)
    magnitude = math.sqrt(sum(component * component for component in result))
    return tuple(component / magnitude for component in result)


def multiply(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def convert_pose(matrix: Any, yaw_degrees: float, translation_cm: list[float]) -> tuple[list[float], tuple[float, float, float, float]]:
    """OpenVR right/up/back metres -> UE forward/right/up centimetres, then alignment."""
    basis = [[0, 0, -1], [1, 0, 0], [0, 1, 0]]
    inverse = [list(row) for row in zip(*basis)]
    rotation = [[float(matrix[i][j]) for j in range(3)] for i in range(3)]
    vector = [float(matrix[i][3]) * 100 for i in range(3)]
    if any(not math.isfinite(x) for row in rotation for x in row) or any(not math.isfinite(x) or abs(x) > 100000 for x in vector):
        raise ValueError("invalid tracker matrix")
    for i in range(3):
        for j in range(3):
            dot = sum(rotation[i][k] * rotation[j][k] for k in range(3))
            if abs(dot - int(i == j)) > 0.08:
                raise ValueError("tracker rotation is not orthonormal")
    determinant = (rotation[0][0] * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1])
                   - rotation[0][1] * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0])
                   + rotation[0][2] * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0]))
    if abs(determinant - 1) > 0.08:
        raise ValueError("tracker rotation must not be reflected")
    theta = math.radians(yaw_degrees)
    alignment = [[math.cos(theta), -math.sin(theta), 0], [math.sin(theta), math.cos(theta), 0], [0, 0, 1]]
    converted = [sum(basis[i][j] * vector[j] for j in range(3)) for i in range(3)]
    position = [sum(alignment[i][j] * converted[j] for j in range(3)) + translation_cm[i] for i in range(3)]
    return position, quaternion(multiply(alignment, multiply(multiply(basis, rotation), inverse)))


class TrackerSource:
    def __init__(self, config: dict[str, Any]):
        self.config = config
        self.vr = importlib.import_module("openvr")
        self.system = self.vr.init(self.vr.VRApplication_Background)
        self.poses = (self.vr.TrackedDevicePose_t * self.vr.k_unMaxTrackedDeviceCount)()
        self.indices: dict[int, int] = {}
        self.next_scan = 0.0

    def close(self) -> None:
        self.vr.shutdown()

    def packets(self, device_id: str) -> list[bytes]:
        now = time.monotonic()
        if now >= self.next_scan:
            found: dict[str, int] = {}
            for index in range(self.vr.k_unMaxTrackedDeviceCount):
                if not self.system.isTrackedDeviceConnected(index):
                    continue
                try:
                    found[self.system.getStringTrackedDeviceProperty(index, self.vr.Prop_SerialNumber_String)] = index
                except Exception:
                    continue
            self.indices = {left: found[serial] for left, serial in ((1, self.config["left_serial"]), (0, self.config["right_serial"])) if serial in found}
            self.next_scan = now + 2
        poses = self.system.getDeviceToAbsoluteTrackingPose(self.vr.TrackingUniverseStanding, 0, self.poses)
        output = []
        for left, index in self.indices.items():
            pose = poses[index]
            valid = bool(pose.bDeviceIsConnected and pose.bPoseIsValid and pose.eTrackingResult == self.vr.TrackingResult_Running_OK)
            try:
                position, rotation = convert_pose(pose.mDeviceToAbsoluteTracking, self.config["yaw_degrees"], self.config["translation_cm"]) if valid else ([0.0] * 3, (0.0, 0.0, 0.0, 1.0))
            except ValueError:
                valid, position, rotation = False, [0.0] * 3, (0.0, 0.0, 0.0, 1.0)
            output.append(encode_message("/MRFPS/Tracker", 1, device_id, left, *position, *rotation, int(valid)))
        return output


def validate_tracker_config(config: dict[str, Any]) -> dict[str, Any]:
    # Requiring an explicit alignment prevents silently treating two tracking origins as one.
    for field in ("left_serial", "right_serial"):
        if not isinstance(config.get(field), str) or not config[field]:
            raise ValueError(f"{field} must name a SteamVR tracker serial")
    if config["left_serial"] == config["right_serial"]:
        raise ValueError("left and right tracker serials must differ")
    yaw = config.get("yaw_degrees")
    translation = config.get("translation_cm")
    if not isinstance(yaw, (float, int)) or not math.isfinite(yaw):
        raise ValueError("explicit finite yaw_degrees required")
    if not isinstance(translation, list) or len(translation) != 3 or any(not isinstance(x, (float, int)) or not math.isfinite(x) for x in translation):
        raise ValueError("explicit three-element translation_cm required")
    return config


def run(args: argparse.Namespace) -> None:
    targets = [(str(ipaddress.IPv4Address(target)), args.target_port) for target in args.target]
    station = (str(ipaddress.IPv4Address(args.station)), args.station_port)
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # DivingStation can emit tens of thousands of duplicate datagrams per
    # second. Keep its receive queue short so that dropping excess datagrams
    # does not turn into a long backlog of old hand poses.
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, MAX_PACKET)
    receiver.bind(("127.0.0.1", args.listen_port))
    receiver.setblocking(False)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tracker = None
    pending = PendingRelay()
    device_status: dict[str, tuple[dict[str, Any], float]] = {}
    selected_id = args.device_id
    if args.trackers:
        config = validate_tracker_config(json.loads(Path(args.trackers).read_text(encoding="utf-8")))
        tracker = TrackerSource(config)
    LOG.info("DivingStation %s:%d -> local :%d -> %s", *station, args.listen_port, targets)
    LOG.info("Waiting for real glove input; no synthetic tracking is generated")
    next_connect = next_tracker = next_forward = 0.0
    received = rejected = 0
    last_report = time.monotonic()
    last_station_packet = -1000.0
    try:
        while True:
            now = time.monotonic()
            if now >= next_connect and now - last_station_packet > 0.5:
                receiver.sendto(encode_message("/DS/HC/Connect", args.listen_port), station)
                next_connect = now + 2
            if tracker and selected_id and now >= next_tracker:
                for packet in tracker.packets(selected_id):
                    for target in targets:
                        sender.sendto(packet, target)
                next_tracker = now + 1 / 90
            if now >= next_forward:
                for packet in pending.drain():
                    for target in targets:
                        sender.sendto(packet, target)
                next_forward = now + 1 / args.rate_hz
            ready, _, _ = select.select([receiver], [], [], 0.005)
            if ready:
                try:
                    data, source = receiver.recvfrom(65535)
                except ConnectionResetError:
                    continue
                # Bound the work spent parsing a flooded local OSC stream,
                # while leaving room for both hands and the device heartbeat.
                time.sleep(0.00025)
                if source[0] != station[0] or len(data) > MAX_PACKET:
                    rejected += 1
                    continue
                try:
                    messages = decode_packet(data)
                    # Forward individually to avoid vendor bundles carrying unrecognized routes.
                    for address, tags, values in messages:
                        if address == "/DS/HC/Device":
                            devices = parse_device_status(tags, values)
                            last_station_packet = now
                            device_status = {device["id"]: (device, now) for device in devices}
                            if not selected_id:
                                selected = next((d for d in devices if d["main"]), devices[0] if devices else None)
                                if selected:
                                    selected_id = selected["id"]
                            selected_status = device_status.get(selected_id)
                            for side, field in ((1, "left_connected"), (0, "right_connected")):
                                if not selected_status or not selected_status[0][field]:
                                    pending.discard_hand(side)
                            pending.add(address, values)
                            continue
                        if not is_glove_message(address, tags, values):
                            continue
                        last_station_packet = now
                        if selected_id and values[1] != selected_id:
                            continue
                        if not selected_id:
                            selected_id = values[1]
                            LOG.info("Locked to ContactGlove device %s", selected_id)
                        status = device_status.get(selected_id)
                        connected_field = "left_connected" if values[2] == 1 else "right_connected"
                        if not status or now - status[1] > 0.15 or not status[0][connected_field]:
                            continue
                        pending.add(address, values)
                        received += 1
                except (ValueError, UnicodeError, struct.error):
                    rejected += 1
            if now - last_report >= 10:
                LOG.info("glove packets=%d rejected=%d device=%s", received, rejected, selected_id or "not connected")
                last_report = now
    finally:
        receiver.sendto(encode_message("/DS/HC/Disconnect", args.listen_port), station)
        receiver.close()
        sender.close()
        if tracker:
            tracker.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", action="append", required=True, help="Quest/PC IPv4; can be repeated")
    parser.add_argument("--target-port", type=int, default=25788)
    parser.add_argument("--station", default="127.0.0.1")
    parser.add_argument("--station-port", type=int, default=25790)
    parser.add_argument("--listen-port", type=int, default=25789)
    parser.add_argument("--device-id", default="", help="DivingStation device ID; default locks first observed device")
    parser.add_argument("--trackers", help="JSON: left_serial, right_serial, yaw_degrees, translation_cm")
    parser.add_argument("--rate-hz", type=float, default=90, help="maximum newly received samples forwarded per route/hand each second")
    args = parser.parse_args()
    for port in (args.target_port, args.station_port, args.listen_port):
        if not 1024 <= port <= 65535:
            parser.error("ports must be 1024..65535")
    if args.station != "127.0.0.1":
        parser.error("DivingStation must run on this PC (127.0.0.1)")
    if not 15 <= args.rate_hz <= 120:
        parser.error("rate-hz must be 15..120")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        run(args)
    except KeyboardInterrupt:
        LOG.info("Stopped")


if __name__ == "__main__":
    main()
