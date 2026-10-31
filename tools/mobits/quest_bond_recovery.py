#!/usr/bin/env python3
"""Inspect or cancel a demonstrably stale, disconnected Quest BLE bond attempt."""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("QuestBondRecovery.java")


def run_command(command: list[str], timeout: int = 30) -> str:
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    if result.returncode:
        raise RuntimeError("Operation failed: " + (result.stderr or result.stdout).strip()[-1000:])
    return result.stdout.strip()


def adb(args: argparse.Namespace, *command: str, timeout: int = 30) -> str:
    return run_command([str(args.adb), "-s", args.serial, *command], timeout)


def diagnostic(args: argparse.Namespace) -> str:
    identity = hashlib.sha256(SOURCE.read_bytes()).hexdigest()[:16]
    folder = ROOT / "Saved/Diagnostics/MoBits" / ("bond-recovery-" + identity)
    dex = folder / "classes.dex"
    java = Path(args.java_home) / "bin" / ("javac.exe" if sys.platform == "win32" else "javac")
    sdk = Path(args.android_home)
    android_jar = sdk / "platforms/android-35/android.jar"
    d8 = sdk / "build-tools/35.0.1" / ("d8.bat" if sys.platform == "win32" else "d8")
    if not dex.is_file():
        folder.mkdir(parents=True, exist_ok=True)
        run_command([str(java), "-source", "8", "-target", "8", "-bootclasspath", str(android_jar),
                     "-d", str(folder), str(SOURCE)], 60)
        # d8.bat expects JAVA_HOME; the Windows build wrapper sets it already.
        run_command([str(d8), "--min-api", "26", "--lib", str(android_jar), "--output", str(folder),
                     str(folder / "jp/mrfps/diagnostics/QuestBondRecovery.class")], 60)
    digest = hashlib.sha256(dex.read_bytes()).hexdigest()
    remote = "/data/local/tmp/mrfps-bond-recovery-" + digest[:16] + ".dex"
    # Do not overwrite a different file at this deterministic path.
    check = subprocess.run([str(args.adb), "-s", args.serial, "shell", "sha256sum", remote],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
    if check.returncode == 0:
        if not check.stdout.startswith(digest + " "):
            raise RuntimeError("Existing diagnostic on Quest differs; it was not overwritten")
    else:
        adb(args, "push", str(dex), remote)
    return remote


def invoke(args: argparse.Namespace, dex: str, operation: str) -> str:
    # Every substituted argument is strictly validated before forming the shell command.
    command = "CLASSPATH=" + dex + " app_process /system/bin jp.mrfps.diagnostics.QuestBondRecovery " + operation + " " + args.device
    return adb(args, "shell", command, timeout=25)


def require_old_bond(args: argparse.Namespace, require_no_key: bool = False) -> None:
    activity = adb(args, "shell", "dumpsys", "activity", "activities")
    if re.search(r"BluetoothPairing|PairingDialog|com\.oculus\.panelapp\.devicepairing", activity, re.I):
        raise RuntimeError("Quest has a Bluetooth pairing screen; resolve it before clearing anything")
    notifications = adb(args, "shell", "dumpsys", "notification")
    if re.search(r"NotificationRecord[^\r\n]*(?:bluetooth|pairing)", notifications, re.I):
        raise RuntimeError("Quest has a Bluetooth notification; resolve it before clearing anything")
    dump = adb(args, "shell", "dumpsys", "bluetooth_manager")
    section = re.search(r"Bond Events:\s*(.*?)(?:Link Key Types:|\Z)", dump, re.S)
    if not section:
        raise RuntimeError("Android bond history is unavailable; no changes made")
    suffix = re.escape(args.device[-5:].lower())
    events = re.findall(r"^\s*(\d\d:\d\d:\d\d\.\d+)\s+xx:xx:xx:xx:" + suffix + r"\s+\S+\s+BOND_STATE_(\w+)", section.group(1), re.I | re.M)
    if not events or events[-1][1].upper() != "BONDING":
        raise RuntimeError("The most recent recorded state is not an orphaned bonding attempt")
    quest_clock = adb(args, "shell", "date", "+%Y-%m-%dT%H:%M:%S%z")
    now = datetime.strptime(quest_clock, "%Y-%m-%dT%H:%M:%S%z")
    event_time = datetime.strptime(events[-1][0], "%H:%M:%S.%f").time()
    event = datetime.combine(now.date(), event_time, now.tzinfo)
    if event > now:
        event -= timedelta(days=1)
    if now - event < timedelta(minutes=2):
        raise RuntimeError("The pairing attempt is recent; it was not cancelled")
    if require_no_key:
        keys = re.search(r"Link Key Types:\s*(.*?)(?:\n\S|\Z)", dump, re.S)
        if not keys or re.search(r"xx:xx:xx:xx:" + suffix, keys.group(1), re.I):
            raise RuntimeError("The target may have a saved key; no removal was attempted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--android-home", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--clear-stale", action="store_true")
    parser.add_argument("--remove-orphan", action="store_true", help="after a failed cancellation, remove only an unconnected bonding entry with no saved key")
    args = parser.parse_args()
    try:
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", args.serial):
            raise ValueError("Invalid Quest serial")
        if not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", args.device):
            raise ValueError("Invalid Bluetooth address")
        args.device = args.device.upper()
        import os
        os.environ["JAVA_HOME"] = str(args.java_home.resolve(strict=True))
        dex = diagnostic(args)
        before = invoke(args, dex, "inspect")
        if args.remove_orphan and not args.clear_stale:
            raise ValueError("--remove-orphan requires --clear-stale")
        if args.clear_stale and before == "state=11 connected=false":
            require_old_bond(args, require_no_key=args.remove_orphan)
            result = invoke(args, dex, "remove-orphan" if args.remove_orphan else "cancel-stale")
            if result.startswith("error="):
                raise RuntimeError(result)
        elif args.clear_stale and before == "state=10 connected=false":
            result = "already_clear"
        elif args.clear_stale:
            raise RuntimeError("Bond is connected, bonded, or not in a recognized state; no changes made")
        else:
            result = "observed"
        print(json.dumps({"device": args.device, "before": before, "result": result}))
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(json.dumps({"status": "error", "message": str(exc)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
