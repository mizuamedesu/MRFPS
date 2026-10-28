import io
import os
import pathlib
import random
import struct
import sys
import tempfile
import time
import unittest
import uuid
from unittest.mock import patch

from PIL import Image
import quest_video_bridge as video


def png_image(width=64, height=32, color=(255, 0, 0)):
    image = Image.new("RGB", (width, height), color)
    output = io.BytesIO()
    image.save(output, "PNG")
    image.close()
    return output.getvalue()


class Clock:
    def __init__(self):
        self.value = 0.0

    def now(self):
        return self.value

    def utc(self):
        return 1_700_000_000_000_000_000 + int(self.value * 1_000_000_000)

    def sleep(self, seconds):
        self.value += seconds


class FakeBridge:
    def __init__(self, clock, delay=0.03):
        self.clock, self.delay = clock, delay
        self.frames, self.capture_starts = [], []
        self.cleaned = False

    def prepare(self, timeout):
        assert timeout() > 0

    def capture(self, kind, timeout):
        self.capture_starts.append(self.clock.value)
        self.clock.value += self.delay
        return png_image()

    def publish(self, frame, timeout):
        assert timeout > 0
        self.frames.append(frame)
        self.clock.value += 0.01

    def cleanup(self):
        self.cleaned = True


class CodecTests(unittest.TestCase):
    def test_exact_24_byte_little_endian_header(self):
        jpeg = b"\xff\xd8test\xff\xd9"
        sequence = 0x0102030405060708
        frame = video.pack_frame(640, 360, jpeg, sequence)
        self.assertEqual(video.HEADER.size, 24)
        self.assertEqual(frame[:24], struct.pack("<4sIIIQ", b"MRV1", 640, 360, len(jpeg), sequence))
        self.assertEqual(frame[24:], jpeg)

    def test_header_rejects_zero_sequence_and_oversize(self):
        for sequence in (0, -1, 2**64):
            with self.assertRaises(video.BridgeError):
                video.pack_frame(640, 360, b"\xff\xd8\xff\xd9", sequence)
        with self.assertRaises(video.BridgeError):
            video.pack_frame(640, 360, b"\xff\xd8" + b"x" * video.MAX_JPEG + b"\xff\xd9", 1)
        for sequence in (True, 1.5, "1"):
            with self.assertRaises(video.BridgeError):
                video.pack_frame(640, 360, b"\xff\xd8\xff\xd9", sequence)

    def test_jpeg_dimensions_and_sixty_kib_budget(self):
        pixels = random.Random(7).randbytes(480 * 320 * 3)
        image = Image.frombytes("RGB", (480, 320), pixels)
        output = io.BytesIO()
        image.save(output, "PNG")
        encoded = video.encode_jpeg(output.getvalue(), video.Config(quality=95))
        self.assertLessEqual(len(encoded), 60 * 1024)
        with Image.open(io.BytesIO(encoded)) as decoded:
            self.assertEqual(decoded.size, (640, 360))
            self.assertEqual(decoded.format, "JPEG")

    def test_explicit_eye_selection_no_default_crop(self):
        image = Image.new("RGB", (128, 32), "red")
        image.paste("blue", (64, 0, 128, 32))
        source = io.BytesIO()
        image.save(source, "PNG")
        left = Image.open(io.BytesIO(video.encode_jpeg(source.getvalue(), video.Config(eye="left"))))
        right = Image.open(io.BytesIO(video.encode_jpeg(source.getvalue(), video.Config(eye="right"))))
        both = Image.open(io.BytesIO(video.encode_jpeg(source.getvalue(), video.Config(eye="both"))))
        self.assertGreater(left.getpixel((320, 180))[0], 200)
        self.assertGreater(right.getpixel((320, 180))[2], 200)
        self.assertGreater(both.getpixel((160, 180))[0], 200)
        self.assertGreater(both.getpixel((480, 180))[2], 200)
        left.close(); right.close(); both.close()

    def test_odd_sbs_crop_is_rejected(self):
        with self.assertRaisesRegex(video.BridgeError, "EYE_REQUIRES"):
            video.encode_jpeg(png_image(63, 32), video.Config(eye="left"))

    def test_raw_12_and_16_byte_headers(self):
        pixels = b"\xff\x00\x00\xff" * 8
        for header in (struct.pack("<III", 4, 2, 1), struct.pack("<IIII", 4, 2, 1, 142671872)):
            with video.decode_capture(header + pixels, "raw") as image:
                self.assertEqual(image.size, (4, 2))
                self.assertEqual(image.getpixel((0, 0)), (255, 0, 0, 255))

    def test_raw_length_and_format_are_not_guessed(self):
        for data in (struct.pack("<III", 4, 2, 22) + b"x" * 64, struct.pack("<III", 4, 2, 1) + b"x" * 31):
            with self.assertRaises(video.BridgeError):
                video.decode_capture(data, "raw")

    def test_png_header_limits_before_decode(self):
        forged = b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR" + struct.pack(">II", 100000, 100000) + b"\x08\x06\0\0\0" + b"\0" * 4
        with self.assertRaisesRegex(video.BridgeError, "SOURCE_DIMENSIONS"):
            video.decode_capture(forged, "png")
        with self.assertRaises(video.BridgeError):
            video.decode_capture(b"x" * (video.MAX_PNG + 1), "png")

    def test_empty_truncated_and_non_png(self):
        for data in (b"", b"not a PNG", png_image()[:35]):
            with self.assertRaises(video.BridgeError):
                video.decode_capture(data, "png")

    def test_transparent_pixels_flatten_to_black(self):
        image = Image.new("RGBA", (8, 8), (255, 0, 0, 0))
        stream = io.BytesIO(); image.save(stream, "PNG")
        with Image.open(io.BytesIO(video.encode_jpeg(stream.getvalue(), video.Config()))) as decoded:
            self.assertEqual(decoded.getpixel((320, 180)), (0, 0, 0))


class AdbTests(unittest.TestCase):
    def test_private_atomic_binary_stdin_no_argv_frame(self):
        calls = []
        def fake(command, **kwargs):
            calls.append((command, kwargs))
            if command[-1] == "get-state": return b"device\n"
            if command[-1] == "pwd": return b"/data/user/0/org.mrfps.arena\n"
            return b""
        bridge = video.AdbBridge("adb.exe", "unit-device", runner=fake)
        bridge.prepare(lambda: 1)
        frame = video.pack_frame(640, 360, b"\xff\xd8synthetic-image\xff\xd9", 123)
        bridge.publish(frame, 1)
        command, parameters = calls[-1]
        self.assertEqual(parameters["input_bytes"], frame)
        self.assertNotIn("synthetic-image", repr(command))
        self.assertIn("umask 077", command[-1]); self.assertIn("chmod 600", command[-1]); self.assertIn(" && mv ", command[-1])
        self.assertIn("chmod 700 files/MRFPS files/MRFPS/Video", calls[2][0][-1])
        bridge.cleanup()
        self.assertIn(bridge.temporary, calls[-1][0][-1])
        self.assertNotIn("frame.bin", calls[-1][0][-1])

    def test_run_as_root_strict_and_authorization(self):
        for state, root in ((b"unauthorized", b"/data/user/0/org.mrfps.arena"),
                            (b"device", b"/sdcard/Android/data/org.mrfps.arena"),
                            (b"device", b"/data/user/0/org.mrfps.arena/../other")):
            def fake(command, **_): return state if command[-1] == "get-state" else root
            with self.assertRaises(video.BridgeError):
                video.AdbBridge("adb", "device", runner=fake).prepare(lambda: 1)

    def test_no_shell_parameter_injection(self):
        for serial, package in (("device;echo bad", "org.mrfps.arena"), ("device", "org.mrfps;echo")):
            with self.assertRaises(video.BridgeError):
                video.AdbBridge("adb", serial, package)

    def test_capture_commands_and_bounds(self):
        calls = []
        def fake(command, **kwargs): calls.append((command, kwargs)); return b"synthetic"
        bridge = video.AdbBridge("adb", "serial", runner=fake)
        bridge.capture("png", 0.5); bridge.capture("raw", 0.5)
        self.assertEqual(calls[0][0][-3:], ["exec-out", "screencap", "-p"])
        self.assertEqual(calls[1][0][-2:], ["exec-out", "screencap"])
        self.assertEqual(calls[0][1]["max_stdout"], video.MAX_PNG)
        self.assertEqual(calls[1][1]["max_stdout"], video.MAX_RAW)

    def test_subprocess_output_limit_and_stderr_not_exposed(self):
        with self.assertRaises(video.BridgeError) as caught:
            video.run_bounded([sys.executable, "-c", "import sys;sys.stderr.write('synthetic-secret');sys.stdout.write('x'*4096)"], max_stdout=32, timeout=2)
        self.assertNotIn("synthetic-secret", str(caught.exception))
        self.assertEqual(str(caught.exception), "ADB_OUTPUT_LIMIT")

    def test_subprocess_timeout_is_bounded(self):
        start = time.monotonic()
        with self.assertRaisesRegex(video.BridgeError, "ADB_TIMEOUT"):
            video.run_bounded([sys.executable, "-c", "import time;time.sleep(3)"], timeout=0.05)
        self.assertLess(time.monotonic() - start, 2)

    def test_subprocess_binary_stdin(self):
        content = b"\0\xffsynthetic\n\r"
        output = video.run_bounded([sys.executable, "-c", "import sys;sys.stdout.buffer.write(sys.stdin.buffer.read())"], input_bytes=content, timeout=2)
        self.assertEqual(output, content)

    def test_single_instance_lock_and_release(self):
        serial = "unit-" + uuid.uuid4().hex
        lock = video.InstanceLock(serial, "org.mrfps.arena")
        with lock:
            with self.assertRaisesRegex(video.BridgeError, "INSTANCE_ALREADY_RUNNING"):
                with video.InstanceLock(serial, "org.mrfps.arena"): pass
        with video.InstanceLock(serial, "org.mrfps.arena"): pass
        lock.path.unlink()


class ScheduleTests(unittest.TestCase):
    def test_duration_rate_and_capture_start_epoch(self):
        clock = Clock(); bridge = FakeBridge(clock)
        result = video.run_bridge(bridge, video.Config(duration=0.7), clock=clock.now, utc_ns=clock.utc, sleep=clock.sleep)
        self.assertEqual(result["published"], 4)
        sequences = [video.HEADER.unpack_from(frame)[4] for frame in bridge.frames]
        self.assertEqual(sequences, [1_700_000_000_000_000_000 + int(value * 1e9) for value in bridge.capture_starts])
        self.assertTrue(all(b > a for a, b in zip(sequences, sequences[1:])))
        self.assertTrue(bridge.cleaned)

    def test_late_frames_are_dropped_not_refreshed(self):
        clock = Clock(); bridge = FakeBridge(clock, delay=0.4)
        result = video.run_bridge(bridge, video.Config(duration=0.9, max_frame_age=0.3), clock=clock.now, utc_ns=clock.utc, sleep=clock.sleep)
        self.assertGreater(result["dropped_stale"], 0)
        self.assertEqual(bridge.frames, [])
        self.assertGreater(result["dropped_schedule_slots"], 0)

    def test_encoder_latency_also_drops(self):
        clock = Clock(); bridge = FakeBridge(clock)
        encoder = video.encode_jpeg
        def slow(data, config): clock.value += 0.5; return encoder(data, config)
        with patch.object(video, "encode_jpeg", slow):
            result = video.run_bridge(bridge, video.Config(duration=0.8, max_frame_age=0.3), clock=clock.now, utc_ns=clock.utc, sleep=clock.sleep)
        self.assertEqual(result["published"], 0)

    def test_clock_rollback_is_not_hidden(self):
        clock = Clock(); bridge = FakeBridge(clock)
        ticks = iter((100, 100, 99, 101))
        result = video.run_bridge(bridge, video.Config(duration=0.7), clock=clock.now, utc_ns=lambda: next(ticks), sleep=clock.sleep)
        self.assertEqual(result["dropped_clock"], 2)
        self.assertEqual([video.HEADER.unpack_from(frame)[4] for frame in bridge.frames], [100, 101])

    def test_invalid_configuration_before_adb(self):
        for config in (video.Config(fps=6), video.Config(width=0), video.Config(quality=100), video.Config(max_frame_age=3)):
            with self.assertRaises(video.BridgeError): config.validate()

    def test_consecutive_failure_keeps_safe_counters_and_cleanup(self):
        clock = Clock(); bridge = FakeBridge(clock)
        bridge.capture = lambda *_: b""
        with self.assertRaises(video.BridgeError) as caught:
            video.run_bridge(bridge, video.Config(duration=2), clock=clock.now, utc_ns=clock.utc, sleep=clock.sleep)
        self.assertEqual(caught.exception.stats["errors"], {"EMPTY_CAPTURE": 3})
        self.assertTrue(bridge.cleaned)


class HostFrameTests(unittest.TestCase):
    def test_host_mode_captures_without_any_android_file_calls(self):
        clock, calls, starts = Clock(), [], []
        def fake(command, **kwargs):
            calls.append((command, kwargs))
            if command[-1] == "get-state":
                return b"device\n"
            self.assertEqual(command[-3:], ["exec-out", "screencap", "-p"])
            starts.append(clock.utc())
            clock.value += 0.02
            return png_image()
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "host frames" / "frame.bin"
            bridge = video.HostFrameBridge("adb", "unit-device", target, runner=fake, clock=clock.now)
            result = video.run_bridge(bridge, video.Config(duration=0.45), clock=clock.now,
                                      utc_ns=clock.utc, sleep=clock.sleep)
            frame = target.read_bytes()
            self.assertEqual(video.HEADER.unpack_from(frame), (b"MRV1", 640, 360, len(frame)-24, starts[-1]))
            with Image.open(io.BytesIO(frame[24:])) as decoded:
                self.assertEqual(decoded.size, (640, 360))
            self.assertEqual(result["published"], 3)
            self.assertEqual(result["publication_target"], "host_frame_file")
            self.assertTrue(result["host_images_persisted"])
            self.assertEqual(list(target.parent.iterdir()), [target])
        self.assertTrue(all("input_bytes" not in kwargs for _, kwargs in calls))
        self.assertTrue(all("shell" not in command and "push" not in command for command, _ in calls))
        self.assertTrue(all(command[:3] == ["adb", "-s", "unit-device"] for command, _ in calls))

    def test_atomic_replace_keeps_old_frame_until_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "frame.bin"
            target.write_bytes(b"previous")
            bridge = video.HostFrameBridge("adb", "unit-device", target)
            frame = video.pack_frame(640, 360, b"\xff\xd8synthetic\xff\xd9", 123)
            replace = os.replace
            def inspect_replace(source, destination):
                self.assertEqual(target.read_bytes(), b"previous")
                self.assertEqual(pathlib.Path(source).parent, target.parent)
                self.assertEqual(pathlib.Path(source).read_bytes(), frame)
                return replace(source, destination)
            with patch.object(video.os, "replace", inspect_replace):
                bridge.publish(frame, 1)
            self.assertEqual(target.read_bytes(), frame)
            self.assertEqual(list(target.parent.iterdir()), [target])

    def test_failed_replace_preserves_previous_and_removes_own_temp(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "frame.bin"
            target.write_bytes(b"previous")
            bridge = video.HostFrameBridge("adb", "unit-device", target)
            with patch.object(video.os, "replace", side_effect=PermissionError("synthetic-only")):
                with self.assertRaisesRegex(video.BridgeError, "^HOST_FRAME_WRITE$"):
                    bridge.publish(b"synthetic", 1)
            self.assertEqual(target.read_bytes(), b"previous")
            self.assertEqual(list(target.parent.iterdir()), [target])
            self.assertFalse(bridge.host_images_persisted)

    def test_slow_disk_drops_without_publishing_or_refreshing_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = Clock()
            target = pathlib.Path(directory) / "frame.bin"
            target.write_bytes(b"previous")
            bridge = video.HostFrameBridge("adb", "unit-device", target, clock=clock.now)
            frame = video.pack_frame(640, 360, b"\xff\xd8synthetic\xff\xd9", clock.utc())
            def slow_fsync(_):
                clock.value += 0.6
            with patch.object(video.os, "fsync", slow_fsync):
                with self.assertRaisesRegex(video.BridgeError, "^HOST_FRAME_STALE$"):
                    bridge.publish(frame, 0.5)
            self.assertEqual(target.read_bytes(), b"previous")
            self.assertEqual(list(target.parent.iterdir()), [target])

    def test_stale_capture_never_creates_host_frame(self):
        clock, calls = Clock(), []
        def fake(command, **kwargs):
            calls.append(command)
            if command[-1] == "get-state":
                return b"device"
            clock.value += 0.4
            return png_image()
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "frame.bin"
            bridge = video.HostFrameBridge("adb", "unit-device", target, runner=fake, clock=clock.now)
            result = video.run_bridge(bridge, video.Config(duration=0.9, max_frame_age=0.3),
                                      clock=clock.now, utc_ns=clock.utc, sleep=clock.sleep)
            self.assertGreater(result["dropped_stale"], 0)
            self.assertFalse(target.exists())
            self.assertFalse(result["host_images_persisted"])
        self.assertTrue(all("shell" not in command for command in calls))

    def test_requires_absolute_regular_host_target_before_adb(self):
        with self.assertRaisesRegex(video.BridgeError, "ABSOLUTE_PATH"):
            video.HostFrameBridge("adb", "unit-device", "relative/frame.bin")
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(video.BridgeError, "NOT_REGULAR"):
                video.HostFrameBridge("adb", "unit-device", directory)


if __name__ == "__main__":
    unittest.main()
