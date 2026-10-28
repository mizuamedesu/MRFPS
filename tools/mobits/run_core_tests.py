#!/usr/bin/env python3
"""Compile the official MoBits portable core/codec tests against vendored source."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def windows_msvc():
    environment = os.environ.copy()
    if shutil.which("cl"):
        return shutil.which("cl"), environment
    program_files = Path(environment.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    vswhere = program_files / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                       "-property", "installationPath"], text=True).strip()
    if not install:
        raise RuntimeError("Visual Studio C++ tools were not found.")
    versions = lambda path: tuple(int(x) for x in path.name.split(".") if x.isdigit())
    msvc = max((Path(install) / "VC/Tools/MSVC").iterdir(), key=versions)
    kits = program_files / "Windows Kits/10"
    sdk_version = max((kits / "Include").iterdir(), key=versions).name
    environment["INCLUDE"] = os.pathsep.join(map(str, [msvc / "include", kits / "Include" / sdk_version / "ucrt",
                                                       kits / "Include" / sdk_version / "shared", kits / "Include" / sdk_version / "um"]))
    environment["LIB"] = os.pathsep.join(map(str, [msvc / "lib/x64", kits / "Lib" / sdk_version / "ucrt/x64",
                                                   kits / "Lib" / sdk_version / "um/x64"]))
    environment["PATH"] = str(msvc / "bin/Hostx64/x64") + os.pathsep + environment["PATH"]
    return str(msvc / "bin/Hostx64/x64/cl.exe"), environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", help="C++ compiler; default MSVC on Windows, clang++ elsewhere")
    parser.add_argument("--no-sanitizers", action="store_true", help="disable ASan/UBSan on non-MSVC compilers")
    args = parser.parse_args()
    environment = os.environ.copy()
    compiler = args.cxx
    if compiler is None and os.name == "nt":
        compiler, environment = windows_msvc()
    compiler = compiler or environment.get("CXX", "clang++")
    with tempfile.TemporaryDirectory(prefix="mrfps-mobits-core-") as temporary:
        output = Path(temporary) / ("core-tests.exe" if os.name == "nt" else "core-tests")
        source = Path(__file__).with_name("upstream_core_tests.cpp")
        if Path(compiler).name.lower() in ("cl", "cl.exe"):
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/Fo" + str(Path(temporary) / "core-tests.obj"),
                       "/Fe" + str(output), str(source)]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror"]
            if not args.no_sanitizers:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            command += [str(source), "-o", str(output)]
        subprocess.run(command, env=environment, check=True)
        subprocess.run([str(output)], env=environment, check=True)


if __name__ == "__main__":
    main()
