#!/usr/bin/env python3
"""Recompile auth sources with UBT's actual generated UE headers/PCH, separately."""
import argparse
from pathlib import Path
import subprocess

from run_core_tests import msvc_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path("C:/UnrealEngine/UE5.8"))
    args = parser.parse_args()
    plugin = Path(__file__).resolve().parents[1]
    generated = plugin / "Intermediate/Build/Win64/x64/UnrealEditor/Development/MRFPSAuth"
    output = plugin / "Intermediate/CompileChecks"
    output.mkdir(parents=True, exist_ok=True)
    compiler, environment = msvc_environment()
    names = ["MRFPSAuthCrypto", "MRFPSAuthTests", "MRFPSAuthenticatedLocalPlayer", "MRFPSAuthClientSubsystem",
             "MRFPSAuthLogPrivacy", "MRFPSAuthVerifier", "MRFPSAuthModule"]
    failures = []
    for name in names:
        source = generated / (name + ".cpp.obj.rsp")
        text = source.read_text(encoding="utf-8-sig")
        for suffix in (".cpp.obj", ".cpp.sarif", ".cpp.dep.json"):
            text = text.replace((generated / (name + suffix)).as_posix(), (output / (name + suffix)).as_posix())
        response = output / (name + ".rsp")
        response.write_text(text, encoding="utf-8")
        result = subprocess.run([str(compiler), "@" + str(response)], cwd=args.engine / "Engine/Source",
                                env=environment, capture_output=True, text=True)
        (output / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        print(f"{name}: exit {result.returncode}", flush=True)
        if result.returncode:
            print(result.stdout + result.stderr, flush=True)
            failures.append(name)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
