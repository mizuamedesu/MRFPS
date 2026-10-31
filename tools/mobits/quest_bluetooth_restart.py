#!/usr/bin/env python3
"""Restart Quest Bluetooth once to clear a confirmed orphaned pairing state.

The operation is receipt-backed and does not act during system confirmation
dialogs or while an application reports an open MoBits session.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

import quest_bond_recovery as bond

ROOT = bond.ROOT


def optional_pid(args: argparse.Namespace, package: str) -> bool:
    process = subprocess.run([str(args.adb), "-s", args.serial, "shell", "pidof", package],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
    if process.returncode == 0 and re.fullmatch(r"\d+(?:\s+\d+)*\s*", process.stdout):
        return True
    if process.returncode == 1 and not process.stdout.strip() and not process.stderr.strip():
        return False
    raise RuntimeError("Cannot reliably determine whether " + package + " is running")


def require_idle(args: argparse.Namespace) -> None:
    activity = bond.adb(args, "shell", "dumpsys", "activity", "activities")
    if re.search(r"BluetoothPairing|PairingDialog|com\.oculus\.panelapp\.devicepairing|GrantPermissionsActivity|MediaProjectionPermission|ScreenCapturePermission", activity, re.I):
        raise RuntimeError("A Quest system confirmation screen is open; handle it first")
    if optional_pid(args, "com.takutomuramatsu.mobits_app_tauri"):
        raise RuntimeError("The separate MoBits app is running; stop its session first")
    if optional_pid(args, "org.mrfps.arena"):
        control = ROOT / "tools/automation/control.py"
        result = subprocess.run([sys.executable, str(control), "--platform", "quest", "--serial", args.serial,
                                 "--adb", str(args.adb), "--operation", "status", "--operation-id", str(uuid.uuid4()),
                                 "--wait-seconds", "8"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=20)
        if result.returncode:
            raise RuntimeError("Cannot read the current Quest app state; Bluetooth was not changed")
        state = json.loads(result.stdout)
        data = state.get("data", {})
        if (state.get("status") != "ok" or data.get("haptics_armed") is not False
                or data.get("quest_mobits_has_session") is not False
                or data.get("quest_mobits_stop_unconfirmed") != 0):
            raise RuntimeError("MoBits has an active or unknown session; Bluetooth was not changed")
        if data.get("video_capture", {}).get("consent_pending") is not False:
            raise RuntimeError("Quest screen-sharing consent is pending or unknown; Bluetooth was not changed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--android-home", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--operation-id", required=True, help="UUIDv4; reusing it never repeats the restart")
    args = parser.parse_args()
    receipt = None
    try:
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", args.serial) or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", args.device):
            raise ValueError("Invalid serial or Bluetooth address")
        args.device = args.device.upper()
        identifier = uuid.UUID(args.operation_id)
        if identifier.version != 4 or str(identifier) != args.operation_id.lower():
            raise ValueError("Operation ID must be a canonical UUIDv4")
        local = Path(os.environ.get("LOCALAPPDATA", str(ROOT / "Saved")))
        serial_key = hashlib.sha256(args.serial.encode("ascii")).hexdigest()[:24]
        folder = local / "MRFPS/BluetoothOperations" / serial_key
        receipt = folder / (str(identifier) + ".json")
        if receipt.exists():
            previous = json.loads(receipt.read_text(encoding="utf-8"))
            if previous.get("id") != str(identifier) or previous.get("device") != args.device:
                raise RuntimeError("Operation ID was already used for a different request")
            print(json.dumps(previous))
            return 0 if previous.get("status") == "cleared" else 1
        os.environ["JAVA_HOME"] = str(args.java_home.resolve(strict=True))
        dex = bond.diagnostic(args)
        before = bond.invoke(args, dex, "inspect")
        if before == "state=10 connected=false":
            print(json.dumps({"status": "already_clear", "device": args.device}))
            return 0
        if before != "state=11 connected=false":
            raise RuntimeError("The target is connected, bonded, or in an unexpected state")
        bond.require_old_bond(args, require_no_key=True)
        require_idle(args)
        # Recheck immediately before changing the adapter; dialogs are never dismissed.
        activity = bond.adb(args, "shell", "dumpsys", "activity", "activities")
        if re.search(r"BluetoothPairing|PairingDialog|com\.oculus\.panelapp\.devicepairing|GrantPermissionsActivity|MediaProjectionPermission|ScreenCapturePermission", activity, re.I):
            raise RuntimeError("A Quest system confirmation screen is open; handle it first")
        if bond.invoke(args, dex, "inspect") != "state=11 connected=false":
            raise RuntimeError("The target bond changed during recovery; Bluetooth was not changed")
        status = bond.adb(args, "shell", "dumpsys", "bluetooth_manager")
        if not re.search(r"^\s*enabled:\s*true\s*$", status, re.I | re.M):
            raise RuntimeError("Bluetooth was not already enabled; its state was not changed")
        folder.mkdir(parents=True, exist_ok=True)
        with receipt.open("x", encoding="utf-8") as output:
            json.dump({"status": "pending", "device": args.device, "id": str(identifier)}, output)
        # The trap re-enables Bluetooth even if a command fails or the ADB shell
        # disconnects after disabling it. No pairing or privacy prompt is answered.
        shell = "trap 'cmd bluetooth_manager enable >/dev/null 2>&1' EXIT HUP INT TERM; " \
                "cmd bluetooth_manager disable >/dev/null || exit 1; " \
                "cmd bluetooth_manager wait-for-state:STATE_OFF >/dev/null || exit 1; " \
                "cmd bluetooth_manager enable >/dev/null || exit 1; " \
                "cmd bluetooth_manager wait-for-state:STATE_ON >/dev/null || exit 1; trap - EXIT HUP INT TERM"
        bond.adb(args, "shell", shell, timeout=60)
        after = bond.invoke(args, dex, "inspect")
        result = {"status": "cleared" if after == "state=10 connected=false" else "restart_completed_state_not_clear",
                  "device": args.device, "id": str(identifier), "after": after}
        receipt.write_text(json.dumps(result), encoding="utf-8")
        print(json.dumps(result))
        return 0 if result["status"] == "cleared" else 1
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        # An existing pending receipt remains pending: its outcome may be unknown.
        print(json.dumps({"status": "error", "message": str(exc)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
