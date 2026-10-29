"""macOS-only memory reserve guard; terminates only its own child process group."""
import argparse
import json
import math
import os
import pathlib
import re
import signal
import stat
import subprocess
import sys
import time
import uuid


def available_memory():
    raw = subprocess.check_output(["vm_stat"], text=True, timeout=5)
    page = re.search(r"page size of (\d+) bytes", raw)
    values = dict(re.findall(r"^(Pages [^:]+):\s+(\d+)\.", raw, re.M))
    required = ("Pages free", "Pages inactive", "Pages speculative")
    if not page or any(key not in values for key in required):
        raise RuntimeError("vm_statの空きメモリを取得できません。起動を中止します。")
    return int(page[1]) * sum(int(values[key]) for key in required)


def write_report(path, report):
    if path is None:
        return
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(report, stream, separators=(",", ":"), sort_keys=True)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def stop_requested(path, instance):
    if path is None:
        return False
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except FileNotFoundError:
        return False
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or info.st_size > 256 or info.st_mode & 0o077:
            raise RuntimeError("unsafe stop request")
        if json.loads(os.read(fd, 257)) != {"action": "stop", "instance": instance}:
            raise RuntimeError("invalid stop request")
        return True
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reserve-gb", type=float)
    parser.add_argument("--report-file", type=pathlib.Path)
    parser.add_argument("--stop-file", type=pathlib.Path)
    parser.add_argument("--stop-id")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if sys.platform != "darwin" or not command:
        parser.error("macOSと実行するコマンドが必要です。")
    if bool(args.stop_file) != bool(args.stop_id) or (args.stop_id and not re.fullmatch(r"[0-9a-f]{32}", args.stop_id)):
        parser.error("stop file and valid instance ID must be supplied together")
    report = {"status": "preflight", "reason": None, "exit_code": None,
              "reserve_gb": None, "minimum_available_gb": None, "scope": "own process group only"}
    write_report(args.report_file, report)
    try:
        total = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True, timeout=5))
        reserve = args.reserve_gb * 2**30 if args.reserve_gb is not None else max(3*2**30, min(10*2**30, total/4))
        free = available_memory()
        if not math.isfinite(reserve) or reserve <= 0 or reserve >= total or free < reserve:
            report.update(status="failed", reason="insufficient_memory", exit_code=125)
            raise RuntimeError("必要な空きメモリがありません。起動していません。")
        report.update(reserve_gb=round(reserve/2**30, 2), minimum_available_gb=round(free/2**30, 2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        if report["reason"] is None:
            report.update(status="failed", reason="monitor_unavailable", exit_code=125)
        write_report(args.report_file, report)
        raise
    # New session prevents signals from reaching the terminal or existing apps.
    try:
        child = subprocess.Popen(command, start_new_session=True)
    except OSError:
        report.update(status="failed", reason="launch_failed", exit_code=125)
        write_report(args.report_file, report)
        raise
    minimum = free
    result = None
    report.update(status="running")
    try:
        write_report(args.report_file, report)
        while child.poll() is None:
            if stop_requested(args.stop_file, args.stop_id):
                result = 0
                report["reason"] = "requested_stop"
                break
            free = available_memory()
            minimum = min(minimum, free)
            if free < reserve:
                print("空きメモリの下限を検出。この処理の子プロセスだけを停止します。", file=sys.stderr)
                result = 125
                report["reason"] = "memory_limit"
                break
            time.sleep(2)
        if result is None:
            result = child.returncode
            report["reason"] = "completed" if result == 0 else "child_failed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        result = 125
        report["reason"] = "monitor_error"
    finally:
        # The launcher can exit before a compiler/worker. A process group still
        # belongs to this launch even after its leader has finished.
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        except OSError:
            result = 125
            report["reason"] = "cleanup_failed"
        if child.poll() is None:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError:
            result = 125
            report["reason"] = "cleanup_failed"
        if child.poll() is None:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                result = 125
                report["reason"] = "cleanup_failed"
        report.update(status="stopped", exit_code=result, minimum_available_gb=round(minimum/2**30, 2))
        write_report(args.report_file, report)
        print(json.dumps(report))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
