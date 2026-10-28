#!/usr/bin/env python3
"""Mirror MoBits discovery and reverse its loopback TCP port over authorized ADB.

No LAN listener, protocol translation, automatic ARM, or EMS waveform generation.
The MoBits app still owns calibration, permission, STOP and device acknowledgments.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


class BridgeError(RuntimeError):
    pass


def validate_discovery(raw: bytes) -> int:
    """Return the sole loopback port; diagnostics deliberately omit input data."""
    if not 0 < len(raw) <= 4096:
        raise BridgeError("MoBits discovery size must be 1..4096 bytes.")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, ValueError):
        raise BridgeError("MoBits discovery is not valid UTF-8 JSON.") from None
    if not isinstance(value, dict):
        raise BridgeError("MoBits discovery must be an object.")
    if type(value.get("discovery_version")) is not int or value["discovery_version"] != 1:
        raise BridgeError("Unsupported MoBits discovery version.")
    if type(value.get("protocol_version")) is not int or value["protocol_version"] != 2:
        raise BridgeError("MoBits protocol v2 is required.")
    if not isinstance(value.get("app_version"), str) or not value["app_version"]:
        raise BridgeError("MoBits app version is missing.")
    if not isinstance(value.get("token"), str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value["token"]):
        raise BridgeError("MoBits token format is invalid.")
    if not isinstance(value.get("session_id"), str) or not re.fullmatch(
        r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", value["session_id"]
    ):
        raise BridgeError("MoBits session ID format is invalid.")
    ports = []
    for channel in ("control", "motion"):
        url = value.get(channel + "_url")
        match = re.fullmatch(r"ws://127\.0\.0\.1:([1-9][0-9]{0,4})/mobits/v2/" + channel, url) if isinstance(url, str) else None
        if not match or not 1 <= int(match[1]) <= 65535:
            raise BridgeError("MoBits requires exact authenticated IPv4 loopback endpoints.")
        ports.append(int(match[1]))
    if ports[0] != ports[1]:
        raise BridgeError("MoBits channel ports must match.")
    return ports[0]


class QuestBridge:
    def __init__(self, adb: str, package: str, serial: str | None = None):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", package):
            raise BridgeError("Android package name is invalid.")
        self.package = package
        self.command = [adb] + (["-s", serial] if serial else [])
        self.directory: str | None = None
        self.port: int | None = None
        self.digest: bytes | None = None

    def run(self, *arguments: str, input_data: bytes | None = None, capture: bool = False) -> bytes:
        try:
            options = dict(check=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=15)
            if input_data is None:
                options["stdin"] = subprocess.DEVNULL
            else:
                options["input"] = input_data
            result = subprocess.run(self.command + list(arguments), **options)
            return result.stdout if capture else b""
        except (OSError, subprocess.SubprocessError):
            raise BridgeError("ADB operation failed; check the selected authorized device and installed development APK.") from None

    def private_directory(self) -> str:
        raw = self.run("shell", "run-as", self.package, "pwd", capture=True)
        try:
            root = raw.decode("ascii").strip() if len(raw) <= 512 else ""
        except UnicodeError:
            root = ""
        if not re.fullmatch(r"/data/(?:user/[0-9]+|data)/" + re.escape(self.package), root):
            raise BridgeError("Cannot locate the installed development APK's app-private directory using run-as.")
        return root + "/files/MoBits/SDK"

    def install(self, raw: bytes) -> bool:
        port = validate_discovery(raw)
        digest = hashlib.sha256(raw).digest()
        if digest == self.digest:
            return False
        directory = self.private_directory()
        self.run("reverse", f"tcp:{port}", f"tcp:{port}")
        # run-as starts in the validated app-private root. Bytes travel on stdin,
        # not argv, environment, a shared host tempfile, or external storage.
        # -T disables a remote PTY; the same-filesystem rename publishes atomically.
        script = (f"run-as {self.package} sh -c 'umask 077; "
                  "mkdir -p files/MoBits/SDK && chmod 700 files/MoBits files/MoBits/SDK && "
                  "cat > files/MoBits/SDK/connection-v2.json.incoming && "
                  "chmod 600 files/MoBits/SDK/connection-v2.json.incoming && "
                  "mv -f files/MoBits/SDK/connection-v2.json.incoming files/MoBits/SDK/connection-v2.json'")
        try:
            self.run("shell", "-T", script, input_data=raw)
        except BridgeError:
            if self.port != port:
                self.run("reverse", "--remove", f"tcp:{port}")
            raise
        old_port = self.port
        self.directory = directory
        self.port = port
        self.digest = digest
        if old_port is not None and old_port != port:
            self.run("reverse", "--remove", f"tcp:{old_port}")
        return True

    def disconnect(self) -> None:
        if self.port is not None:
            self.run("reverse", "--remove", f"tcp:{self.port}")
            self.port = None
            self.digest = None

    def refresh_if_transport_lost(self) -> None:
        """ADB drops reverse mappings when USB/Wi-Fi disconnects; re-add on recovery."""
        if self.port is None:
            return
        try:
            result = subprocess.run(self.command + ["reverse", "--list"], check=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        except (OSError, subprocess.SubprocessError):
            self.digest = None
            raise BridgeError("The selected Quest ADB connection is unavailable.") from None
        expected = f"tcp:{self.port} tcp:{self.port}".encode()
        if not any(line.endswith(expected) for line in result.stdout.splitlines()):
            self.digest = None


def find_adb() -> str:
    found = shutil.which("adb")
    if found:
        return found
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        root = os.environ.get(variable)
        if root:
            path = Path(root) / "platform-tools" / ("adb.exe" if os.name == "nt" else "adb")
            if path.is_file():
                return str(path)
    raise BridgeError("ADB was not found. Pass --adb or set ANDROID_HOME.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", default="org.mrfps.arena")
    parser.add_argument("--serial", help="authorized ADB device serial, required when multiple devices are attached")
    parser.add_argument("--adb", help="path to the Android SDK adb executable")
    parser.add_argument("--session", type=Path, default=Path(os.environ.get("LOCALAPPDATA", "")) / "MoBits/SDK/connection-v2.json")
    parser.add_argument("--watch", action="store_true", help="mirror rotated desktop sessions; Ctrl+C disconnects the owned reverse")
    args = parser.parse_args()
    bridge = None
    try:
        bridge = QuestBridge(args.adb or find_adb(), args.package, args.serial)
        waiting = False
        next_health_check = 0.0
        while True:
            try:
                if args.watch and time.monotonic() >= next_health_check:
                    bridge.refresh_if_transport_lost()
                    next_health_check = time.monotonic() + 2.0
                raw = args.session.read_bytes()
                if bridge.install(raw):
                    print(f"MoBits discovery mirrored; authenticated loopback port {bridge.port} reversed. Haptics remain unarmed.", flush=True)
                waiting = False
            except (OSError, BridgeError) as error:
                if not args.watch:
                    if isinstance(error, BridgeError):
                        raise
                    raise BridgeError("MoBits discovery is missing; open the app's Unity / UE5 SDK session.") from None
                if not waiting:
                    print("Waiting for a valid MoBits app session and an authorized Quest ADB connection.", flush=True)
                    waiting = True
            if not args.watch:
                return 0
            time.sleep(0.25)
    except KeyboardInterrupt:
        return 0
    except BridgeError as error:
        print(str(error))
        return 1
    finally:
        if args.watch and bridge is not None:
            try:
                bridge.disconnect()
            except BridgeError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
