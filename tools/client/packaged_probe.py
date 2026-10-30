#!/usr/bin/env python3
"""Staged, retry-safe verification of a guarded Windows packaged GUI client.

Use a fresh run directory. Commands never print or persist codes, JWTs, keys,
or the issued subject. `login` issues one role-specific code and submits it to the
actual GUI client; an ambiguous submission is never retried automatically.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import uuid

import device_login as client
from role_probe import CREATE_FLAGS, JWT_RE, file_sha256, memory_snapshot, process_image

ROOT=Path(__file__).resolve().parents[2]
ADMIN_CLI=ROOT/"Deploy"/"CloudflareAdmin"/"admin-cli.mjs"
SHA256=re.compile(r"[0-9a-f]{64}\Z")
CONTAINER=re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,62}\Z")
SUBJECT_SUFFIX=r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"
ROLES=("player","inspector")
SERVER_ID=re.compile(r"[0-9a-f]{64}\Z")
UTC_TIMESTAMP=re.compile(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?Z\Z")
SERVER_FINGERPRINT=re.compile(r"MRFPS_DTLS_SERVER_SHA256=([0-9a-f]{64})(?=\s|$)")
SERVER_EVENT=re.compile(r"MRFPS_AUTH_ACCEPT role=(?:player|inspector) subject=[A-Za-z0-9._:-]{1,128}(?=\s|$)"
                        r"|MRFPS_JOIN role=(?:player|inspector) slot=(?:-1|0|1) name=[A-Za-z0-9._:-]{1,128}(?=\s|$)")
INSPECT_FORMAT=("{{.Id}}|{{.State.StartedAt}}|{{.RestartCount}}|{{.State.Pid}}|{{.State.Running}}|"
                "{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}")
MAX_LOG_LINE_BYTES=64*1024
MAX_SERVER_EVIDENCE_BYTES=1024*1024
STARTUP_WINDOW_SECONDS=300


def run_safe(command,timeout=25):
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout,creationflags=CREATE_FLAGS)
    if result.returncode: raise RuntimeError("a required local or server operation failed")
    return result.stdout.decode("utf-8","replace")


def docker(*args):
    command=["wsl.exe","-d","Ubuntu","-u","root","--exec","docker",*args]
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=25,creationflags=CREATE_FLAGS)
    if result.returncode: raise RuntimeError("a required local or server operation failed")
    return result.stdout.decode("utf-8","replace")


def docker_log_lines(*args,timeout=25):
    """Stream one finite Docker log query; cap each line and its lifetime.

    The watchdog terminates only our WSL CLI process, never the server. No raw
    log, error text, or potentially sensitive line is written to a file/output.
    """
    command=["wsl.exe","-d","Ubuntu","-u","root","--exec","docker","logs",*args]
    process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                             bufsize=MAX_LOG_LINE_BYTES,creationflags=CREATE_FLAGS)
    expired=threading.Event()
    def stop_owned_process():
        if process.poll() is None:
            expired.set()
            try: process.kill()
            except OSError: pass
    watchdog=threading.Timer(timeout,stop_owned_process); watchdog.daemon=True; watchdog.start()
    try:
        while True:
            line=process.stdout.readline(MAX_LOG_LINE_BYTES+1)
            if not line: break
            if len(line)>MAX_LOG_LINE_BYTES or (len(line)==MAX_LOG_LINE_BYTES and not line.endswith(b"\n")):
                raise RuntimeError("server log line exceeds its safe bound")
            text=line.decode("utf-8","replace")
            if JWT_RE.search(text) or "-----BEGIN PRIVATE KEY-----" in text:
                raise RuntimeError("server log failed privacy check")
            yield text
        code=process.wait()
        if expired.is_set(): raise RuntimeError("server log inspection timed out")
        if code: raise RuntimeError("a required local or server operation failed")
    finally:
        watchdog.cancel()
        if process.poll() is None:
            try: process.kill()
            except OSError: pass
        process.wait()
        if process.stdout: process.stdout.close()


def timestamp(value):
    """Parse Docker UTC RFC3339 timestamps without rounding nanoseconds."""
    match=UTC_TIMESTAMP.fullmatch(value)
    if not match: raise ValueError("invalid server timestamp")
    instant=datetime.datetime.strptime(match.group(1),"%Y-%m-%dT%H:%M:%S")
    delta=instant-datetime.datetime(1970,1,1)
    if delta.days<0: raise ValueError("invalid server timestamp")
    nanos=(delta.days*86400+delta.seconds)*1_000_000_000+int((match.group(2) or "").ljust(9,"0"))
    return instant, nanos


def current_log_line(line,started_ns):
    prefix,separator,_rest=line.partition(" ")
    if not separator: return False
    try: return timestamp(prefix)[1]>=started_ns
    except ValueError: return False


def inspect_server(name):
    fields=docker("inspect","--format",INSPECT_FORMAT,name).strip().split("|")
    if len(fields)!=6 or not SERVER_ID.fullmatch(fields[0]): raise RuntimeError("test server identity is invalid")
    try:
        started,started_ns=timestamp(fields[1])
        restarts,pid=int(fields[2]),int(fields[3])
    except ValueError:
        raise RuntimeError("test server identity is invalid") from None
    if restarts<0 or pid<=0 or fields[4]!="true" or fields[5]!="healthy":
        raise RuntimeError("test server is not healthy")
    return {"id":fields[0],"started":fields[1],"restarts":restarts},pid,started,started_ns


def server_snapshot(name,fingerprint):
    if not CONTAINER.fullmatch(name) or not SHA256.fullmatch(fingerprint): raise ValueError("invalid container or fingerprint")
    identity,pid,started,started_ns=inspect_server(name)
    # The key is logged at startup, not necessarily among the latest 5000
    # entries. Bound the query to this run's first five minutes, while scanning
    # it line by line. Missing/rotated startup logs fail closed.
    fraction=UTC_TIMESTAMP.fullmatch(identity["started"]).group(2)
    until=(started+datetime.timedelta(seconds=STARTUP_WINDOW_SECONDS)).strftime("%Y-%m-%dT%H:%M:%S")
    until+=("."+fraction if fraction else "")+"Z"
    observed=None
    for line in docker_log_lines("--timestamps","--since",identity["started"],"--until",until,identity["id"]):
        if current_log_line(line,started_ns):
            for match in SERVER_FINGERPRINT.finditer(line): observed=match.group(1)
    if observed is None: raise RuntimeError("server startup fingerprint is unavailable")
    if observed!=fingerprint: raise RuntimeError("server fingerprint does not match")
    # Keep only compact authentication/join markers, not arbitrary log lines.
    # The actual query remains the previous 5000-line window, now restricted to
    # the current container start so old admissions cannot be mistaken as new.
    events=[]; event_bytes=0
    for line in docker_log_lines("--timestamps","--since",identity["started"],"--tail","5000",identity["id"]):
        if not current_log_line(line,started_ns): continue
        for match in SERVER_EVENT.finditer(line):
            event=match.group(0)+"\n"; event_bytes+=len(event)
            if event_bytes>MAX_SERVER_EVIDENCE_BYTES: raise RuntimeError("server evidence exceeds its safe bound")
            events.append(event)
    after,after_pid,_after_started,_after_ns=inspect_server(name)
    if after!=identity or after_pid!=pid: raise RuntimeError("test server changed during verification")
    return identity,"".join(events)


def server_join_matches(log,expected_hash,role="inspector"):
    if role not in ROLES or not SHA256.fullmatch(expected_hash or ""): return False
    slot=r"-1" if role=="inspector" else r"[01]"
    accepted=set(re.findall(r"MRFPS_AUTH_ACCEPT role="+role+r" subject=([A-Za-z0-9._:-]{1,128})(?:\s|$)",log))
    joined=set(re.findall(r"MRFPS_JOIN role="+role+r" slot="+slot+r" name=([A-Za-z0-9._:-]{1,128})(?:\s|$)",log))
    return any(hashlib.sha256(subject.encode("ascii")).hexdigest()==expected_hash for subject in accepted & joined)


@contextmanager
def issuer_lock(state_dir,timeout=5):
    """Use admin-cli's exclusive PID lock; never reclaim another owner's lock."""
    directory=state_dir.resolve(strict=True)
    if not directory.is_dir(): raise RuntimeError("issuer directory is unavailable")
    lock=directory/"lock"; deadline=time.monotonic()+timeout
    while True:
        try:
            fd=os.open(lock,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
            break
        except FileExistsError:
            if time.monotonic()>=deadline: raise RuntimeError("issuer state is locked; no code was sent")
            time.sleep(.05)
    owner=str(os.getpid()).encode("ascii"); identity=None
    try:
        identity=os.fstat(fd)
        if os.write(fd,owner)!=len(owner): raise RuntimeError("cannot initialize issuer lock")
        os.fsync(fd)
        yield
    finally:
        os.close(fd)
        if identity is not None:
            try:
                current=lock.stat()
                if (lock.is_symlink() or current.st_size!=len(owner) or
                    (current.st_dev,current.st_ino)!=(identity.st_dev,identity.st_ino) or lock.read_bytes()!=owner):
                    raise RuntimeError("issuer lock changed unexpectedly")
                lock.unlink()
            except FileNotFoundError:
                raise RuntimeError("issuer lock disappeared unexpectedly") from None
        # If fstat failed, leave the unverified lock for admin-cli's stale-lock recovery.


def issuer_snapshot(state_dir,code_path,fingerprint,role="inspector"):
    """Caller holds issuer_lock. Code bytes remain in memory only."""
    if role not in ROLES: raise ValueError("invalid client role")
    directory=state_dir.resolve(strict=True); state_path=directory/"state.json"
    if (state_path.is_symlink() or not state_path.is_file() or state_path.stat().st_size>1024*1024 or
        code_path.is_symlink() or code_path.resolve(strict=True).parent!=directory):
        raise RuntimeError("issuer snapshot files are invalid")
    state=json.loads(state_path.read_text(encoding="utf-8"))
    if not isinstance(state,dict) or not isinstance(state.get("roles"),dict):
        raise RuntimeError("issuer snapshot state is invalid")
    record=state["roles"].get(role,{})
    if not isinstance(record,dict): raise RuntimeError("issuer snapshot state is invalid")
    marker,code=client.submission(code_path)
    subject=record.get("subject",""); owner=state.get("owner",""); issued_id=record.get("id","")
    if (state.get("version")!=1 or state.get("registered") is not True or state.get("stopped") or state.get("stop_pending") or
        not isinstance(owner,str) or not isinstance(issued_id,str) or not isinstance(subject,str) or
        not re.fullmatch(SUBJECT_SUFFIX,owner) or not re.fullmatch(SUBJECT_SUFFIX,issued_id) or
        state.get("fingerprint")!=fingerprint or "pending" in record or
        record.get("code")!=code or not re.fullmatch(role+"-"+SUBJECT_SUFFIX,subject) or
        type(record.get("expires_at")) is not int or record["expires_at"]<=int(time.time())):
        raise RuntimeError("issuer state does not match a fresh published role code")
    return {"marker":marker,"code":code,"subject_hash":hashlib.sha256(subject.encode("ascii")).hexdigest(),
            "owner":state["owner"],"id":record["id"],"expires_at":record["expires_at"],"fingerprint":fingerprint,"role":role}


def issuer_identity(state_dir,code_path,fingerprint,role="inspector"):
    with issuer_lock(state_dir):
        return issuer_snapshot(state_dir,code_path,fingerprint,role)["subject_hash"]


def verify_packaged_binary(path,expected):
    binary=path.resolve(strict=True)
    if binary.name.lower() not in ("mrfps.exe","mrfps-win64-development.exe") or binary.parent.name.lower()!="win64" or binary.parent.parent.name.lower()!="binaries" or binary.parent.parent.parent.name.lower()!="mrfps":
        raise ValueError("select the native Windows/MRFPS/Binaries/Win64 executable")
    paks=binary.parents[2]/"Content"/"Paks"
    if not paks.is_dir() or not any(paks.glob("*.pak")) or not any(paks.glob("*.utoc")):
        raise RuntimeError("packaged game content is missing")
    if not SHA256.fullmatch(expected) or file_sha256(binary)!=expected: raise RuntimeError("packaged binary SHA-256 mismatch")
    return binary


def window_process(name,guard_pid,guard_created):
    user=ctypes.WinDLL("user32",use_last_error=True); kernel=ctypes.WinDLL("kernel32",use_last_error=True)
    user.FindWindowW.argtypes=[wintypes.LPCWSTR,wintypes.LPCWSTR]; user.FindWindowW.restype=wintypes.HWND
    user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
    window=user.FindWindowW(None,name)
    if not window: return None
    owner=wintypes.DWORD(); user.GetWindowThreadProcessId(window,ctypes.byref(owner))
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]; kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    kernel.GetProcessTimes.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME)]
    kernel.GetProcessTimes.restype=wintypes.BOOL
    handle=kernel.OpenProcess(0x1000,False,guard_pid)
    if not handle: raise RuntimeError("guard process is no longer available")
    try:
        created,exited,system,user_time=(wintypes.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle,ctypes.byref(created),ctypes.byref(exited),ctypes.byref(system),ctypes.byref(user_time)) or ((created.dwHighDateTime<<32)|created.dwLowDateTime)!=guard_created:
            raise RuntimeError("guard process identity changed")
    finally: kernel.CloseHandle(handle)
    class Entry(ctypes.Structure):
        _fields_=[("size",wintypes.DWORD),("usage",wintypes.DWORD),("pid",wintypes.DWORD),("heap",ctypes.c_size_t),
                  ("module",wintypes.DWORD),("threads",wintypes.DWORD),("parent",wintypes.DWORD),("priority",wintypes.LONG),("flags",wintypes.DWORD),("exe",wintypes.WCHAR*260)]
    kernel.CreateToolhelp32Snapshot.argtypes=[wintypes.DWORD,wintypes.DWORD]; kernel.CreateToolhelp32Snapshot.restype=wintypes.HANDLE
    kernel.Process32FirstW.argtypes=[wintypes.HANDLE,ctypes.POINTER(Entry)]; kernel.Process32FirstW.restype=wintypes.BOOL
    kernel.Process32NextW.argtypes=[wintypes.HANDLE,ctypes.POINTER(Entry)]; kernel.Process32NextW.restype=wintypes.BOOL
    snapshot=kernel.CreateToolhelp32Snapshot(2,0)
    if snapshot==ctypes.c_void_p(-1).value: raise RuntimeError("cannot inspect process ancestry")
    try:
        entry=Entry(); entry.size=ctypes.sizeof(entry); parents={}; found=kernel.Process32FirstW(snapshot,ctypes.byref(entry))
        while found:
            parents[entry.pid]=entry.parent; found=kernel.Process32NextW(snapshot,ctypes.byref(entry))
    finally: kernel.CloseHandle(snapshot)
    cursor=owner.value
    for _ in range(16):
        if cursor==guard_pid: break
        cursor=parents.get(cursor,0)
        if not cursor: break
    if cursor!=guard_pid: raise RuntimeError("named window is not a descendant of its guard")
    return {"pid":owner.value,"image":str(process_image(owner.value)),"title":name}


def client_args(state,show_settings=False,code_file=None,expected_marker=None,expected_code=None,before_submit=None):
    return argparse.Namespace(server=state["server"],role=state.get("role","inspector"),name=state["name"],
        executable=Path(state["executable"]),editor=False,code_file=code_file,show_settings=show_settings,
        replace=False,auth_test_url="",expected_marker=expected_marker,expected_code=expected_code,before_submit=before_submit)


def save(run,state):
    client.save_state(run/"state.json",state)
    public={k:v for k,v in state.items() if k not in ("subject_hash","code_marker")}
    client.save_state(run/"report.json",public)


def guard_summary(report):
    value=client.load_state(report)
    return {"status":value.get("status"),"child_exit_code":value.get("childExitCode"),"job_assigned":value.get("jobAssigned"),
            "minimum_free_gib":value.get("minimumObservedFreeGB"),"maximum_commit_percent":value.get("maximumObservedCommitPercent")}


def start(run,args,state):
    binary=verify_packaged_binary(Path(args.executable),args.sha256)
    address=client.endpoint(args.server)
    role=getattr(args,"role","inspector")
    if role not in ROLES: raise ValueError("invalid client role")
    server,_=server_snapshot(args.container,args.fingerprint)
    admin=Path(args.admin_dir).resolve(strict=True)
    for path in (admin/"issuer.encrypted.json",admin/"issuer-passphrase.txt",admin/"basic-credentials.json"):
        if not path.is_file(): raise RuntimeError("required protected admin files are unavailable")
    if state:
        expected=(str(binary),args.sha256,address,args.container,args.fingerprint,str(admin),role)
        actual=tuple(state.get(k) for k in ("executable","sha256","server","container","fingerprint","admin_dir","role"))
        if state.get("schema")!=1 or actual!=expected: raise RuntimeError("run directory already identifies a different test")
        if state.get("stage")!="starting": return {"status":"already_started","report":str(run/"report.json")}
    else:
        if any(p.name not in ("state.lock",) for p in run.iterdir()): raise RuntimeError("run directory must be empty")
        run_id=uuid.uuid4().hex
        state={"schema":1,"stage":"starting","name":"MRFPS-Package-"+run_id[:12],"executable":str(binary),"sha256":args.sha256,
               "server":address,"role":role,"container":args.container,"fingerprint":args.fingerprint,"server_identity":server,
               "admin_dir":str(admin),"issuer_dir":str(admin/("package-probe-"+run_id)),"checks":{},"actions":{}}
        save(run,state)
    if state["server_identity"]!=server: raise RuntimeError("test server changed")
    memory=memory_snapshot()
    if memory["free_gib"]<12.5 or memory["commit_percent"]>72:
        raise RuntimeError("insufficient memory headroom to launch the test client")
    state["launch_memory"]=memory; save(run,state)
    result=client.windows(client_args(state,show_settings=True))
    if result["status"] not in ("launching","already_running"): raise RuntimeError("client did not start")
    state["guard_pid"]=result["guard_pid"]
    if result.get("report"): state["guard_report"]=result["report"]
    state["stage"]="launched"; save(run,state)
    return {"status":result["status"],"name":state["name"],"report":str(run/"report.json")}


def inspect(run,state):
    status=client.status(argparse.Namespace(name=state["name"]))
    private=client.load_state(ROOT/"Saved"/"DeviceAuth"/("windows-"+state["name"]+".json"))
    checks=state["checks"]
    events=status.get("events",[])
    role=state.get("role","inspector")
    if "MRFPS_DEVICE_EXCHANGE_VERIFIED role="+role in events: checks["exchange_verified"]=True
    if "MRFPS_DEVICE_CONNECT role="+role in events: checks["connection_requested"]=True
    if "MRFPS_DEVICE_CONTROL role="+role+" has_code=0" in events: checks["settings_applied"]=True
    report=Path(status["guard_report"]); log=report.parent/"engine.log"
    state["guard_report"]=str(report); state["guard_summary"]=guard_summary(report)
    if log.is_file():
        if log.stat().st_size>16*1024*1024: raise RuntimeError("client log exceeds its safe bound")
        contents=log.read_text(encoding="utf-8",errors="replace")
        if JWT_RE.search(contents) or "-----BEGIN PRIVATE KEY-----" in contents: raise RuntimeError("client log failed privacy check")
        if "UPendingNetGame::TravelCompleted Pending net game travel completed" in contents: checks["travel_completed"]=True
    identity,server_log=server_snapshot(state["container"],state["fingerprint"])
    if identity!=state["server_identity"]: raise RuntimeError("server identity changed during verification")
    checks["same_healthy_server"]=True
    if server_join_matches(server_log,state.get("subject_hash",""),role): checks["server_auth_join_matches_issued_subject"]=True
    if status["status"]=="running":
        info=window_process(state["name"],int(private.get("pid",0)),int(private.get("created",0)))
        if info:
            if not os.path.samefile(info["image"],state["executable"]) or file_sha256(Path(info["image"]))!=state["sha256"]:
                raise RuntimeError("window process does not match the selected package and SHA-256")
            state["window_pid"]=info["pid"]; checks["window_image_and_sha_match"]=True
            if not checks.get("connection_requested"): checks["initial_window_title"]=True
            if checks.get("travel_completed") and checks.get("server_auth_join_matches_issued_subject"):
                checks["connected_window_title"]=True
    state["guard_status"]=status["status"]
    save(run,state)
    return {"status":"observed","name":state["name"],"window_pid":state.get("window_pid"),"checks":checks,"report":str(run/"report.json")}


def configure(run,state):
    if state.get("login_intent"): return {"status":"already_configured"}
    observed=inspect(run,state)
    if not observed["checks"].get("initial_window_title"): raise RuntimeError("named packaged window is not ready")
    result=client.windows(client_args(state))
    if "configure" not in state["actions"]: state["actions"]["configure"]=result["status"]
    elif result["status"]=="already_running": state["actions"]["configure_repeated"]=True
    save(run,state)
    return {"status":result["status"]}


def login(run,state):
    if state.get("login_submitted"): return {"status":"already_submitted"}
    if state.get("login_intent"):
        # A prior process may have stopped before it checked the issuer again.
        # The launcher marker alone cannot prove that later check completed.
        raise RuntimeError("code submission outcome is unknown; do not reuse this code")
    if not inspect(run,state)["checks"].get("settings_applied"): raise RuntimeError("settings have not been applied by the client")
    admin=Path(state["admin_dir"]); issuer=Path(state["issuer_dir"])
    role=state.get("role","inspector")
    node=__import__("shutil").which("node")
    if not node: raise RuntimeError("Node.js is unavailable")
    run_safe([node,str(ADMIN_CLI),"ensure","--role",role,"--key-file",str(admin/"issuer.encrypted.json"),
              "--passphrase-file",str(admin/"issuer-passphrase.txt"),"--credentials-file",str(admin/"basic-credentials.json"),
              "--state-dir",str(issuer),"--fingerprint",state["fingerprint"]])
    code=issuer/(role+".code")
    with issuer_lock(issuer):
        snapshot=issuer_snapshot(issuer,code,state["fingerprint"],role)
        state["subject_hash"]=snapshot["subject_hash"]; state["code_marker"]=snapshot["marker"]
        state["login_intent"]=True; save(run,state)
        def validate_snapshot():
            if issuer_snapshot(issuer,code,state["fingerprint"],role)!=snapshot:
                raise RuntimeError("issuer state changed before submission; no new code was sent")
        result=client.windows(client_args(state,code_file=code,expected_marker=snapshot["marker"],
                                          expected_code=snapshot["code"],before_submit=validate_snapshot))
        if result["status"] not in ("login_queued","already_running"): raise RuntimeError("code submission outcome is unknown")
        # A launcher result cannot be evidence for a different submission.
        previous=client.load_submission_state(ROOT/"Saved"/"DeviceAuth"/("windows-"+state["name"]+".json"),"Windows")
        if previous.get("submitted")!=snapshot["marker"] or previous.get("submission_inflight"):
            raise RuntimeError("code submission outcome is unknown; do not reuse this code")
        try:
            validate_snapshot()
        except (OSError,ValueError,RuntimeError) as exc:
            raise RuntimeError("issuer state changed after submission; outcome is unknown; do not reuse this code") from exc
    # The lock must also have been released without any identity mismatch;
    # only then record that the submission and final issuer check completed.
    state["login_submitted"]=True; state["actions"]["login"]=result["status"]; save(run,state)
    return {"status":result["status"]}


def stop(run,state):
    result=client.stop_windows(argparse.Namespace(name=state["name"]))
    previous=state["actions"].get("stop")
    if result["status"]=="stopped": state["actions"]["stop"]="stopped"
    elif result["status"]=="already_stopped" and previous=="stopped": state["actions"]["stop_repeated"]=True
    else: state["actions"]["stop"]=result["status"]
    report=Path(client.status(argparse.Namespace(name=state["name"]))["guard_report"])
    guard=client.load_state(report)
    state["guard_report"]=str(report); state["guard_summary"]=guard_summary(report)
    state["guard_status"]=guard.get("status")
    state["checks"]["graceful_guard_exit"]=guard.get("status")=="completed" and guard.get("childExitCode")==0 and state["actions"].get("stop")=="stopped"
    issuer=Path(state["issuer_dir"])
    if (issuer/"state.json").is_file():
        admin=Path(state["admin_dir"]); node=__import__("shutil").which("node")
        if not node: raise RuntimeError("Node.js is unavailable for issuer cleanup")
        run_safe([node,str(ADMIN_CLI),"stop","--credentials-file",str(admin/"basic-credentials.json"),"--state-dir",str(issuer)])
        state["checks"]["test_issuer_stopped"]=True
    state["stage"]="stopped" if result["status"] in ("stopped","already_stopped") else "close_requested"
    save(run,state); return {"status":result["status"],"report":str(run/"report.json")}


def main():
    parser=argparse.ArgumentParser(description=__doc__); sub=parser.add_subparsers(dest="command",required=True)
    for command in ("start","configure","login","observe","stop"):
        p=sub.add_parser(command); p.add_argument("--run-dir",required=True)
        if command=="start":
            p.add_argument("--executable",required=True); p.add_argument("--sha256",required=True)
            p.add_argument("--server",required=True); p.add_argument("--container",required=True); p.add_argument("--fingerprint",required=True)
            p.add_argument("--role",choices=ROLES,default="inspector")
            p.add_argument("--admin-dir",default=str(Path(os.environ.get("LOCALAPPDATA",""))/"MRFPS"/"Admin"))
    args=parser.parse_args()
    if os.name!="nt": raise RuntimeError("Windows is required")
    run=Path(args.run_dir).resolve(); run.mkdir(parents=True,exist_ok=True)
    with client.state_lock(run/"state.json"):
        state=client.load_state(run/"state.json")
        if args.command=="start": result=start(run,args,state)
        else:
            if state.get("schema")!=1: raise RuntimeError("test has not been started")
            result={"configure":configure,"login":login,"observe":inspect,"stop":stop}[args.command](run,state)
        print(json.dumps(result,ensure_ascii=False)); return 0


if __name__=="__main__":
    try: raise SystemExit(main())
    except (ValueError,RuntimeError,OSError,subprocess.SubprocessError,json.JSONDecodeError) as error:
        print(json.dumps({"status":"error","message":str(error)},ensure_ascii=False),file=sys.stderr); raise SystemExit(1)
