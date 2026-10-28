"""macOS-only memory reserve guard; terminates only its own child process group."""
import argparse
import json
import math
import os
import pathlib
import re
import signal
import subprocess
import sys
import time


def available_memory():
    raw = subprocess.check_output(["vm_stat"], text=True)
    page = re.search(r"page size of (\d+) bytes", raw)
    values = dict(re.findall(r"^(Pages [^:]+):\s+(\d+)\.", raw, re.M))
    required = ("Pages free", "Pages inactive", "Pages speculative")
    if not page or any(key not in values for key in required):
        raise RuntimeError("vm_statの空きメモリを取得できません。起動を中止します。")
    return int(page[1]) * sum(int(values[key]) for key in required)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reserve-gb", type=float)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if sys.platform != "darwin" or not command:
        parser.error("macOSと実行するコマンドが必要です。")
    total = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True))
    reserve = args.reserve_gb * 2**30 if args.reserve_gb is not None else max(3*2**30, min(10*2**30, total/4))
    if not math.isfinite(reserve) or reserve <= 0 or reserve >= total or available_memory() < reserve:
        raise RuntimeError("必要な空きメモリがありません。起動していません。")
    # New session prevents signals from reaching the terminal or existing apps.
    child = subprocess.Popen(command, start_new_session=True)
    minimum = total
    result = None
    try:
        while child.poll() is None:
            free = available_memory()
            minimum = min(minimum, free)
            if free < reserve:
                print("空きメモリの下限を検出。この処理の子プロセスだけを停止します。", file=sys.stderr)
                result = 125
                break
            time.sleep(2)
        if result is None:
            result = child.returncode
    finally:
        # The launcher can exit before a compiler/worker. A process group still
        # belongs to this launch even after its leader has finished.
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        if child.poll() is None:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        if child.poll() is None:
            child.wait()
        print(json.dumps({"exit_code": result, "reserve_gb": round(reserve/2**30, 2), "minimum_available_gb": round(minimum/2**30, 2), "scope": "own process group only"}))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
