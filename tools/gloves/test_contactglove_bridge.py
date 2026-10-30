import importlib.util
import math
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("contactglove_bridge.py")
spec = importlib.util.spec_from_file_location("bridge", MODULE_PATH)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)
align_spec = importlib.util.spec_from_file_location("alignment", MODULE_PATH.with_name("calibrate_alignment.py"))
alignment = importlib.util.module_from_spec(align_spec)
align_spec.loader.exec_module(alignment)


class OSCProtocolTests(unittest.TestCase):
    def test_vendor_contactglove2_hand_schema(self):
        values = [1, "cg2-id", 1] + [0.1, 0.2, 0.3, -0.1] * 5
        packet = bridge.encode_message("/DS/HC/Hand", *values)
        address, tags, decoded = bridge.decode_packet(packet)[0]
        self.assertTrue(bridge.is_glove_message(address, tags, decoded))
        self.assertEqual(decoded[:3], values[:3])
        self.assertAlmostEqual(decoded[7], values[7])

    def test_connect_advertises_receive_port(self):
        self.assertEqual(bridge.decode_packet(bridge.encode_message("/DS/HC/Connect", 25789)), [("/DS/HC/Connect", "i", [25789])])

    def test_wrong_version_or_side_rejected(self):
        for version, side in ((2, 1), (1, 2)):
            self.assertFalse(bridge.is_glove_message("/DS/HC/Hand", "isi" + "f" * 20, [version, "id", side] + [0.0] * 20))

    def test_controller_pressed_distinct_from_touched(self):
        values = [1, "cg2-id", 0, 2, 1, 0, 2, 0, 0, 0.8, 0.4, 0.0]
        address, tags, decoded = bridge.decode_packet(bridge.encode_message("/DS/HC/Controller", *values))[0]
        self.assertTrue(bridge.is_glove_message(address, tags, decoded))
        self.assertEqual(decoded[3:9], values[3:9])

    def test_bundle_with_multiple_hands(self):
        messages = [bridge.encode_message("/DS/HC/Wrist", 1, "id", side, 1.0, 0.0, 0.0, 0.0) for side in (1, 0)]
        bundle = b"#bundle\0" + struct.pack(">Q", 1) + b"".join(struct.pack(">I", len(packet)) + packet for packet in messages)
        self.assertEqual([m[2][2] for m in bridge.decode_packet(bundle)], [1, 0])

    def test_bad_packets_do_not_decode(self):
        packet = bridge.encode_message("/DS/HC/Wrist", 1, "id", 1, 1.0, 0.0, 0.0, 0.0)
        bad = [packet[:-1], packet + b"\0" * 4, b"#bundle\0" + b"\0" * 8 + struct.pack(">I", 9000), packet[:-4] + struct.pack(">f", math.nan)]
        for value in bad:
            with self.subTest(value=value), self.assertRaises(ValueError):
                bridge.decode_packet(value)

    def test_unknown_route_not_forwarded(self):
        self.assertFalse(bridge.is_glove_message("/evil", "isi", [1, "id", 1]))

    def test_device_snapshot_contains_independent_connection_flags(self):
        args = [1, "fixture", 1, 2, "ContactGlove2", 6, 0.0, 0, 0, 0.0, 1, 85, 2.0]
        address, tags, decoded = bridge.decode_packet(bridge.encode_message("/DS/HC/Device", *args))[0]
        status = bridge.parse_device_status(tags, decoded)[0]
        self.assertFalse(status["left_connected"])
        self.assertTrue(status["right_connected"])
        self.assertEqual(status["right_battery"], 85)
        with self.assertRaises(ValueError):
            bridge.parse_device_status(tags, decoded[:-1])

    def test_pending_relay_does_not_retransmit_cached_input(self):
        relay = bridge.PendingRelay()
        relay.add("/DS/HC/Hand", [1, "fixture", 1] + [0.1] * 20)
        relay.add("/DS/HC/Hand", [1, "fixture", 1] + [0.5] * 20)
        batch = relay.drain()
        self.assertEqual(len(batch), 1)
        self.assertAlmostEqual(bridge.decode_packet(batch[0])[0][2][3], 0.5)
        self.assertEqual(relay.drain(), [])

    def test_status_and_sample_have_independent_timeouts(self):
        status = ({"right_connected": True}, 10.0)
        self.assertTrue(bridge.connected_status(status, "right_connected", 10.25))
        self.assertFalse(bridge.connected_status(status, "right_connected", 11.01))
        self.assertFalse(bridge.connected_status(({"right_connected": False}, 10.2), "right_connected", 10.21))
        self.assertFalse(bridge.connected_status(status, "right_connected", 9.9))
        relay = bridge.PendingRelay()
        sample = [1, "fixture", 0] + [0.5] * 20
        relay.add("/DS/HC/Hand", sample, received_at=10.0)
        self.assertEqual(relay.drain(10.16), [])
        self.assertEqual(relay.expired, 1)
        relay.add("/DS/HC/Hand", sample, received_at=10.20)
        self.assertEqual(len(relay.drain(10.25)), 1)

    def test_explicit_disconnect_discards_pending_samples(self):
        relay = bridge.PendingRelay()
        relay.add("/DS/HC/Hand", [1, "fixture", 0] + [0.5] * 20, received_at=10.0)
        relay.discard_hand(0)
        self.assertEqual(relay.drain(10.01), [])

    @unittest.skipUnless(os.name == "nt", "process suspension fixture uses Windows NT APIs")
    def test_process_resume_drops_old_held_input_and_requires_new_status(self):
        import ctypes
        ntdll = ctypes.WinDLL("ntdll")
        for function in (ntdll.NtSuspendProcess, ntdll.NtResumeProcess):
            function.argtypes = [ctypes.c_void_p]
            function.restype = ctypes.c_long
        station = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reserve = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for sock in (station, target, reserve):
            sock.bind(("127.0.0.1", 0))
        listen_port = reserve.getsockname()[1]
        reserve.close()
        station.settimeout(5)
        target.settimeout(.2)
        process = subprocess.Popen([sys._base_executable, str(MODULE_PATH), "--target", "127.0.0.1",
            "--target-port", str(target.getsockname()[1]), "--station-port", str(station.getsockname()[1]),
            "--listen-port", str(listen_port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        suspended = False
        try:
            _, source = station.recvfrom(8192)
            status = bridge.encode_message("/DS/HC/Device", 1, "fixture", 1, 2, "ContactGlove2", 6, 0.0, 1, 80, 0.0, 1, 80, 0.0)
            held = bridge.encode_message("/DS/HC/Hand", 1, "fixture", 0, *([.8] * 20))
            station.sendto(status, source)
            self.assertEqual(target.recvfrom(8192)[0], status)
            self.assertEqual(ntdll.NtSuspendProcess(int(process._handle)), 0)
            suspended = True
            station.sendto(held, source)
            time.sleep(.25)
            self.assertEqual(ntdll.NtResumeProcess(int(process._handle)), 0)
            suspended = False
            try:
                unexpected = bridge.decode_packet(target.recvfrom(8192)[0])[0][0]
            except socket.timeout:
                unexpected = None
            self.assertIsNone(unexpected, f"unexpected packet after resume: {unexpected}")
            # A new status, followed by live input, re-enables forwarding.
            station.sendto(status, source)
            self.assertEqual(target.recvfrom(8192)[0], status)
            station.sendto(held, source)
            self.assertEqual(target.recvfrom(8192)[0], held)
        finally:
            if suspended:
                ntdll.NtResumeProcess(int(process._handle))
            process.terminate()
            process.wait(timeout=5)
            station.close()
            target.close()

    def test_openvr_basis_and_alignment(self):
        matrix = [[1, 0, 0, 1], [0, 1, 0, 2], [0, 0, 1, -3]]
        position, rotation = bridge.convert_pose(matrix, 0, [0, 0, 0])
        self.assertEqual(position, [300, 100, 200])
        self.assertEqual(rotation, (0, 0, 0, 1))
        position, rotation = bridge.convert_pose(matrix, 90, [10, 20, 30])
        for actual, expected in zip(position, [-90, 320, 230]):
            self.assertAlmostEqual(actual, expected)
        self.assertAlmostEqual(rotation[2], math.sqrt(0.5))
        self.assertAlmostEqual(rotation[3], math.sqrt(0.5))

    def test_alignment_required_for_separate_tracking_spaces(self):
        with self.assertRaises(ValueError):
            bridge.validate_tracker_config({"left_serial": "a", "right_serial": "b"})

    def test_corrupt_tracker_rotation_rejected(self):
        with self.assertRaises(ValueError):
            bridge.convert_pose([[0, 0, 0, 1], [0, 0, 0, 2], [0, 0, 0, 3]], 0, [0, 0, 0])

    def test_measured_alignment_fits_yaw_and_translation(self):
        fit = alignment.fit_alignment([[0, 0, 0], [100, 0, 0], [0, 100, 0]], [[10, 20, 30], [10, 120, 30], [-90, 20, 30]])
        self.assertAlmostEqual(fit["yaw_degrees"], 90)
        for actual, expected in zip(fit["translation_cm"], [10, 20, 30]):
            self.assertAlmostEqual(actual, expected)
        self.assertLess(fit["rms_error_cm"], 1e-8)

    def test_inconsistent_or_short_alignment_rejected(self):
        for source, quest in [([[0, 0, 0], [1, 0, 0]], [[0, 0, 0], [1, 0, 0]]),
                              ([[0, 0, 0], [100, 0, 0]], [[0, 0, 0], [150, 0, 0]])]:
            with self.assertRaises(ValueError):
                alignment.fit_alignment(source, quest)

    def test_real_udp_bridge_connect_and_forward(self):
        # A loopback protocol fixture exercises the actual process and UDP sockets.
        station = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        station.bind(("127.0.0.1", 0))
        target.bind(("127.0.0.1", 0))
        station.settimeout(5)
        target.settimeout(5)
        reserve = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reserve.bind(("127.0.0.1", 0))
        listen_port = reserve.getsockname()[1]
        reserve.close()
        process = subprocess.Popen([sys.executable, str(MODULE_PATH), "--target", "127.0.0.1", "--target-port", str(target.getsockname()[1]), "--station-port", str(station.getsockname()[1]), "--listen-port", str(listen_port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            connect, source = station.recvfrom(8192)
            self.assertEqual(bridge.decode_packet(connect)[0][2], [listen_port])
            status = bridge.encode_message("/DS/HC/Device", 1, "cg2-fixture", 1, 2, "ContactGlove2", 6, 0.0, 1, 80, 0.0, 1, 80, 0.0)
            station.sendto(status, source)
            forwarded_status, _ = target.recvfrom(8192)
            self.assertEqual(forwarded_status, status)
            packet = bridge.encode_message("/DS/HC/Hand", 1, "cg2-fixture", 1, *([0.3] * 20))
            station.sendto(packet, source)
            forwarded, _ = target.recvfrom(8192)
            self.assertEqual(forwarded, packet)
            # A fresh sample remains usable after the old 150 ms status limit.
            time.sleep(.25)
            station.sendto(packet, source)
            self.assertEqual(target.recvfrom(8192)[0], packet)
            # Device selection is pinned after the first valid device; other IDs cannot replace it.
            station.sendto(bridge.encode_message("/DS/HC/Hand", 1, "another-device", 1, *([0.6] * 20)), source)
            target.settimeout(0.1)
            with self.assertRaises(socket.timeout):
                target.recvfrom(8192)
            time.sleep(.8)
            station.sendto(packet, source)
            with self.assertRaises(socket.timeout):
                target.recvfrom(8192)
            station.sendto(status, source)
            self.assertEqual(target.recvfrom(8192)[0], status)
            # A real Station sends these default Hand packets even when disconnected.
            disconnected = bridge.encode_message("/DS/HC/Device", 1, "cg2-fixture", 1, 2, "ContactGlove2", 6, 0.0, 0, 0, 0.0, 0, 0, 0.0)
            station.sendto(disconnected, source)
            station.sendto(bridge.encode_message("/DS/HC/Hand", 1, "cg2-fixture", 1, *([0.0] * 20)), source)
            self.assertEqual(target.recvfrom(8192)[0], disconnected)
            with self.assertRaises(socket.timeout):
                target.recvfrom(8192)
        finally:
            process.terminate()
            process.wait(timeout=5)
            station.close()
            target.close()

    def test_overloaded_station_still_forwards_both_hands_and_disconnect(self):
        # A local flood reproduces the high-rate stream seen from DivingStation.
        # These packets go only to loopback fixture sockets, never to hardware.
        station = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reserve = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for sock in (station, target, reserve):
            sock.bind(("127.0.0.1", 0))
        listen_port = reserve.getsockname()[1]
        reserve.close()
        station.settimeout(5)
        target.settimeout(.1)
        process = subprocess.Popen([sys.executable, str(MODULE_PATH), "--target", "127.0.0.1",
            "--target-port", str(target.getsockname()[1]), "--station-port", str(station.getsockname()[1]),
            "--listen-port", str(listen_port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        stop = threading.Event()
        thread = None
        try:
            _, source = station.recvfrom(8192)
            connected = bridge.encode_message("/DS/HC/Device", 1, "fixture", 1, 2, "ContactGlove2", 6, 0.0, 1, 80, 0.0, 1, 80, 0.0)
            hands = [bridge.encode_message("/DS/HC/Hand", 1, "fixture", side, *([.3] * 20)) for side in (1, 0)]
            def flood():
                while not stop.is_set():
                    for packet in (connected, *hands):
                        station.sendto(packet, source)
            thread = threading.Thread(target=flood, daemon=True)
            thread.start()
            seen = {"device": 0, "left": 0, "right": 0}
            until = time.monotonic() + 1.5
            while time.monotonic() < until:
                try:
                    packet, _ = target.recvfrom(8192)
                except socket.timeout:
                    continue
                for address, _, values in bridge.decode_packet(packet):
                    key = "device" if address == "/DS/HC/Device" else "left" if values[2] == 1 else "right"
                    seen[key] += 1
            self.assertGreaterEqual(min(seen.values()), 5, seen)
            stop.set()
            thread.join(timeout=2)
            disconnected = bridge.encode_message("/DS/HC/Device", 1, "fixture", 1, 2, "ContactGlove2", 6, 0.0, 0, 0, 0.0, 0, 0, 0.0)
            deadline = time.monotonic() + 1.0
            confirmed = False
            while time.monotonic() < deadline and not confirmed:
                station.sendto(disconnected, source)
                try:
                    packet, _ = target.recvfrom(8192)
                    confirmed = packet == disconnected
                except socket.timeout:
                    pass
            self.assertTrue(confirmed, "disconnect status was delayed by the flood")
        finally:
            stop.set()
            if thread:
                thread.join(timeout=2)
            process.terminate()
            process.wait(timeout=5)
            station.close()
            target.close()


if __name__ == "__main__":
    unittest.main()
