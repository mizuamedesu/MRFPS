"""One private-repository runner and owned BuildKit, with an independent watchdog.

Linux only. A short-lived registration token arrives on stdin, never argv/files.
The Windows guard's durable report is the lease: stale/terminal/pressure stops
only this task's recorded process groups and isolated Docker builder/resources.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import time

REPOSITORY = "mizuamedesu/MRFPS-Server-Registry"
BUILDKIT = "moby/buildkit@sha256:cec9f139f45e93c5c69c60f8b07cfad9f43f4ef6b6a6cd917527fea5ff2e3dea"
RUNNER_HASH = "baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40"
GH_HASH = "4c806978ffce1cec70853302a56d70ac6da4f19bae7adca4e1a8ab00633dd91d"


def task_root(task):
    if not re.fullmatch(r"[0-9a-f]{16}", task):
        raise ValueError("RUNNER_TASK_INVALID")
    root = Path("/tmp/mrfps-private-publisher-" + task)
    if root.is_symlink():
        raise ValueError("RUNNER_ROOT_SYMLINK")
    return root


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".incoming")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


def process_identity(pid):
    try:
        fields = Path(f"/proc/{int(pid)}/stat").read_text().rsplit(") ", 1)[1].split()
        return {"pid": int(pid), "pgid": int(fields[2]), "start_ticks": int(fields[19]), "state": fields[0]}
    except (OSError, ValueError, IndexError):
        return None


def same_process(record):
    actual = process_identity(record.get("pid", 0)) if record else None
    return bool(actual and actual["pid"] == actual["pgid"] == record["pgid"] and actual["start_ticks"] == record["start_ticks"])


def stop_group(record):
    if not same_process(record):
        return False
    os.killpg(record["pgid"], signal.SIGTERM)
    for _ in range(20):
        if not same_process(record):
            return True
        time.sleep(0.1)
    if same_process(record):
        os.killpg(record["pgid"], signal.SIGKILL)
    return True


def docker_environment(root):
    environment = os.environ.copy()
    environment["DOCKER_CONFIG"] = str(root / "docker-config")
    environment["BUILDX_CONFIG"] = str(root / "buildx")
    return environment


def command(arguments, root, timeout=60, check=True):
    result = subprocess.run(arguments, env=docker_environment(root), capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise ValueError("OWNED_COMMAND_FAILED_" + str(result.returncode))
    return result


def inspect_container(name, root):
    result = command(["docker", "inspect", name], root, check=False)
    return json.loads(result.stdout)[0] if result.returncode == 0 else None


def guard_reason(path, now=None, diagnostic=None):
    failures = []
    for attempt in range(8):
        try:
            modified = path.stat().st_mtime
            current = time.time() if now is None else now
            if current - modified > 15:
                return "guard_heartbeat_stale"
            report = json.loads(path.read_text(encoding="utf-8-sig"))
            if not isinstance(report, dict):
                raise ValueError("GUARD_REPORT_OBJECT_REQUIRED")
            if report.get("status") not in {"initializing", "running"}:
                return "guard_not_running"
            sample = report.get("lastSample")
            if not isinstance(sample, dict):
                raise ValueError("GUARD_SAMPLE_OBJECT_REQUIRED")
            free, commit = sample.get("availablePhysicalGB"), sample.get("commitPercent")
            if any(type(value) not in (int, float) or not math.isfinite(value) for value in (free, commit)):
                raise ValueError("GUARD_FINITE_MEMORY_SAMPLE_REQUIRED")
            if sample.get("pressureReason") or free < 10 or commit > 75 or commit < 0:
                return "host_memory_pressure"
            # Retry is only a bounded read operation, never a new lease. The
            # original file mtime must still be fresh after that read finishes.
            current = time.time() if now is None else now
            if current - modified > 15:
                return "guard_heartbeat_stale"
            if diagnostic is not None:
                diagnostic.update(read_attempts=attempt + 1, failures=failures)
            return None
        except (OSError, ValueError) as error:
            failures.append({"type": type(error).__name__, "errno": getattr(error, "errno", None)})
            if attempt < 7:
                time.sleep(0.025)
    if diagnostic is not None:
        diagnostic.update(read_attempts=8, failures=failures)
    return "guard_report_unreadable"


def cleanup(task, reason, *, skip_manager=False):
    import fcntl
    root = task_root(task)
    if not (root / "state.json").exists():
        return {"task_id": task, "absent": True}
    with (root / "cleanup.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if (root / "cleanup.json").exists():
            return json.loads((root / "cleanup.json").read_text())
        state = json.loads((root / "state.json").read_text())
        if state.get("task_id") != task or state.get("repository") != REPOSITORY or root.stat().st_uid != os.getuid():
            raise ValueError("RUNNER_CLEANUP_IDENTITY_MISMATCH")
        report = {"task_id": task, "reason": reason, "runtime_group_stopped": False, "manager_group_stopped": False,
                  "removed_container_ids": [], "builder_was_created": state.get("builder_claimed") is True,
                  "builder_removed": False, "credential_directory_removed": False, "errors": []}
        report["runtime_group_stopped"] = stop_group(state.get("runtime"))
        if not skip_manager:
            report["manager_group_stopped"] = stop_group(state.get("manager"))
            unit = "mrfps-private-manager-" + task + ".service"
            invocation = command(["systemctl", "show", unit, "--property=InvocationID", "--value"], root, check=False).stdout.decode().strip()
            if invocation and invocation == state.get("manager_invocation_id"):
                # cgroup containment also catches a fork before its PID was
                # recorded, and children that create another process session.
                command(["systemctl", "stop", unit], root, check=False, timeout=30)
                report["manager_cgroup_stopped"] = True
        # The preclaimed random name is checked absent before creation. An
        # already existing builder/container is never adopted or overwritten.
        name = "buildx_buildkit_mrfps-private-builder-" + task + "0"
        builder = inspect_container(name, root)
        if builder:
            image = builder.get("Config", {}).get("Image", "")
            expected_id = state.get("builder_container_id")
            if (state.get("builder_claimed") is True and builder.get("Name") == "/" + name
                and image.endswith(BUILDKIT.split("@", 1)[1]) and (not expected_id or builder["Id"] == expected_id)):
                result = command(["docker", "rm", "-f", builder["Id"]], root, check=False)
                if result.returncode == 0:
                    report["removed_container_ids"].append(builder["Id"])
                else:
                    report["errors"].append("BUILDER_CONTAINER_REMOVE_FAILED")
            else:
                report["errors"].append("BUILDER_IDENTITY_MISMATCH")
        if state.get("builder_claimed") and "BUILDER_IDENTITY_MISMATCH" not in report["errors"]:
            result = command(["docker", "buildx", "rm", "--force", "mrfps-private-builder-" + task], root, check=False)
            report["builder_removed"] = result.returncode == 0
        ids = command(["docker", "ps", "-aq", "--filter", "label=app.mrfps.publisher.task=" + task], root, check=False).stdout.decode().split()
        for identifier in ids:
            container = inspect_container(identifier, root)
            if (container and container.get("Config", {}).get("Labels", {}).get("app.mrfps.publisher.task") == task
                and container["Name"].startswith("/mrfps-private-negative-" + task + "-")):
                if command(["docker", "rm", "-f", identifier], root, check=False).returncode == 0:
                    report["removed_container_ids"].append(identifier)
        # Only this job's labels; no prune, shared caches, or game images.
        images = command(["docker", "image", "ls", "-q", "--filter", "label=app.mrfps.publisher.task=" + task], root, check=False).stdout.decode().split()
        for identifier in set(images):
            command(["docker", "image", "rm", identifier], root, check=False)
        runtime_identity = process_identity((state.get("runtime") or {}).get("pid", 0))
        if runtime_identity and runtime_identity["state"] != "Z" and same_process(state.get("runtime")):
            report["errors"].append("RUNTIME_GROUP_STILL_ALIVE")
        if not report["errors"]:
            for directory in (root / "runner", root / "docker-config", root / "buildx"):
                if directory.is_symlink() or directory.parent != root:
                    raise ValueError("RUNNER_CLEANUP_PATH_INVALID")
                if directory.exists():
                    shutil.rmtree(directory)
            report["credential_directory_removed"] = True
        report["finished_utc_ns"] = time.time_ns()
        report["passed"] = not report["errors"]
        write_json(root / "cleanup.json", report)
        return report


def watchdog(task, guard):
    root = task_root(task)
    while True:
        if (root / "cleanup.json").exists():
            return
        state = json.loads((root / "state.json").read_text())
        diagnostic = {}
        reason = guard_reason(guard, diagnostic=diagnostic)
        if diagnostic.get("failures"):
            write_json(root / "guard-read-diagnostic.json", {"reason": reason, **diagnostic})
        if not same_process(state.get("manager")):
            reason = reason or "manager_disappeared"
        if time.time() - state["created_at"] > 1900:
            reason = "runner_maximum_lifetime"
        if reason:
            cleanup(task, reason)
            return
        time.sleep(1)


def checked_archive(path, expected):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != expected:
        raise ValueError("OFFICIAL_RUNNER_ARCHIVE_DIGEST_MISMATCH")


def safe_configuration_diagnostic(result):
    # Classify output in memory; never store arbitrary config text or tokens.
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace").lower()
    return {"exit_code": result.returncode, "icu_mentioned": "icu" in output,
            "dependency_mentioned": any(text in output for text in ("dependencies", "dependency", "error while loading", "not found")),
            "unauthorized_mentioned": any(text in output for text in ("unauthorized", "401", "403", "forbidden")),
            "network_mentioned": any(text in output for text in ("timed out", "unable to connect", "name or service", "ssl connection", "could not resolve")),
            "unsupported_mentioned": "not supported" in output,
            "sudo_mentioned": "sudo" in output,
            "token_mentioned": "token" in output}


def isolated_icu(root):
    # Use the distro's authenticated APT metadata, but never install a package
    # or change ldconfig/system paths. The official runner config explicitly
    # checks LD_LIBRARY_PATH using ldconfig -NXv (no cache/link writes).
    metadata = command(["apt-cache", "show", "libicu78"], root).stdout.decode()
    fields = dict(line.split(": ", 1) for line in metadata.split("\n\n", 1)[0].splitlines() if ": " in line)
    version, digest = fields.get("Version", ""), fields.get("SHA256", "")
    if (fields.get("Package") != "libicu78" or fields.get("Architecture") != "amd64"
        or not fields.get("Filename", "").startswith("pool/main/i/icu/")
        or not re.fullmatch(r"[0-9A-Za-z:.+~-]{1,80}", version)
        or not re.fullmatch(r"[0-9a-f]{64}", digest)):
        raise ValueError("OFFICIAL_ISOLATED_ICU_METADATA_INVALID")
    download = subprocess.run(["apt-get", "download", "libicu78=" + version], cwd=root, capture_output=True, timeout=120)
    if download.returncode:
        raise ValueError("OFFICIAL_ISOLATED_ICU_DOWNLOAD_FAILED")
    packages = list(root.glob("libicu78_*.deb"))
    if len(packages) != 1:
        raise ValueError("OFFICIAL_ISOLATED_ICU_PACKAGE_AMBIGUOUS")
    checked_archive(packages[0], digest)
    destination = root / "runtime-libs"
    command(["dpkg-deb", "-x", str(packages[0]), str(destination)], root)
    write_json(root / "isolated-icu.json", {"package": "libicu78", "version": version, "sha256": digest, "system_installed": False})
    return destination / "usr/lib/x86_64-linux-gnu"


def run(task, guard, runner_archive, gh_archive, inspect_only=False):
    root = task_root(task)
    os.umask(0o077)
    root.mkdir(mode=0o700)
    if os.getpgrp() != os.getpid():
        os.setsid()
    invocation_id = os.environ.get("INVOCATION_ID", "")
    if not re.fullmatch(r"[0-9a-f]{32}", invocation_id):
        raise ValueError("OWNED_SYSTEMD_CGROUP_REQUIRED")
    state = {"task_id": task, "repository": REPOSITORY, "created_at": time.time(), "manager": process_identity(os.getpid()), "manager_invocation_id": invocation_id, "builder_claimed": False}
    write_json(root / "state.json", state)
    watchdog_process = subprocess.Popen(["systemd-run", "--quiet", "--collect", "--unit=mrfps-private-watch-" + task, "--property=MemoryMax=64M", "--property=MemorySwapMax=0", "--property=RuntimeMaxSec=1900",
                                         sys.executable, str(Path(__file__).resolve()), "watch", "--task", task, "--guard", str(guard)], start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if watchdog_process.wait(timeout=10) != 0:
        raise ValueError("OWNED_INDEPENDENT_WATCHDOG_START_FAILED")
    exit_code = 1
    try:
        if guard_reason(guard):
            raise ValueError("RUNNER_GUARD_NOT_READY")
        properties = command(["systemctl", "show", "mrfps-private-manager-" + task + ".service", "--property=MemoryMax", "--property=MemorySwapMax", "--property=CPUQuotaPerSecUSec", "--property=InvocationID"], root).stdout.decode()
        properties = dict(line.split("=", 1) for line in properties.splitlines() if "=" in line)
        if (properties.get("InvocationID") != invocation_id or properties.get("MemoryMax") != str(1024 * 1024 * 1024)
            or properties.get("MemorySwapMax") != "0"):
            raise ValueError("OWNED_RUNNER_CGROUP_LIMITS_MISMATCH")
        state["manager_limits"] = properties
        write_json(root / "state.json", state)
        checked_archive(runner_archive, RUNNER_HASH)
        checked_archive(gh_archive, GH_HASH)
        runner = root / "runner"
        runner.mkdir()
        with tarfile.open(runner_archive) as archive:
            archive.extractall(runner, filter="data")
        (root / "bin").mkdir()
        with tarfile.open(gh_archive) as archive:
            matches = [member for member in archive.getmembers() if member.name.endswith("/bin/gh") and member.isfile()]
            if len(matches) != 1:
                raise ValueError("OFFICIAL_GH_ARCHIVE_LAYOUT_INVALID")
            with archive.extractfile(matches[0]) as source, (root / "bin/gh").open("wb") as target:
                shutil.copyfileobj(source, target)
            (root / "bin/gh").chmod(0o755)
        for directory in ("docker-config", "buildx"):
            (root / directory).mkdir()
        icu = isolated_icu(root)
        builder = "mrfps-private-builder-" + task
        info = None
        if not inspect_only:
            if inspect_container("buildx_buildkit_" + builder + "0", root):
                raise ValueError("OWNED_BUILDER_NAME_COLLISION")
            state["builder_claimed"] = True
            write_json(root / "state.json", state)
            config = root / "buildkit.toml"
            config.write_text('[worker.oci]\n  max-parallelism = 1\n')
            command(["docker", "buildx", "create", "--name", builder, "--driver", "docker-container", "--driver-opt", "image=" + BUILDKIT,
                     "--driver-opt", "memory=1536m", "--driver-opt", "memory-swap=1536m", "--driver-opt", "cpu-quota=200000", "--driver-opt", "cpu-period=100000", "--buildkitd-config", str(config)], root)
            command(["docker", "buildx", "inspect", builder, "--bootstrap"], root, timeout=180)
            info = inspect_container("buildx_buildkit_" + builder + "0", root)
            if (not info or info["HostConfig"]["Memory"] != 1536 * 1024 * 1024 or info["HostConfig"]["MemorySwap"] != 1536 * 1024 * 1024
                or info["HostConfig"]["CpuQuota"] != 200000 or info["HostConfig"]["CpuPeriod"] != 100000):
                raise ValueError("OWNED_BUILDER_RESOURCE_LIMIT_MISMATCH")
            state["builder_container_id"] = info["Id"]
            write_json(root / "state.json", state)
        payload = json.loads(sys.stdin.buffer.readline(16384))
        token = payload.pop("token", None)
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{20,200}", token):
            raise ValueError("EPHEMERAL_REGISTRATION_TOKEN_INVALID")
        environment = docker_environment(root)
        environment.update(PATH=str(root / "bin") + ":" + environment.get("PATH", "/usr/bin:/bin"), RUNNER_ALLOW_RUNASROOT="1", MRFPS_PUBLISHER_TASK_ID=task, MRFPS_BUILDER_NAME=builder)
        environment["LD_LIBRARY_PATH"] = str(icu)
        environment.pop("GH_TOKEN", None)
        environment.pop("GITHUB_TOKEN", None)
        registration_environment = environment.copy()
        registration_environment["ACTIONS_RUNNER_INPUT_TOKEN"] = token
        result = subprocess.run([str(runner / "config.sh"), "--unattended", "--ephemeral", "--disableupdate", "--url", "https://github.com/" + REPOSITORY,
                                 "--name", "mrfps-private-" + task, "--labels", "mrfps-private-" + task, "--work", "_work"], cwd=runner,
                                env=registration_environment, capture_output=True, timeout=90)
        registration_environment.clear()
        token = None
        if result.returncode:
            diagnostic = safe_configuration_diagnostic(result)
            write_json(root / "config-diagnostic.json", diagnostic)
            print(json.dumps({"phase": "configuration-failed", **diagnostic}), flush=True)
            raise ValueError("EPHEMERAL_RUNNER_CONFIGURATION_FAILED_" + str(result.returncode))
        runner_config = json.loads((runner / ".runner").read_text(encoding="utf-8-sig"))
        if not isinstance(runner_config.get("agentId"), int) or runner_config.get("agentName") != "mrfps-private-" + task:
            raise ValueError("OWNED_CONFIGURED_RUNNER_IDENTITY_INVALID")
        state["runner_id"] = runner_config["agentId"]
        write_json(root / "state.json", state)
        # The group leader stays alive after run.sh exits, so cleanup can still
        # verify /proc starttime and remove any background job descendants.
        bootstrap = "import os,signal,subprocess,time,json; from pathlib import Path; os.kill(os.getpid(),signal.SIGSTOP); p=subprocess.run(['./run.sh']); Path('../runner-finished.incoming').write_text(json.dumps({'exit_code':p.returncode})); os.replace('../runner-finished.incoming','../runner-finished.json'); time.sleep(1900)"
        runtime = subprocess.Popen([sys.executable, "-c", bootstrap], cwd=runner, env=environment, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            identity = process_identity(runtime.pid)
            if identity and identity["state"] == "T":
                break
            time.sleep(0.01)
        else:
            raise ValueError("OWNED_RUNNER_START_GATE_FAILED")
        state["runtime"] = identity
        write_json(root / "state.json", state)
        os.kill(runtime.pid, signal.SIGCONT)
        print(json.dumps({"phase": "runner-started", "task_id": task, "runner_pid": runtime.pid, "builder_id": info["Id"] if info else None}), flush=True)
        while not (root / "runner-finished.json").exists():
            if runtime.poll() is not None:
                raise ValueError("OWNED_RUNNER_GROUP_EXITED_EARLY")
            if guard_reason(guard):
                raise ValueError("HOST_GUARD_LEASE_ENDED")
            if time.time() - state["created_at"] > 1800:
                raise ValueError("OWNED_RUNNER_TIME_LIMIT")
            time.sleep(1)
        exit_code = json.loads((root / "runner-finished.json").read_text())["exit_code"]
    finally:
        report = cleanup(task, "manager_finally", skip_manager=True)
        watchdog_process.wait(timeout=10)
        print(json.dumps({"phase": "cleanup", "task_id": task, "passed": report["passed"]}), flush=True)
    return exit_code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("run", "watch", "cleanup"))
    parser.add_argument("--task", required=True)
    parser.add_argument("--guard", type=Path)
    parser.add_argument("--runner-archive", type=Path)
    parser.add_argument("--gh-archive", type=Path)
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    if args.action == "run":
        return run(args.task, args.guard, args.runner_archive, args.gh_archive, args.inspect_only)
    if args.action == "watch":
        watchdog(args.task, args.guard)
    else:
        print(json.dumps(cleanup(args.task, "host_finally")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        value = str(error)
        safe = value if re.fullmatch(r"[A-Z0-9_]+", value) else type(error).__name__
        raise SystemExit("MRFPS_PRIVATE_RUNNER_ERROR " + safe)
