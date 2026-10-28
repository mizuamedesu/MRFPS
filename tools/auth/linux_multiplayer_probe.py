"""Existing Linux QA server -> two synthetic desktop fighters/three inspectors.

Uses no headset, physical input, renderer, container mutation, or auth bypass.
The PowerShell entry point supplies the outer 10 GiB/75 percent Windows job guard.
"""
import argparse
import datetime as dt
import json
import os
import pathlib
import re
import subprocess
import time

from linux_security_probe import Probe, JWT_PATTERN, CREATE_FLAGS, save_json, utc, native_executable

QA_SCORE_ARGUMENT = "-ini:Game:[/Script/MRFPS.MRFPSGameMode]:ScoreToWin=1"
QA_AUTO_START_ARGUMENT = "-ini:Game:[/Script/MRFPS.MRFPSGameMode]:bRequireAdminStart=False"


def baseline_checks(server, logs, subjects, privacy, pinned):
    checks = {
        "TwoPlayerSlots": all(re.search(r"MRFPS_JOIN role=player slot="+str(i)+r" name="+re.escape(subjects["player-"+str(i)])+r"\b", server) for i in range(2)),
        "InspectorsJoined": all("MRFPS_JOIN role=inspector slot=-1 name="+subjects["inspector-"+str(i)] in server for i in range(3)),
        "ExtraPlayerRejected": "ARENA_FULL" in logs.get("extra-player", ""),
        "ExactlyTwoPlayerJoins": len(re.findall(r"MRFPS_JOIN role=player\b", server)) == 2,
        "MatchStarted": "MRFPS_MATCH playing" in server,
        "PlayersCalibrated": all(f"MRFPS_CALIBRATED server slot={i} ready=1" in server for i in range(2)),
        "WallSpawned": "MRFPS_WALL" in server,
        "AuthoritativeDamage": "MRFPS_DAMAGE" in server,
        "ShieldBlockedShot": "MRFPS_BLOCK" in server,
        "NoFatalError": not re.search(r"Fatal error:|Assertion failed:|Unhandled Exception:", server+"\n"+"\n".join(logs.values())),
        "PublicKeyAuthenticatedServer": pinned,
        "NoCredentialLeak": privacy,
    }
    for i in range(3):
        text = logs.get("inspector-"+str(i), "")
        checks.update({
            f"Inspector{i}ReceivedTwoPlayers": bool(re.search(r"MRFPS_SNAPSHOT players=2 role=inspector", text)),
            f"Inspector{i}ReceivedWall": bool(re.search(r"MRFPS_SNAPSHOT[^\r\n]*walls=[1-9][0-9]*", text)),
            f"Inspector{i}ReceivedDamage": bool(re.search(r"MRFPS_SNAPSHOT[^\r\n]*p1hp=(?:[0-9]|[1-9][0-9])\.[0-9]", text)),
            f"Inspector{i}ViewedBothPlayers": all(f"MRFPS_VIEW inspector=player slot={slot}" in text for slot in range(2)),
            f"Inspector{i}ReturnedToFreeView": "MRFPS_VIEW inspector=free" in text,
        })
    return checks


def victory_checks(server, logs):
    result = {"ServerElimination": bool(re.search(r"MRFPS_ELIMINATION winner=0 victim=1 kills=[1-9][0-9]*", server)),
              "ServerWinner": "MRFPS_MATCH winner=0" in server}
    for i in range(3):
        result[f"Inspector{i}ReplicatedFinishedScoreboard"] = bool(re.search(
            r"MRFPS_SNAPSHOT players=2 role=inspector phase=2[^\r\n]*kills0=[1-9][0-9]*", logs.get("inspector-"+str(i), "")))
    return result


def snapshot(text):
    matches = list(re.finditer(r"\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]\[\s*\d+\][^\r\n]*MRFPS_SNAPSHOT players=([0-2]) role=(player|inspector)\b", text))
    if not matches:
        return None
    item = matches[-1]
    when = dt.datetime.strptime(item[1], "%Y.%m.%d-%H.%M.%S:%f").replace(tzinfo=dt.timezone.utc)
    return {"at_utc": when.isoformat(), "age_seconds": (dt.datetime.now(dt.timezone.utc)-when).total_seconds(),
            "players": int(item[2]), "role": item[3]}


class Multiplayer(Probe):
    def __init__(self, args):
        super().__init__(args)
        item = json.loads(self.docker("inspect", args.container))[0]
        self.qa_score = QA_SCORE_ARGUMENT in (item.get("Config", {}).get("Cmd") or [])
        if not self.qa_score:
            raise RuntimeError("QA_SCORE_TO_WIN_ONE_OVERRIDE_REQUIRED")
        if QA_AUTO_START_ARGUMENT not in (item.get("Config", {}).get("Cmd") or []):
            raise RuntimeError("QA_SYNTHETIC_COMBAT_AUTO_START_OVERRIDE_REQUIRED")
        self.instances = []
        self.subjects = {}
        self.tokens = []
        self.since = utc()
        self.stopped = self.directory / "STOP"
        self.report.update(scope="Actual Linux-authority synthetic desktop1v1 with three independent NullRHI inspectors; no physical Quest/MR/geometry/haptics or rendered-view claim",
                           phase="linux_multiplayer", maximum_simultaneous_ue_clients=6, cases=[],
                           inspector_count=3, stable_seconds_required=args.stable_seconds,
                           launch_mode="PackagedWindows" if args.packaged else "EditorGame",
                           executable=str(native_executable(args.executable)), qa_score_to_win_one_process_override=True,
                           qa_synthetic_auto_start_process_override=True, administrator_controls_exercised=False,
                           server_fingerprint_verified_before_issue=True,
                           baseline_checks={}, extended_checks={}, stability_started_utc=None, stability_completed_seconds=0,
                           own_clients_cleaned_up=False, whole_objective_complete=False)

    def text(self, path):
        if not path.exists():
            return ""
        if path.stat().st_size > 8*1024*1024:
            raise RuntimeError("CLIENT_LOG_SIZE_LIMIT")
        return path.read_text("utf-8", errors="replace")

    def logs(self):
        return {item["name"]: self.text(item["log"]) for item in self.instances}

    def check_privacy(self, server, logs):
        text = server + "\n" + "\n".join(logs.values())
        if JWT_PATTERN.search(text.encode("utf-8")) or "-----BEGIN PRIVATE KEY-----" in text:
            return False
        return not any(token in text for token in self.tokens)

    def alive(self, require_snapshots=False):
        if self.stopped.exists():
            raise RuntimeError("STOP_REQUESTED")
        for item in self.instances:
            if item["name"] == "extra-player":
                continue
            if item["process"].poll() is not None:
                raise RuntimeError("OWN_CLIENT_EXITED")
            text = self.text(item["log"])
            if re.search(r"Fatal error:|Assertion failed:|Unhandled Exception:|MRFPS_NETWORK_FAILURE|MRFPS_AUTH_(?:STARTUP_FAILED|CLIENT_REJECT|REJECT)", text):
                raise RuntimeError("OWN_CLIENT_RUNTIME_OR_NETWORK_FAILURE")
            if require_snapshots:
                current = snapshot(text)
                expected = "inspector" if item["name"].startswith("inspector") else "player"
                if not current or current["role"] != expected or current["players"] != 2 or not -1 <= current["age_seconds"] <= 3:
                    raise RuntimeError("REPLICATION_SNAPSHOT_STALE_OR_WRONG_ROLE")

    def wait(self, name, condition, timeout=None):
        self.report["stage"] = name
        self.persist()
        deadline = time.monotonic() + (timeout or self.args.startup_timeout)
        while time.monotonic() < deadline:
            self.alive()
            if condition():
                return
            time.sleep(.5)
        raise RuntimeError("STAGE_TIMEOUT")

    def start(self, name):
        role = "inspector" if name.startswith("inspector") else "player"
        path, token, claims = self.token(name, role, ttl=3600)
        self.tokens.append(token)
        self.subjects[name] = claims["sub"]
        log = self.directory / (name+".log")
        stdout_path = self.directory/(name+".stdout.log")
        stderr_path = self.directory/(name+".stderr.log")
        out = stdout_path.open("wb")
        err = stderr_path.open("wb")
        executable = native_executable(self.args.executable)
        command = [str(executable)]
        if not self.args.packaged:
            command += [str(pathlib.Path(self.args.project).resolve()), "-game"]
        command += ["/Game/Maps/MRFPSArena", f"-MRFPSServer={self.args.server}:{self.args.port}", "-MRFPSDesktop",
                    f"-MRFPSAuthTokenFile={path}", f"-MRFPSAuthPublicKeyFile={self.public}", "-MRFPSAuthAudience=mrfps",
                    "-MRFPSSmoke", "-nullrhi", "-nohmd", "-nosound", "-unattended", "-NoSplash", "-NoLiveCoding",
                    "-stdout", "-FullStdOutLogOutput", "-ExecCmds=t.MaxFPS 30", f"-abslog={log}"]
        if role == "inspector":
            command.append("-MRFPSInspector")
        else:
            command += ["-MRFPSAutoReady", "-MRFPSSmokeRole="+("shield" if name == "player-1" else "player")]
        env = dict(os.environ, UE_SKIP_UBT_SDK_SETUP="1", MSBUILDDISABLENODEREUSE="1", DOTNET_PROCESSOR_COUNT="2")
        for key in list(env):
            if key.startswith("MRFPS_AUTH_"):
                env.pop(key)
        try:
            process = subprocess.Popen(command, cwd=pathlib.Path(self.args.project).parent, env=env,
                                       stdout=out, stderr=err, creationflags=CREATE_FLAGS)
        finally:
            out.close(); err.close()
        self.instances.append({"name": name, "process": process, "log": log, "stdout": stdout_path, "stderr": stderr_path, "claims": claims})
        self.report["processes"] = [{"name": item["name"], "pid": item["process"].pid, "log": str(item["log"]),
                                     "token_expires_utc": item["claims"]["exp"], "subject": item["claims"]["sub"]} for item in self.instances]
        self.persist()

    def evaluate(self):
        logs = self.logs()
        server = self.server_logs(self.since)
        all_outputs = dict(logs)
        for item in self.instances:
            all_outputs[item["name"]+"-stdout"] = self.text(item["stdout"])
            all_outputs[item["name"]+"-stderr"] = self.text(item["stderr"])
        privacy = self.check_privacy(server, all_outputs)
        if not privacy:
            # Do not preserve a credential leak as a copied server-evidence log.
            # The native logging policy is expected to prevent it; fail closed.
            self.report["credential_privacy_failed"] = True
        baseline = baseline_checks(server, logs, self.subjects, privacy, True)
        baseline["NoFatalError"] = baseline["NoFatalError"] and not re.search(r"Fatal error:|Assertion failed:|Unhandled Exception:", "\n".join(all_outputs.values()))
        extended = victory_checks(server, logs)
        extended["EveryPrimarySubjectAuthenticated"] = all(
            "MRFPS_AUTH_ACCEPT role="+("inspector" if name.startswith("inspector") else "player")+" subject="+subject+" " in server
            for name, subject in self.subjects.items() if name != "extra-player")
        extended["EveryPrimaryNativeDtlsCompleted"] = all(
            "DTLSCertVerifyCallback: Verified" in logs[name] and "Handshaking completed" in logs[name]
            for name in logs if name != "extra-player")
        extended["NoSessionExpiredDuringStability"] = "MRFPS_AUTH_SESSION_EXPIRED" not in server
        self.report["baseline_checks"] = baseline
        self.report["extended_checks"] = extended
        self.persist()
        if not privacy:
            raise RuntimeError("CREDENTIAL_PRIVACY_FAILED")
        return server, logs, all(baseline.values()) and all(extended.values())

    def execute_multiplayer(self):
        try:
            self.report["status"] = "running"
            # Inspectors first: don't miss the short wall/damage/finished phase.
            for i in range(3):
                self.start("inspector-"+str(i))
                time.sleep(1)
            self.wait("three_inspectors_connected_to_empty_arena", lambda: all(
                "MRFPS_SNAPSHOT players=0 role=inspector" in self.text(item["log"]) for item in self.instances))
            self.start("player-0")
            self.wait("slot0_calibrated", lambda: "MRFPS_CALIBRATED server slot=0 ready=1" in self.server_logs(self.since))
            self.start("player-1")
            self.wait("slot1_calibrated_and_playing", lambda: all(value in self.server_logs(self.since) for value in (
                "MRFPS_CALIBRATED server slot=1 ready=1", "MRFPS_MATCH playing")))
            self.start("extra-player")
            self.wait("extra_player_rejected", lambda: "ARENA_FULL" in self.text(self.instances[-1]["log"]))
            extra = self.instances[-1]["process"]
            if extra.poll() is None:
                extra.kill()
            extra.wait(timeout=10)
            self.wait("combat_observer_and_victory_checks", lambda: self.evaluate()[2])
            self.report["stability_started_utc"] = utc()
            hold = time.monotonic()
            next_server_check = 0
            while time.monotonic()-hold < self.args.stable_seconds:
                self.alive(require_snapshots=True)
                if time.monotonic() >= next_server_check:
                    current = self.state()
                    if current != self.initial:
                        raise RuntimeError("SERVER_IDENTITY_OR_HEALTH_CHANGED")
                    if not self.evaluate()[2]:
                        raise RuntimeError("REGRESSION_DURING_STABILITY")
                    next_server_check = time.monotonic()+5
                self.report["stability_completed_seconds"] = round(time.monotonic()-hold, 3)
                self.persist()
                time.sleep(1)
            self.report["stability_completed_seconds"] = round(time.monotonic()-hold, 3)
            server, logs, checks = self.evaluate()
            self.report["final_container"] = self.state()
            self.report["same_healthy_container"] = self.report["final_container"] == self.initial
            self.report["passed"] = checks and self.report["same_healthy_container"] and self.report["stability_completed_seconds"] >= self.args.stable_seconds
            self.report["status"] = "completed"
            if self.report["passed"]:
                (self.directory/"server-evidence.log").write_text(server, encoding="utf-8")
        except Exception as error:
            self.report.update(passed=False, status="failed", exception_type=type(error).__name__)
            # Only fixed internal codes; never exception text from OS/log input.
            if type(error) is RuntimeError and re.fullmatch(r"[A-Z0-9_]{1,80}", str(error)):
                self.report["safe_code"] = str(error)
        finally:
            cleanup_errors = []
            for item in reversed(self.instances):
                try:
                    if item["process"].poll() is None:
                        item["process"].kill()
                    item["process"].wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired) as error:
                    cleanup_errors.append(type(error).__name__)
            self.report["own_clients_cleaned_up"] = all(item["process"].poll() is not None for item in self.instances)
            self.report["own_client_cleanup_errors"] = cleanup_errors
            if not self.report["own_clients_cleaned_up"]:
                self.report.update(passed=False, status="failed", safe_code="OWN_CLIENT_CLEANUP_FAILED")
            self.report["sdk_descendants_cleanup"] = "Outer resource guard's kill-on-close Windows job"
            self.report["finished_utc"] = utc()
            self.persist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("executable", "project", "private-key", "public-key", "fingerprint", "report-directory"):
        parser.add_argument("--"+name, required=True)
    parser.add_argument("--server", default="172.23.166.33")
    parser.add_argument("--port", type=int, default=17779)
    parser.add_argument("--container", default="linux-mrfps-1")
    parser.add_argument("--passphrase-file")
    parser.add_argument("--dev-unencrypted", action="store_true")
    parser.add_argument("--packaged", action="store_true")
    parser.add_argument("--stable-seconds", type=int, choices=range(300,601), default=300, metavar="300..600")
    parser.add_argument("--startup-timeout", type=int, default=180)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    args.phase = "remote_multiplayer"
    args.legacy_protocol2 = False
    if args.dry_run:
        plan = {"status": "prepared_only", "passed": False, "whole_objective_complete": False,
                "ue_processes_started": False, "docker_commands_run": False, "issuer_key_read": False,
                "launch_mode": "PackagedWindows" if args.packaged else "EditorGame", "maximum_ue_clients": 6,
                "primary_clients": ["inspector-0","inspector-1","inspector-2","player-0","player-1"],
                "extra_player_rejection": True, "baseline_check_count": 27, "stability_seconds": args.stable_seconds,
                "required_qa_server_arguments": [QA_SCORE_ARGUMENT, QA_AUTO_START_ARGUMENT], "token_ttl_seconds": 3600,
                "administrator_controls_exercised": False}
        directory = pathlib.Path(args.report_directory)
        directory.mkdir(parents=True, exist_ok=False)
        save_json(directory/"plan.json", plan)
        print(json.dumps(plan))
        return 0
    instance = None
    try:
        instance = Multiplayer(args)
        instance.execute_multiplayer()
        return 0 if instance.report["passed"] else 1
    except Exception as error:
        code = str(error) if type(error) is RuntimeError and re.fullmatch(r"[A-Z0-9_]{1,80}", str(error)) else "LINUX_MULTIPLAYER_PREPARATION_FAILED"
        print(json.dumps({"status":"failed", "safe_code":code, "exception_type":type(error).__name__}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
