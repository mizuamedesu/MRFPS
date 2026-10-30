#!/usr/bin/env python3
"""Compile and run native session guards with an ordinary C++17 compiler."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++ compiler; defaults to cl, c++, g++, or clang++")
    args = parser.parse_args()
    compiler = args.compiler or next((found for name in ("cl", "c++", "g++", "clang++")
                                      if (found := shutil.which(name))), None)
    if not compiler:
        raise SystemExit("C++17 compiler not found; enter a Visual Studio developer shell or pass --compiler")
    source = Path(__file__).with_name("test_session_guards.cpp")
    with tempfile.TemporaryDirectory(prefix="mrfps-session-test-") as temp:
        directory = Path(temp)
        msvc = Path(compiler).stem.lower() == "cl"
        output = directory / ("test.exe" if msvc else "test")
        if msvc:
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", str(source),
                       "/Fe:" + str(output), "/Fo:" + str(directory / "test.obj")]
        else:
            command = [compiler, "-std=c++17", "-pthread", str(source), "-o", str(output)]
        subprocess.run(command, cwd=temp, check=True, timeout=30)
        subprocess.run([str(output)], check=True, timeout=15)


if __name__ == "__main__":
    main()
