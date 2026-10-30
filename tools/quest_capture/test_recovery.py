#!/usr/bin/env python3
"""Run the capture recovery policy tests without the Android SDK or UE."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def tool(name: str, override: str | None) -> str:
    if override:
        return override
    java_home = os.environ.get("JAVA_HOME")
    candidate = Path(java_home) / "bin" / (name + (".exe" if os.name == "nt" else "")) if java_home else None
    resolved = str(candidate) if candidate and candidate.is_file() else shutil.which(name)
    if not resolved:
        raise SystemExit(f"{name} not found; set JAVA_HOME or pass --{name}")
    return resolved


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java")
    parser.add_argument("--javac")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    policy = root / "Plugins/MRFPSXR/Source/MRFPSXR/Android/Java/com/mrfps/capture/MRFPSCaptureRecoveryPolicy.java"
    harness = Path(__file__).with_name("MRFPSCaptureRecoveryPolicyTest.java")
    with tempfile.TemporaryDirectory(prefix="mrfps-capture-test-") as output:
        subprocess.run([tool("javac", args.javac), "-J-Xmx64m", "-encoding", "UTF-8", "-d", output,
                        str(policy), str(harness)], check=True, timeout=30)
        subprocess.run([tool("java", args.java), "-Xms8m", "-Xmx32m", "-cp", output,
                        "com.mrfps.capture.MRFPSCaptureRecoveryPolicyTest"], check=True, timeout=30)


if __name__ == "__main__":
    main()
