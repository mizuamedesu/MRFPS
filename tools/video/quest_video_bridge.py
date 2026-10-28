"""Bounded, authenticated-ADB display capture -> MRV1/JPEG frames.

No app is launched. Captured pixels, including system overlays, are not retouched.
Default output is app-private. --host-frame-file opts into one atomic host frame
instead, without Android file writes. Console output contains counters only.
"""
import argparse
import contextlib
import io
import json
import os
import pathlib
import re
import struct
import subprocess
import tempfile
import threading
import time
import uuid
import warnings
import hashlib
from dataclasses import dataclass

from PIL import Image, ImageOps

HEADER = struct.Struct("<4sIIIQ")
MAX_JPEG = 60 * 1024
MAX_PIXELS = 16_000_000
MAX_DIMENSION = 8192
MAX_PNG = 24 * 1024 * 1024
MAX_RAW = 64 * 1024 * 1024
Image.MAX_IMAGE_PIXELS = MAX_PIXELS


class BridgeError(Exception):
    """Only fixed codes are exposed; underlying stderr/pixels are discarded."""


@dataclass
class Config:
    width: int = 640
    height: int = 360
    fps: float = 5
    quality: int = 75
    eye: str = "both"
    capture_format: str = "png"
    duration: float | None = 15
    timeout: float = 1.5
    max_frame_age: float = 1.5
    max_errors: int = 3

    def validate(self):
        if not (1 <= self.width <= 1280 and 1 <= self.height <= 720):
            raise BridgeError("OUTPUT_DIMENSIONS")
        if not (0 < self.fps <= 5 and 1 <= self.quality <= 95):
            raise BridgeError("RATE_OR_QUALITY")
        if self.eye not in ("both", "left", "right") or self.capture_format not in ("png", "raw"):
            raise BridgeError("CAPTURE_CONFIGURATION")
        if self.duration is not None and not (0 < self.duration <= 3600):
            raise BridgeError("DURATION")
        if not (0 < self.timeout <= 3 and 0 < self.max_frame_age <= 2 and 1 <= self.max_errors <= 10):
            raise BridgeError("TIME_BUDGET")


def dimensions(width, height):
    if not (0 < width <= MAX_DIMENSION and 0 < height <= MAX_DIMENSION and width * height <= MAX_PIXELS):
        raise BridgeError("SOURCE_DIMENSIONS")


def decode_capture(data, kind):
    if not data:
        raise BridgeError("EMPTY_CAPTURE")
    if kind == "raw":
        if len(data) < 12 or len(data) > MAX_RAW:
            raise BridgeError("RAW_SIZE")
        width, height, pixel_format = struct.unpack_from("<III", data)
        dimensions(width, height)
        formats = {1: ("RGBA", 4), 2: ("RGBX", 4), 3: ("RGB", 3)}
        if pixel_format not in formats:
            raise BridgeError("RAW_PIXEL_FORMAT")
        mode, bpp = formats[pixel_format]
        header_length = len(data) - width * height * bpp
        if header_length not in (12, 16):
            raise BridgeError("RAW_HEADER_OR_LENGTH")
        return Image.frombytes(mode, (width, height), data[header_length:])
    if kind != "png" or len(data) > MAX_PNG or len(data) < 33:
        raise BridgeError("PNG_SIZE")
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[8:16] != b"\0\0\0\rIHDR":
        raise BridgeError("PNG_HEADER")
    dimensions(*struct.unpack_from(">II", data, 16))
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            image = Image.open(io.BytesIO(data))
            dimensions(*image.size)
            if image.format != "PNG" or getattr(image, "n_frames", 1) != 1:
                image.close()
                raise BridgeError("PNG_FORMAT")
            image.load()
            return image
    except BridgeError:
        raise
    except Exception:
        raise BridgeError("PNG_DECODE") from None


def encode_jpeg(data, config):
    config.validate()
    image = decode_capture(data, config.capture_format)
    try:
        if config.eye != "both":
            # Explicit side-by-side eye selection only. Never infer stereo or
            # remove overlays/crop automatically when the default is "both".
            if image.width % 2:
                raise BridgeError("EYE_REQUIRES_EVEN_SBS_WIDTH")
            half = image.width // 2
            box = (0, 0, half, image.height) if config.eye == "left" else (half, 0, image.width, image.height)
            cropped = image.crop(box)
            image.close()
            image = cropped
        if "A" in image.getbands() or "transparency" in image.info:
            rgba = image.convert("RGBA")
            rgb = Image.new("RGB", image.size, (0, 0, 0))
            rgb.paste(rgba, mask=rgba.getchannel("A"))
            rgba.close()
        else:
            rgb = image.convert("RGB")
        try:
            fitted = ImageOps.contain(rgb, (config.width, config.height), Image.Resampling.LANCZOS)
            canvas = Image.new("RGB", (config.width, config.height), (0, 0, 0))
            canvas.paste(fitted, ((config.width - fitted.width) // 2, (config.height - fitted.height) // 2))
            fitted.close()
            try:
                qualities = list(range(config.quality, 0, -10))
                if qualities[-1] != 1:
                    qualities.append(1)
                for quality in qualities:
                    output = io.BytesIO()
                    canvas.save(output, "JPEG", quality=quality, subsampling=2, optimize=True,
                                progressive=False, exif=b"", icc_profile=None)
                    encoded = output.getvalue()
                    if len(encoded) <= MAX_JPEG:
                        return encoded
                raise BridgeError("JPEG_OVERSIZE")
            finally:
                canvas.close()
        finally:
            rgb.close()
    finally:
        image.close()


def pack_frame(width, height, jpeg, sequence):
    if any(type(value) is not int for value in (width, height, sequence)):
        raise BridgeError("FRAME_HEADER_TYPE")
    if not (1 <= width <= 1280 and 1 <= height <= 720 and 0 < sequence < 2**64):
        raise BridgeError("FRAME_HEADER")
    if not (4 <= len(jpeg) <= MAX_JPEG and jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")):
        raise BridgeError("FRAME_JPEG")
    return HEADER.pack(b"MRV1", width, height, len(jpeg), sequence) + jpeg


def run_bounded(command, *, input_bytes=None, timeout=1.5, max_stdout=4096):
    """Bounded pipes, no shell, no stderr retention, own process handle only."""
    process = subprocess.Popen(command, stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    output, faults = bytearray(), []

    def stop():
        with contextlib.suppress(OSError):
            if process.poll() is None:
                process.kill()

    def read_stdout():
        try:
            while True:
                block = process.stdout.read(65536)
                if not block:
                    return
                if len(output) + len(block) > max_stdout:
                    faults.append("ADB_OUTPUT_LIMIT")
                    stop()
                    return
                output.extend(block)
        except OSError:
            faults.append("ADB_PIPE")

    def discard_stderr():
        with contextlib.suppress(OSError):
            while process.stderr.read(65536):
                pass

    def write_stdin():
        try:
            process.stdin.write(input_bytes)
            process.stdin.close()
        except (OSError, BrokenPipeError):
            faults.append("ADB_STDIN")

    threads = [threading.Thread(target=read_stdout, daemon=True), threading.Thread(target=discard_stderr, daemon=True)]
    if input_bytes is not None:
        threads.append(threading.Thread(target=write_stdin, daemon=True))
    for thread in threads:
        thread.start()
    try:
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            stop()
            process.wait(timeout=2)
            raise BridgeError("ADB_TIMEOUT") from None
        for thread in threads:
            thread.join(timeout=1)
        if any(thread.is_alive() for thread in threads):
            raise BridgeError("ADB_PIPE_TIMEOUT")
        if faults:
            raise BridgeError(faults[0])
        if process.returncode:
            raise BridgeError("ADB_EXIT_NONZERO")
        return bytes(output)
    finally:
        stop()
        for pipe in (process.stdin, process.stdout, process.stderr):
            if pipe:
                with contextlib.suppress(OSError):
                    pipe.close()


class AdbBridge:
    publication_target = "android_private"
    host_images_persisted = False

    def __init__(self, adb, serial, package="org.mrfps.arena", runner=run_bounded):
        if not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", serial):
            raise BridgeError("DEVICE_SERIAL")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", package):
            raise BridgeError("PACKAGE")
        self.prefix, self.serial, self.package, self.runner = [str(adb), "-s", serial], serial, package, runner
        self.temporary = "files/MRFPS/Video/.frame-" + uuid.uuid4().hex + ".incoming"
        self.prepared = False

    def call(self, arguments, **kwargs):
        return self.runner(self.prefix + arguments, **kwargs)

    def prepare(self, timeout):
        if self.call(["get-state"], timeout=timeout(), max_stdout=1024).strip() != b"device":
            raise BridgeError("DEVICE_NOT_AUTHORIZED")
        root = self.call(["shell", "run-as", self.package, "pwd"], timeout=timeout(), max_stdout=1024).strip()
        if not re.fullmatch(rb"/data/(?:user/[0-9]+|data)/" + re.escape(self.package.encode()), root):
            raise BridgeError("APP_PRIVATE_ROOT")
        command = "run-as " + self.package + " sh -c 'umask 077; mkdir -p files/MRFPS/Video && chmod 700 files/MRFPS files/MRFPS/Video'"
        self.call(["shell", command], timeout=timeout(), max_stdout=1024)
        self.prepared = True

    def capture(self, kind, timeout):
        arguments = ["exec-out", "screencap"] + (["-p"] if kind == "png" else [])
        return self.call(arguments, timeout=timeout, max_stdout=MAX_PNG if kind == "png" else MAX_RAW)

    def publish(self, frame, timeout):
        command = "run-as " + self.package + " sh -c 'umask 077; cat > " + self.temporary + " && chmod 600 " + self.temporary + " && mv " + self.temporary + " files/MRFPS/Video/frame.bin'"
        self.call(["shell", command], input_bytes=frame, timeout=timeout, max_stdout=1024)

    def cleanup(self):
        if self.prepared:
            # Remove only this producer's incomplete temporary file. The last
            # encoded frame is left for the native UTC-age rejection policy.
            with contextlib.suppress(BridgeError):
                self.call(["shell", "run-as " + self.package + " rm -f " + self.temporary], timeout=1, max_stdout=1024)


class HostFrameBridge(AdbBridge):
    """Read-only ADB capture, publishing only to an explicitly selected host path."""

    publication_target = "host_frame_file"

    def __init__(self, adb, serial, host_frame_file, package="org.mrfps.arena", runner=run_bounded,
                 *, clock=time.monotonic):
        super().__init__(adb, serial, package, runner)
        self.target = pathlib.Path(host_frame_file)
        if not self.target.is_absolute() or not self.target.name:
            raise BridgeError("HOST_FRAME_ABSOLUTE_PATH_REQUIRED")
        if self.target.is_symlink() or (self.target.exists() and not self.target.is_file()):
            raise BridgeError("HOST_FRAME_NOT_REGULAR")
        self.clock = clock
        self.host_images_persisted = False

    def prepare(self, timeout):
        # Do not call the Android-private prepare/cleanup in this mode. Even an
        # installed/development APK is unnecessary for read-only screencap.
        if self.call(["get-state"], timeout=timeout(), max_stdout=1024).strip() != b"device":
            raise BridgeError("DEVICE_NOT_AUTHORIZED")
        try:
            self.target.parent.mkdir(parents=True, exist_ok=True)
        except OSError:
            raise BridgeError("HOST_FRAME_DIRECTORY") from None

    def publish(self, frame, timeout):
        deadline = self.clock() + timeout
        temporary = None
        try:
            fd, temporary = tempfile.mkstemp(prefix="." + self.target.name + "-", suffix=".incoming",
                                             dir=self.target.parent)
            with os.fdopen(fd, "wb") as output:
                output.write(frame)
                output.flush()
                os.fsync(output.fileno())
            if self.clock() >= deadline:
                raise BridgeError("HOST_FRAME_STALE")
            os.replace(temporary, self.target)
            self.host_images_persisted = True
        except BridgeError:
            raise
        except OSError:
            raise BridgeError("HOST_FRAME_WRITE") from None
        finally:
            if temporary is not None:
                with contextlib.suppress(OSError):
                    os.unlink(temporary)

    def cleanup(self):
        # The atomic final frame is intentionally retained. Its original UTC
        # capture-start sequence allows the native receiver to reject it at2s.
        pass


class InstanceLock:
    def __init__(self, serial, package):
        digest = hashlib.sha256((serial + "\0" + package).encode()).hexdigest()
        self.path = pathlib.Path(tempfile.gettempdir()) / ("mrfps-quest-video-" + digest + ".lock")
        self.file = None

    def __enter__(self):
        self.file = open(self.path, "a+b")
        self.file.seek(0, 2)
        if self.file.tell() == 0:
            self.file.write(b"0")
            self.file.flush()
        self.file.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.file.close()
            raise BridgeError("INSTANCE_ALREADY_RUNNING") from None
        return self

    def __exit__(self, *_):
        self.file.close()  # OS releases the lock, also after process death.


def run_bridge(adb, config, *, clock=time.monotonic, utc_ns=time.time_ns, sleep=time.sleep):
    config.validate()
    start = clock()
    deadline = start + config.duration if config.duration is not None else float("inf")
    stats = {"captured": 0, "published": 0, "dropped_stale": 0, "dropped_clock": 0,
             "dropped_schedule_slots": 0, "errors": {}, "maximum_jpeg_bytes": 0,
             "maximum_capture_seconds": 0, "source_eye": config.eye, "capture_format": config.capture_format,
             "publication_target": getattr(adb, "publication_target", "android_private"),
             "host_images_persisted": False, "visual_acceptance_verified": False}

    def remaining():
        value = min(config.timeout, deadline - clock())
        if value <= 0:
            raise BridgeError("DURATION_FINISHED")
        return value

    last_sequence, next_tick, errors = 0, start, 0
    try:
        adb.prepare(remaining)
        while clock() < deadline:
            if clock() < next_tick:
                sleep(min(next_tick - clock(), deadline - clock()))
            if clock() >= deadline:
                break
            capture_start, sequence = clock(), utc_ns()  # Before starting screencap.
            if sequence <= last_sequence or not (0 < sequence < 2**64):
                stats["dropped_clock"] += 1
            else:
                last_sequence = sequence
                try:
                    pixels = adb.capture(config.capture_format, min(remaining(), config.max_frame_age))
                    stats["maximum_capture_seconds"] = max(stats["maximum_capture_seconds"], clock() - capture_start)
                    stats["captured"] += 1
                    if clock() - capture_start >= config.max_frame_age:
                        stats["dropped_stale"] += 1
                    else:
                        jpeg = encode_jpeg(pixels, config)
                        del pixels
                        if clock() - capture_start >= config.max_frame_age or clock() >= deadline:
                            stats["dropped_stale"] += 1
                        else:
                            frame = pack_frame(config.width, config.height, jpeg, sequence)
                            budget = min(remaining(), config.max_frame_age - (clock() - capture_start))
                            adb.publish(frame, budget)
                            stats["published"] += 1
                            stats["host_images_persisted"] = bool(getattr(adb, "host_images_persisted", False))
                            stats["maximum_jpeg_bytes"] = max(stats["maximum_jpeg_bytes"], len(jpeg))
                            errors = 0
                except BridgeError as error:
                    if str(error) == "DURATION_FINISHED":
                        break
                    if str(error) == "HOST_FRAME_STALE":
                        stats["dropped_stale"] += 1
                        next_tick = clock() + 1 / config.fps
                        continue
                    stats["errors"][str(error)] = stats["errors"].get(str(error), 0) + 1
                    errors += 1
                    if errors >= config.max_errors:
                        failure = BridgeError("CONSECUTIVE_FRAME_FAILURES")
                        failure.stats = stats
                        raise failure from None
            next_tick += 1 / config.fps
            if next_tick < clock():
                stats["dropped_schedule_slots"] += int((clock() - next_tick) * config.fps) + 1
                next_tick = clock() + 1 / config.fps
    finally:
        adb.cleanup()
    stats["duration_seconds"] = clock() - start
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--package", default="org.mrfps.arena")
    parser.add_argument("--host-frame-file", help="Explicit absolute host MRV1 path; atomic replace, no Android frame/file writes")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--duration", type=float, help="Bounded seconds, default15, maximum3600")
    mode.add_argument("--watch", action="store_true", help="Explicit continuous mode; Ctrl+C stops the producer")
    parser.add_argument("--fps", type=float, default=5)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--quality", type=int, default=75)
    parser.add_argument("--eye", choices=("both", "left", "right"), default="both", help="left/right explicitly select halves of side-by-side input; both preserves all pixels")
    parser.add_argument("--capture-format", choices=("png", "raw"), default="png", help="Raw accepts exact12/16-byte Android headers and RGB(A/X)8888 only; device support must be measured")
    parser.add_argument("--timeout", type=float, default=1.5)
    parser.add_argument("--max-frame-age", type=float, default=1.5, help="Maximum capture-to-publish budget; at most2s, sequence is never refreshed after capture")
    args = parser.parse_args()
    config = Config(width=args.width, height=args.height, fps=args.fps, quality=args.quality, eye=args.eye,
                    capture_format=args.capture_format, duration=None if args.watch else (args.duration if args.duration is not None else 15),
                    timeout=args.timeout, max_frame_age=args.max_frame_age)
    bridge = None
    publication_target = "host_frame_file" if args.host_frame_file else "android_private"
    try:
        config.validate()
        bridge = (HostFrameBridge(args.adb, args.serial, args.host_frame_file, args.package)
                  if args.host_frame_file else AdbBridge(args.adb, args.serial, args.package))
        with InstanceLock(args.serial, args.package):
            stats = run_bridge(bridge, config)
        print(json.dumps({"status": "completed" if stats["published"] else "no_frames", **stats}))
        return 0 if stats["published"] else 1
    except KeyboardInterrupt:
        print(json.dumps({"status": "stopped", "publication_target": publication_target,
                          "host_images_persisted": bool(bridge and bridge.host_images_persisted)}))
        return 0
    except BridgeError as error:
        print(json.dumps({"status": "failed", "code": str(error), "stats": getattr(error, "stats", None),
                          "publication_target": publication_target,
                          "host_images_persisted": bool(bridge and bridge.host_images_persisted)}))
        return 1
    except Exception:
        print(json.dumps({"status": "failed", "code": "UNEXPECTED_ERROR", "publication_target": publication_target,
                          "host_images_persisted": bool(bridge and bridge.host_images_persisted)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
