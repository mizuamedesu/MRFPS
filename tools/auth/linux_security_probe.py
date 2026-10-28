"""Serial real UE clients against an existing Linux server; no packet dumps.

Run only inside the project's resource guard. Tokens and the issuer key remain
host-only. Each proxy owns one loopback endpoint and forwards one client's UDP.
"""
import argparse
import contextlib
import datetime as dt
import json
import os
import pathlib
import re
import selectors
import socket
import subprocess
import threading
import time
import uuid

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import mrfps_tokens as auth

JWT_PATTERN = re.compile(rb"eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{80,}")
CREATE_FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
EXPECTED_CASE_COUNTS = {"lifecycle": 9, "rejections": 24, "all": 33}
LEGACY_CASE_COUNTS = {"lifecycle": 6, "rejections": 16, "all": 22}


def expected_case_count(args):
    return (LEGACY_CASE_COUNTS if args.legacy_protocol2 else EXPECTED_CASE_COUNTS).get(args.phase)


def native_executable(path):
    path = pathlib.Path(path).resolve()
    if path.name.lower() == "mrfps.exe":
        for name in ("MRFPS.exe", "MRFPS-Win64-Development.exe"):
            candidate = path.parent / "MRFPS" / "Binaries" / "Win64" / name
            if candidate.is_file():
                return candidate
    return path


def client_command(args, token_path, public_path, role, audience, proxy_port, log):
    command = [str(native_executable(args.executable))]
    if not args.packaged:
        command += [str(pathlib.Path(args.project).resolve()), "-game"]
    command += ["/Game/Maps/MRFPSArena", f"-MRFPSServer=127.0.0.1:{proxy_port}", "-MRFPSDesktop", "-MRFPSSmoke",
                f"-MRFPSAuthTokenFile={token_path}", f"-MRFPSAuthPublicKeyFile={public_path}",
                f"-MRFPSAuthAudience={audience}", "-nullrhi", "-nohmd", "-nosound", "-unattended",
                "-NoSplash", "-NoLiveCoding", "-stdout", "-FullStdOutLogOutput", "-ExecCmds=t.MaxFPS 30", f"-abslog={log}"]
    if role == "inspector":
        command.append("-MRFPSInspector")
    elif role == "admin":
        command.append("-MRFPSAdmin")
    if args.legacy_protocol2:
        if args.packaged:
            raise RuntimeError("LEGACY_PROTOCOL2_REQUIRES_EDITOR_MODE")
        command.append("-DisablePlugins=ProceduralMeshComponent")
    return command


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def save_json(path, value):
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("x", encoding="utf-8") as output:
            json.dump(value, output, indent=2)
            output.flush()
            os.fsync(output.fileno())
        for attempt in range(10):
            try:
                os.replace(temporary, path)
                return
            except PermissionError:
                if attempt == 9:
                    raise
                time.sleep(.025)
    finally:
        with contextlib.suppress(OSError):
            temporary.unlink()


def alignments(packet):
    yield packet
    for shift in range(1, 8):
        yield bytes(((packet[i] >> shift) | (packet[i + 1] << (8 - shift))) & 255
                    for i in range(len(packet) - 1))


def dtls_records(packet):
    """Recognize complete DTLS1.x records under Unreal's LSB-first bit framing.

    This inspects public record headers only, never decrypts an application
    record. Candidate lengths/versions/epochs must all fit the actual datagram.
    """
    records = set()
    for shift, aligned in enumerate(alignments(packet)):
        for offset in range(max(0, len(aligned) - 12)):
            if aligned[offset] not in (20, 21, 22, 23) or aligned[offset+1:offset+3] not in (b"\xfe\xff", b"\xfe\xfd"):
                continue
            epoch = int.from_bytes(aligned[offset+3:offset+5], "big")
            length = int.from_bytes(aligned[offset+11:offset+13], "big")
            if 0 < length <= 18432 and offset + 13 + length <= len(aligned):
                if aligned[offset] != 23 or (epoch > 0 and length >= 16):
                    records.add((shift, offset, aligned[offset], epoch, length))
    return records


class PacketProxy:
    def __init__(self, server, port, token):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.upstream.connect((server, port))
        self.client = None
        self.token = token.encode("ascii") if token else b""
        self.stop_event = threading.Event()
        self.stats = {"client_datagrams": 0, "server_datagrams": 0, "bytes": 0,
                      "dtls_handshake_records": 0, "dtls_encrypted_application_records": 0,
                      "plaintext_exact_token_matches": 0, "plaintext_token_fragment_matches": 0,
                      "jwt_pattern_matches": 0, "foreign_client_datagrams_dropped": 0,
                      "packet_limit_reached": False, "packet_payloads_persisted": False,
                      "tls_decryption_performed": False}
        self.thread = threading.Thread(target=self._pump, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def _inspect(self, data):
        self.stats["bytes"] += len(data)
        exact = fragments = False
        patterns = 0
        wide = self.token.decode("ascii").encode("utf-16-le")
        for aligned in alignments(data):
            exact |= bool(self.token and (self.token in aligned or wide in aligned))
            fragments |= bool(self.token and (self.token[:40] in aligned or self.token[-40:] in aligned
                                               or wide[:80] in aligned or wide[-80:] in aligned))
            patterns += len(JWT_PATTERN.findall(aligned))
        self.stats["plaintext_exact_token_matches"] += int(exact)
        self.stats["plaintext_token_fragment_matches"] += int(fragments)
        self.stats["jwt_pattern_matches"] += patterns
        for _, _, kind, _, _ in dtls_records(data):
            self.stats["dtls_handshake_records"] += int(kind == 22)
            self.stats["dtls_encrypted_application_records"] += int(kind == 23)
        # Refuse to forward a credential accidentally emitted in cleartext.
        return not (exact or fragments or patterns)

    def _pump(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.listener, selectors.EVENT_READ, "client")
            selector.register(self.upstream, selectors.EVENT_READ, "server")
            while not self.stop_event.is_set():
                for key, _ in selector.select(.1):
                    try:
                        data, endpoint = key.fileobj.recvfrom(65535)
                        if key.data == "client":
                            if self.client is None:
                                self.client = endpoint
                            if endpoint != self.client:
                                self.stats["foreign_client_datagrams_dropped"] += 1
                                continue
                        self.stats[key.data + "_datagrams"] += 1
                        if self.stats["client_datagrams"] + self.stats["server_datagrams"] > 200000:
                            self.stats["packet_limit_reached"] = True
                            self.stop_event.set()
                            break
                        if not self._inspect(data):
                            continue
                        if key.data == "client":
                            self.upstream.send(data)
                        elif self.client:
                            self.listener.sendto(data, self.client)
                    except OSError:
                        continue

    def __exit__(self, *_):
        self.stop_event.set()
        self.thread.join(2)
        self.listener.close()
        self.upstream.close()


class Probe:
    def __init__(self, args):
        self.args = args
        self.directory = pathlib.Path(args.report_directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=False)
        self.credentials = self.directory / "credentials"
        self.credentials.mkdir()
        self.key = auth.load_private(pathlib.Path(args.private_key),
                                     pathlib.Path(args.passphrase_file) if args.passphrase_file else None,
                                     args.dev_unencrypted)
        self.public = pathlib.Path(args.public_key).resolve()
        if auth.public_pem(self.key.public_key()) != self.public.read_bytes():
            raise RuntimeError("ISSUER_PUBLIC_KEY_MISMATCH")
        self.attacker_key = Ed25519PrivateKey.generate()
        self.attacker_public = self.credentials / "attacker-public.pem"
        auth.secure_write(self.attacker_public, auth.public_pem(self.attacker_key.public_key()))
        self.run_id = uuid.uuid4().hex[:10]
        self.process = None
        self.report = {"started_utc": utc(), "passed": False, "status": "prepared", "phase": args.phase,
                       "scope": "Serial actual Windows NullRHI clients against existing Linux Docker; client, nativeTLS and server rejection scopes remain distinct",
                       "whole_objective_complete": False, "quest_device_used": False,
                       "maximum_simultaneous_ue_clients": 1, "container_recreated": False,
                       "server": args.server, "port": args.port, "expected_fingerprint": args.fingerprint,
                       "public_key_file": str(self.public), "cases": []}
        self.report.update(launch_mode="PackagedWindows" if args.packaged else "EditorGame",
                           executable=str(native_executable(args.executable)),
                           expected_case_count=expected_case_count(args))
        self.report["disabled_plugins"] = ["ProceduralMeshComponent"] if args.legacy_protocol2 else []
        self.initial = self.state()
        logs = self.server_logs()
        matches = re.findall(r"MRFPS_AUTH_TRANSPORT_READY fingerprint=([0-9a-f]{64})", logs)
        if not self.initial["running"] or self.initial["health"] != "healthy" or not matches or matches[-1] != args.fingerprint:
            raise RuntimeError("PINNED_SERVER_NOT_HEALTHY")
        self.report["initial_container"] = self.initial
        self.persist()

    def docker(self, *arguments):
        result = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--exec", "docker", *arguments],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20, creationflags=CREATE_FLAGS)
        if result.returncode:
            raise RuntimeError("DOCKER_READ_FAILED")
        return (result.stdout + result.stderr).decode("utf-8", "replace")

    def state(self):
        item = json.loads(self.docker("inspect", self.args.container))[0]
        return {"id": item["Id"], "image": item["Image"], "started_at": item["State"]["StartedAt"],
                "running": item["State"]["Running"], "health": item["State"]["Health"]["Status"],
                "restarts": item["RestartCount"], "oom_killed": item["State"]["OOMKilled"]}

    def server_logs(self, since=None):
        return self.docker("logs", *(["--since", since] if since else []), self.args.container)

    def persist(self):
        self.report["updated_utc"] = utc()
        save_json(self.directory / "report.json", self.report)

    def token(self, name, role, *, key=None, audience="mrfps", ttl=600, expired=False, wrong_pin=False):
        subject = "linux-sec-" + self.run_id + "-" + name
        token, claims = auth.mint(key or self.key, subject, role,
                                  "0" * 64 if wrong_pin else self.args.fingerprint,
                                  audience=audience, ttl=ttl)
        if expired:
            now = int(time.time())
            claims.update(iat=now-180, nbf=now-180, exp=now-120)
            token = auth.encode_token(key or self.key, claims)
        path = self.credentials / (name + ".jwt")
        auth.secure_write(path, token.encode("ascii"))
        return path, token, claims

    def case(self, name, role, credential, *, expected="accept", code=None, public=None, audience="mrfps"):
        path, token, claims = credential
        since = utc()
        log = self.directory / (name + ".log")
        stdout = self.directory / (name + ".stdout.log")
        stderr = self.directory / (name + ".stderr.log")
        record = {"name": name, "role": role, "subject": claims.get("sub"), "expected_scope": expected,
                  "expected_code": code, "started_utc": since, "passed": None, "status": "running", "client_log": str(log),
                  "client_public_key_file": str(public or self.public), "client_expected_audience": audience,
                  "server_expected_audience": "mrfps",
                  "token_file": str(path), "token_expires_utc": claims.get("exp"), "raw_token_recorded": False}
        self.report["cases"].append(record)
        self.report["status"] = "running"
        self.persist()
        env = dict(os.environ, UE_SKIP_UBT_SDK_SETUP="1", MSBUILDDISABLENODEREUSE="1", DOTNET_PROCESSOR_COUNT="2")
        for name_env in ("MRFPS_AUTH_PUBLIC_KEY_B64", "MRFPS_AUTH_PUBLIC_KEY_PEM", "MRFPS_AUTH_AUDIENCE"):
            env.pop(name_env, None)
        server = ""
        def client_text():
            return log.read_text("utf-8", errors="replace") if log.exists() else ""
        with PacketProxy(self.args.server, self.args.port, token) as proxy:
            command = client_command(self.args, path, public or self.public, role, audience, proxy.port, log)
            with stdout.open("wb") as out, stderr.open("wb") as err:
                self.process = subprocess.Popen(command, cwd=pathlib.Path(self.args.project).parent, env=env,
                                                stdout=out, stderr=err, creationflags=CREATE_FLAGS)
                record["client_pid"] = self.process.pid
                self.persist()
                deadline = time.monotonic() + (100 if expected == "expiry" else 65)
                accepted_at = None
                try:
                    while time.monotonic() < deadline:
                        time.sleep(.5)
                        text = client_text()
                        if self.process.poll() is not None:
                            record["unexpected_process_exit"] = self.process.returncode
                            break
                        if any(marker in text for marker in ("Fatal error:", "Assertion failed:", "Unhandled Exception:")):
                            record["fatal_error"] = True
                            break
                        if expected == "client" and code in text:
                            break
                        if expected == "native_tls" and "Fingerprint validation failure" in text:
                            break
                        # Server polling is metadata-only; at most one UE client exists.
                        server = self.server_logs(since)
                        accepted = bool(claims.get("sub") and f"MRFPS_AUTH_ACCEPT role={role} subject={claims['sub']} " in server)
                        joined = bool(claims.get("sub") and f"MRFPS_JOIN role={role} " in server and f"name={claims['sub']}" in server)
                        if accepted and joined and accepted_at is None:
                            accepted_at = time.monotonic()
                        if expected == "accept" and accepted_at and time.monotonic() - accepted_at >= 3:
                            break
                        if expected == "server" and re.search(r"MRFPS_AUTH_REJECT code=" + re.escape(code) + r" role=" + role + r"\b", server):
                            break
                        if expected == "expiry" and accepted_at and "MRFPS_AUTH_SESSION_EXPIRED" in server and (
                                "MRFPS_NETWORK_FAILURE" in text or "?closed" in text or "AUTH_EXPIRED" in text):
                            break
                finally:
                    if self.process.poll() is None:
                        self.process.kill()
                    self.process.wait(timeout=10)
                    self.process = None
            record["packet_inspection"] = dict(proxy.stats)
        text = client_text()
        server = self.server_logs(since)
        accepted = bool(claims.get("sub") and f"MRFPS_AUTH_ACCEPT role={role} subject={claims['sub']} " in server)
        joined = bool(claims.get("sub") and f"name={claims['sub']}" in server and f"MRFPS_JOIN role={role} " in server)
        server_rejected = bool(code and re.search(r"MRFPS_AUTH_REJECT code=" + re.escape(code) + r" role=" + role + r"\b", server))
        record.update(server_accepted=accepted, server_joined=joined, server_expected_rejection_observed=server_rejected,
                      client_expected_code_observed=bool(code and code in text),
                      native_certificate_verified="DTLSCertVerifyCallback: Verified" in text,
                      native_handshake_complete="Handshaking completed" in text,
                      native_pin_rejected="Fingerprint validation failure" in text,
                      server_expired_session="MRFPS_AUTH_SESSION_EXPIRED" in server,
                      client_disconnect_observed=bool(re.search(r"MRFPS_NETWORK_FAILURE|\?closed|AUTH_EXPIRED", text)))
        privacy = True
        for filename in (log, stdout, stderr):
            data = filename.read_bytes() if filename.exists() else b""
            found = bool(JWT_PATTERN.search(data) or (token and token.encode("ascii") in data))
            if found:
                privacy = False
                if token:
                    data = data.replace(token.encode("ascii"), b"[REDACTED_CREDENTIAL]")
                filename.write_bytes(JWT_PATTERN.sub(b"[REDACTED_CREDENTIAL]", data))
        packet = record["packet_inspection"]
        wire_private = not any(packet[name] for name in ("plaintext_exact_token_matches", "plaintext_token_fragment_matches", "jwt_pattern_matches", "packet_limit_reached"))
        if expected == "accept":
            passed = accepted and joined and record["native_handshake_complete"] and packet["dtls_encrypted_application_records"] > 0
        elif expected == "expiry":
            passed = accepted and joined and record["server_expired_session"] and record["client_disconnect_observed"]
        elif expected == "server":
            passed = server_rejected and not joined
        elif expected == "native_tls":
            passed = record["native_pin_rejected"] and not accepted and not joined
        else:
            passed = record["client_expected_code_observed"] and not accepted and not joined
        packet_observation = packet["client_datagrams"] + packet["server_datagrams"] > 0
        record.update(passed=bool(passed and privacy and wire_private and not record.get("fatal_error")),
                      no_credential_in_client_logs=privacy, packet_observation_performed=packet_observation,
                      no_plaintext_credential_on_wire=wire_private if packet_observation else None,
                      jwt_receipt_on_server_proven=bool(accepted or server_rejected), finished_utc=utc(), client_cleaned_up=True)
        record["bootstrap_failure_without_auth_evidence"] = bool(record.get("unexpected_process_exit") is not None
            and not packet_observation and not record["client_expected_code_observed"] and not record["native_pin_rejected"]
            and not accepted and not server_rejected)
        record["status"] = "passed" if record["passed"] else "failed"
        record["safe_server_markers"] = [line for line in server.splitlines() if re.search(
            r"MRFPS_AUTH_(?:ACCEPT|REJECT|SESSION_EXPIRED)|MRFPS_JOIN", line) and not JWT_PATTERN.search(line.encode("utf-8"))][-20:]
        self.persist()
        print(json.dumps({"case": name, "passed": record["passed"], "scope": expected, "role": role}), flush=True)
        if not record["passed"]:
            raise RuntimeError("SECURITY_CASE_FAILED")

    def execute(self):
        for role in (("player", "inspector") if self.args.legacy_protocol2 else ("player", "inspector", "admin")):
            if self.args.phase in ("lifecycle", "all"):
                token = self.token(role + "-valid", role)
                self.case(role + "-valid", role, token)
                if role == "player":
                    # A killed UDP client has no graceful Logout packet. With
                    # another real player online its 30s server timeout occupies
                    # the second slot, so replay would test ARENA_FULL instead.
                    # Wait past the configured timeout; the next case still
                    # requires the actual authoritative AUTH_REPLAY marker.
                    self.report["status"] = "waiting_for_previous_player_disconnect"
                    self.report["player_disconnect_cooldown_seconds"] = 35
                    self.persist()
                    deadline = time.monotonic() + 35
                    while time.monotonic() < deadline:
                        if (self.directory / "STOP").exists():
                            raise RuntimeError("STOP_REQUESTED")
                        time.sleep(0.2)
                self.case(role + "-replay", role, token, expected="server", code="AUTH_REPLAY")
                self.case(role + "-expiry", role, self.token(role + "-expiry", role, ttl=35), expected="expiry")
            if self.args.phase in ("rejections", "all"):
                for name, code, options in (
                        ("fake", "AUTH_INVALID", {"key": self.attacker_key}),
                        ("expired", "AUTH_EXPIRED", {"expired": True}),
                        ("audience", "AUTH_AUDIENCE", {"audience": "wrong-audience"})):
                    self.case(role + "-client-" + name, role, self.token(role + "-" + name, role, **options), expected="client", code=code)
                opposite = "inspector" if role == "player" else "player"
                self.case(role + "-client-role", role, self.token(role + "-wrong-role", opposite), expected="client", code="AUTH_ROLE")
                missing = self.credentials / (role + "-missing.jwt")
                self.case(role + "-client-missing", role, (missing, "", {}), expected="client", code="AUTH_MISSING")
                self.case(role + "-native-pin", role, self.token(role + "-pin", role, wrong_pin=True), expected="native_tls", code="AUTH_TRANSPORT")
                self.case(role + "-server-forged", role, self.token(role + "-forged", role, key=self.attacker_key),
                          expected="server", code="AUTH_INVALID", public=self.attacker_public)
                self.case(role + "-server-audience", role, self.token(role + "-server-audience", role, audience="wrong-audience"),
                          expected="server", code="AUTH_AUDIENCE", audience="wrong-audience")
        self.report["final_container"] = self.state()
        self.report["same_healthy_container"] = self.report["final_container"] == self.initial
        self.report["passed"] = (self.report["same_healthy_container"]
                                 and len(self.report["cases"]) == expected_case_count(self.args)
                                 and all(case["passed"] for case in self.report["cases"]))
        self.report["status"] = "completed"
        self.report["finished_utc"] = utc()
        self.persist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("executable", "project", "private-key", "public-key", "fingerprint", "report-directory"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--server", default="172.23.166.33")
    parser.add_argument("--port", type=int, default=17779)
    parser.add_argument("--container", default="linux-mrfps-1")
    parser.add_argument("--phase", choices=("lifecycle", "rejections", "all"), default="lifecycle")
    parser.add_argument("--passphrase-file", help="Passphrase file for an encrypted issuer key; contents are never logged")
    parser.add_argument("--dev-unencrypted", action="store_true", help="Explicit fixture-only unencrypted key opt-in")
    parser.add_argument("--legacy-protocol2", action="store_true", help="Explicitly omit only the unbuilt ProceduralMeshComponent visualization plugin while testing a frozen protocol2 Editor DLL")
    parser.add_argument("--packaged", action="store_true", help="Native Windows package: no .uproject or -game argument")
    parser.add_argument("--dry-run", action="store_true", help="Write a plan without opening keys, contacting Docker, or starting UE")
    args = parser.parse_args()
    if args.packaged and args.legacy_protocol2:
        parser.error("--legacy-protocol2 is restricted to the frozen Editor mode")
    if args.dry_run:
        plan = {"status": "prepared_only", "passed": False, "whole_objective_complete": False,
                "ue_processes_started": False, "docker_commands_run": False, "issuer_key_read": False,
                "launch_mode": "PackagedWindows" if args.packaged else "EditorGame", "phase": args.phase,
                "maximum_simultaneous_ue_clients": 1, "expected_case_count": expected_case_count(args),
                "roles": ["player", "inspector"], "local_rejection_is_server_evidence": False,
                "raw_packets_persisted": False, "packet_tls_decryption": False, "native_crypto_disabled": False}
        directory = pathlib.Path(args.report_directory)
        directory.mkdir(parents=True, exist_ok=False)
        save_json(directory/"plan.json", plan)
        print(json.dumps(plan))
        return 0
    probe = None
    try:
        probe = Probe(args)
        probe.execute()
        return 0 if probe.report["passed"] else 1
    except Exception as error:
        if probe:
            if probe.process and probe.process.poll() is None:
                probe.process.kill()
                probe.process.wait(timeout=10)
            probe.report.update(status="failed", exception_type=type(error).__name__, finished_utc=utc())
            probe.persist()
        print(json.dumps({"status": "failed", "safe_code": "LINUX_SECURITY_PROBE_FAILED", "exception_type": type(error).__name__}), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
