"""Low-load Docker runtime/configuration checks; never starts a UE client or rebuilds an image."""
import argparse
import base64
import datetime
import hashlib
import json
import pathlib
import re
import subprocess

JWT = re.compile(r"(?<![A-Za-z0-9_-])eyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+(?![A-Za-z0-9_-])")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--public-key", required=True, type=pathlib.Path)
    parser.add_argument("--wsl-distribution")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    prefix = (["wsl", "-d", args.wsl_distribution, "-u", "root", "--exec", "docker"]
              if args.wsl_distribution else ["docker"])

    def docker(arguments, check=True):
        result = subprocess.run(prefix + arguments, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=45)
        if check and result.returncode:
            # Do not echo arbitrary process output or environment values.
            raise RuntimeError("Docker audit command failed; no runtime pass claim was made.")
        return result

    def inspect():
        return json.loads(docker(["inspect", args.container]).stdout)[0]

    def snapshot(container):
        return {"container_id": container["Id"], "image_id": container["Image"],
                "started_at_utc": container["State"]["StartedAt"], "running": container["State"]["Running"],
                "health": container["State"].get("Health", {}).get("Status"), "restart_count": container["RestartCount"],
                "oom_killed": container["State"]["OOMKilled"], "memory_bytes": container["HostConfig"]["Memory"],
                "memory_plus_swap_bytes": container["HostConfig"]["MemorySwap"], "nano_cpus": container["HostConfig"]["NanoCpus"]}

    public = args.public_key.read_bytes()
    if b"PRIVATE" in public or not public.startswith(b"-----BEGIN PUBLIC KEY-----"):
        raise ValueError("Only the issuer public-key file is accepted.")
    encoded = base64.b64encode(public).decode("ascii")
    initial = inspect()
    if not initial["State"]["Running"]:
        raise RuntimeError("The game container must already be running.")
    image = initial["Image"]  # Test the exact immutable image that is running.
    bad_spki = b"-----BEGIN PUBLIC KEY-----\nMAA=\n-----END PUBLIC KEY-----\n"
    negative_cases = [
        ("missing_public_key", {}, None, 78),
        ("ambiguous_public_key", {"MRFPS_AUTH_PUBLIC_KEY_B64": encoded, "MRFPS_AUTH_PUBLIC_KEY_PEM": "ambiguous"}, None, 78),
        ("malformed_base64", {"MRFPS_AUTH_PUBLIC_KEY_B64": "%%%"}, None, 78),
        ("non_ed25519_spki", {"MRFPS_AUTH_PUBLIC_KEY_B64": base64.b64encode(bad_spki).decode("ascii")}, None, 78),
        ("invalid_audience", {"MRFPS_AUTH_PUBLIC_KEY_B64": encoded, "MRFPS_AUTH_AUDIENCE": "contains whitespace"}, None, 78),
        ("empty_audience", {"MRFPS_AUTH_PUBLIC_KEY_B64": encoded, "MRFPS_AUTH_AUDIENCE": ""}, None, 78),
        ("missing_key_unhealthy", {}, "/usr/bin/python3", 1),
        ("public_key_without_game_unhealthy", {"MRFPS_AUTH_PUBLIC_KEY_B64": encoded}, "/usr/bin/python3", 1),
    ]
    results = []
    for name, environment, entrypoint, expected in negative_cases:
        command = ["run", "--rm", "--network", "none", "--memory", "64m", "--memory-swap", "64m",
                   "--cpus", "0.25", "--pids-limit", "32"]
        for key, value in environment.items():
            command.extend(["--env", key + "=" + value])
        if entrypoint:
            command.extend(["--entrypoint", entrypoint])
        command.append(image)
        if entrypoint:
            command.append("/usr/local/bin/mrfps-healthcheck.py")
        run = docker(command, check=False)
        output = run.stdout + run.stderr
        refused_before_game = "MRFPS_SERVER_READY" not in output and "MRFPS_AUTH_TRANSPORT_READY" not in output
        config_marker = "AUTH_CONFIG:" in output
        results.append({"case": name, "expected_exit": expected, "actual_exit": run.returncode,
                        "refused_before_game": refused_before_game, "configuration_rejection_marker": config_marker,
                        "passed": run.returncode == expected and refused_before_game and (config_marker if expected == 78 else True)})

    cgroup_run = docker(["exec", "--user", "10001:10001", args.container, "cat", "/sys/fs/cgroup/memory.max",
                         "/sys/fs/cgroup/memory.swap.max", "/sys/fs/cgroup/cpu.max", "/sys/fs/cgroup/memory.current"])
    cgroup_lines = cgroup_run.stdout.strip().splitlines()
    if len(cgroup_lines) != 4:
        raise RuntimeError("Unexpected cgroup output; resource limits were not verified.")
    cgroup = {"memory_max_bytes": int(cgroup_lines[0]), "memory_swap_max_bytes": int(cgroup_lines[1]),
              "cpu_max": cgroup_lines[2], "memory_current_bytes": int(cgroup_lines[3])}
    stats = json.loads(docker(["stats", "--no-stream", "--format", "{{json .}}", args.container]).stdout)
    logs_run = docker(["logs", args.container])
    logs = logs_run.stdout + logs_run.stderr
    fingerprints = re.findall(r"MRFPS_AUTH_TRANSPORT_READY fingerprint=([a-f0-9]{64})", logs)
    privacy = {"jwt_pattern_matches": len(JWT.findall(logs)), "private_pem_present": bool(re.search(r"-----BEGIN (?:ENCRYPTED |ED25519 |RSA |EC )?PRIVATE KEY-----", logs))}
    final = inspect()
    initial_state, final_state = snapshot(initial), snapshot(final)
    checks = {
        "runtime_healthy": final_state["running"] and final_state["health"] == "healthy",
        "same_container_and_started_at": initial_state["container_id"] == final_state["container_id"] and initial_state["started_at_utc"] == final_state["started_at_utc"],
        "no_restart_or_oom": final_state["restart_count"] == 0 and not final_state["oom_killed"],
        "host_config_limits": final_state["memory_bytes"] == 1073741824 and final_state["memory_plus_swap_bytes"] == 1073741824 and final_state["nano_cpus"] == 2000000000,
        "cgroup_limits": cgroup["memory_max_bytes"] == 1073741824 and cgroup["memory_swap_max_bytes"] == 0 and cgroup["cpu_max"].split() == ["200000", "100000"],
        "native_server_and_dtls_ready": "MRFPS_SERVER_READY" in logs and bool(fingerprints),
        "no_credentials_in_game_logs": privacy["jwt_pattern_matches"] == 0 and not privacy["private_pem_present"],
        "all_configuration_and_health_negatives_passed": all(case["passed"] for case in results),
    }
    report = {"at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "Running Linux game-container limits, health, stable identity, log privacy, and fail-closed configuration/health negatives using network-isolated 64 MiB helper containers. No game clients or builds started.",
              "passed": all(checks.values()), "whole_objective_complete": False,
              "public_key_sha256": hashlib.sha256(public).hexdigest(), "initial": initial_state, "final": final_state,
              "dtls_fingerprint": fingerprints[-1] if fingerprints else None, "cgroup": cgroup, "stats": stats,
              "log_privacy": privacy, "negative_cases": results, "checks": checks}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "report": str(args.output.resolve()), "negative_cases": len(results), "checks": checks}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
