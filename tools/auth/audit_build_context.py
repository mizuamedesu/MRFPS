"""Verify the actual default-deny Docker COPY context contains only its allowlist."""
import argparse
import json
import pathlib
import subprocess
import tarfile
import time

from audit_container import docker_prefix

ALLOWED_FILES = {
    ".dockerignore", "Deploy/Linux/Dockerfile", "Deploy/Linux/Dockerfile.dockerignore",
    "Deploy/Linux/entrypoint.sh", "Deploy/Linux/auth_config.py", "Deploy/Linux/healthcheck.py",
}
PACKAGE_PREFIX = "Artifacts/Packages/LinuxServer/"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="mrfps-context-audit:local")
    parser.add_argument("--wsl-distribution")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    prefix = docker_prefix(args.wsl_distribution)
    created = subprocess.run(prefix + ["create", args.image, "/not-executed-context-audit"], capture_output=True, check=True)
    container = created.stdout.decode("ascii").strip()
    exported = subprocess.Popen(prefix + ["export", container], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    files, unexpected, size = [], [], 0
    try:
        with tarfile.open(fileobj=exported.stdout, mode="r|*") as archive:
            for member in archive:
                path = member.name.removeprefix("./")
                if not member.isfile() or not path.startswith("context/"):
                    continue
                relative = path.removeprefix("context/")
                files.append(relative)
                size += member.size
                if relative not in ALLOWED_FILES and not relative.startswith(PACKAGE_PREFIX):
                    unexpected.append(relative)
        error = exported.stderr.read()
        if exported.wait() != 0:
            raise RuntimeError("Context audit export failed; no exclusion claim made.")
    finally:
        if exported.poll() is None:
            exported.kill()
            exported.wait()
        exported.stdout.close()
        exported.stderr.close()
        subprocess.run(prefix + ["rm", container], capture_output=True, check=True)
    required = ALLOWED_FILES | {PACKAGE_PREFIX + "MRFPS/Binaries/Linux/MRFPSServer"}
    missing = sorted(required - set(files))
    passed = not unexpected and not missing
    report = {"timestamp_utc": int(time.time()), "passed": passed,
              "scope": "Actual BuildKit context-audit COPY file set; .local, workspace Saved, .git and non-LinuxServer package paths are excluded",
              "file_count": len(files), "bytes": size, "unexpected_paths": unexpected,
              "missing_required_paths": missing, "files": sorted(files)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": passed, "files": len(files), "bytes": size, "report": str(args.output.resolve())}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
