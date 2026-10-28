#!/usr/bin/env python3
"""Build actual portable JWT parser/Ed25519 verifier using this UE's OpenSSL."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time


def msvc_environment():
    environment = os.environ.copy()
    program_files = Path(environment.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    vswhere = program_files / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                       "-property", "installationPath"], text=True).strip()
    if not install:
        raise RuntimeError("Visual Studio C++ tools were not found.")
    version = lambda path: tuple(int(x) for x in path.name.split(".") if x.isdigit())
    msvc = max((Path(install) / "VC/Tools/MSVC").iterdir(), key=version)
    kits = program_files / "Windows Kits/10"
    sdk = max((kits / "Include").iterdir(), key=version).name
    environment["INCLUDE"] = os.pathsep.join(str(path) for path in [msvc / "include", kits / "Include" / sdk / "ucrt",
                                                                   kits / "Include" / sdk / "shared", kits / "Include" / sdk / "um"])
    environment["LIB"] = os.pathsep.join(str(path) for path in [msvc / "lib/x64", kits / "Lib" / sdk / "ucrt/x64",
                                                               kits / "Lib" / sdk / "um/x64"])
    environment["PATH"] = str(msvc / "bin/Hostx64/x64") + os.pathsep + environment["PATH"]
    return msvc / "bin/Hostx64/x64/cl.exe", environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path("C:/UnrealEngine/UE5.8"))
    parser.add_argument("--public-key", type=Path)
    parser.add_argument("--token", type=Path)
    parser.add_argument("--audience", default="mrfps")
    parser.add_argument("--role", choices=["player", "inspector"], default="player")
    parser.add_argument("--fingerprint", help="expected public DTLS certificate fingerprint for interop")
    args = parser.parse_args()
    if any([args.public_key, args.token, args.fingerprint]) and not all([args.public_key, args.token, args.fingerprint]):
        parser.error("--public-key, --token, and --fingerprint must be provided together")
    plugin = Path(__file__).resolve().parents[1]
    openssl = args.engine / "Engine/Source/ThirdParty/OpenSSL/1.1.1t"
    compiler, environment = msvc_environment()
    with tempfile.TemporaryDirectory(prefix="mrfps-auth-tests-") as temporary:
        directory = Path(temporary)
        output = directory / "auth-tests.exe"
        command = [str(compiler), "/nologo", "/std:c++17", "/EHsc", "/MD", "/W4", "/WX", "/O2",
                   "/I" + str(openssl / "include/Win64/VS2015"), "/Fo" + str(directory) + "/", "/Fe" + str(output),
                   str(plugin / "Tests/CoreTests.cpp"), str(plugin / "Source/MRFPSAuth/Private/MRFPSAuthCrypto.cpp"),
                   "/link", str(openssl / "lib/Win64/VS2015/Release/libcrypto.lib"), "crypt32.lib", "ws2_32.lib", "advapi32.lib", "user32.lib"]
        subprocess.run(command, env=environment, check=True)
        subprocess.run([str(output)], env=environment, check=True)
        if args.public_key:
            fixture = directory / "verify-fixture.exe"
            fixture_command = command.copy()
            fixture_command[fixture_command.index("/Fe" + str(output))] = "/Fe" + str(fixture)
            fixture_command[fixture_command.index(str(plugin / "Tests/CoreTests.cpp"))] = str(plugin / "Tests/VerifyFixture.cpp")
            subprocess.run(fixture_command, env=environment, check=True)
            subprocess.run([str(fixture), str(args.public_key), str(args.token), args.audience, args.role,
                            args.fingerprint, str(int(time.time()))], env=environment, check=True)


if __name__ == "__main__":
    main()
