#!/usr/bin/env python3
"""Compatibility command: exercise the fist shield gesture without Unreal."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++17 compiler; defaults to cl, c++, g++, or clang++")
    args = parser.parse_args()
    compiler = args.compiler or next((found for name in ("cl", "c++", "g++", "clang++") if (found := shutil.which(name))), None)
    if not compiler:
        raise SystemExit("C++17 compiler not found")
    source = Path(__file__).with_name("test_shield_gesture.cpp").resolve()
    with tempfile.TemporaryDirectory(prefix="mrfps-shield-gesture-") as temp:
        directory = Path(temp)
        msvc = Path(compiler).stem.lower() == "cl"
        output = directory / ("test.exe" if msvc else "test")
        command = ([compiler, "/nologo", "/std:c++17", "/EHsc", str(source), "/Fe:" + str(output), "/Fo:" + str(directory / "test.obj")]
                   if msvc else [compiler, "-std=c++17", str(source), "-o", str(output)])
        subprocess.run(command, cwd=temp, check=True, timeout=30)
        subprocess.run([str(output)], check=True, timeout=15)


if __name__ == "__main__":
    main()
