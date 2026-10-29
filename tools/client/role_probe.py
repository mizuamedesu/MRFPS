#!/usr/bin/env python3
"""Guarded, retry-safe live admission test against an explicitly named container.

The PowerShell entry point supplies the 10 GiB/75% job guard. Each case uses a
fresh production Worker device-code exchange; no raw code or JWT is printed.
"""
from __future__ import annotations

import argparse
import base64
import ctypes
import datetime as dt
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid
from ctypes import wintypes

from device_login import state_lock

ROOT=Path(__file__).resolve().parents[2]
ADMIN_CLI=ROOT / "Deploy" / "CloudflareAdmin" / "admin-cli.mjs"
JWT_RE=re.compile(r"eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{80,}")
SUBJECT_RE=re.compile(r"[A-Za-z0-9._:-]{1,128}\Z")
FINGERPRINT_RE=re.compile(r"[0-9a-f]{64}\Z")
CREATE_FLAGS=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0


class StopRequested(Exception): pass
class ResourceLimit(Exception): pass


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary=path.with_name(path.name+"."+uuid.uuid4().hex+".tmp")
    try:
        temporary.write_text(json.dumps(value,indent=2,ensure_ascii=False),encoding="utf-8")
        os.replace(temporary,path)
    finally:
        temporary.unlink(missing_ok=True)


def safe_subject(path: Path, role: str, fingerprint: str) -> str:
    token=path.read_text(encoding="ascii").strip()
    pieces=token.split(".")
    if len(pieces)!=3 or not JWT_RE.fullmatch(token):
        raise RuntimeError("exchanged token has invalid structure")
    claims=json.loads(base64.urlsafe_b64decode(pieces[1]+"="*((-len(pieces[1]))%4)))
    subject=claims.get("sub","")
    if not SUBJECT_RE.fullmatch(subject) or claims.get("role")!=role or claims.get("dtls_fp")!=fingerprint:
        raise RuntimeError("exchanged token has wrong role, identity, or server binding")
    return subject


def memory_snapshot() -> dict:
    class Status(ctypes.Structure):
        _fields_=[("length",wintypes.DWORD),("load",wintypes.DWORD),
                  ("total_phys",ctypes.c_ulonglong),("avail_phys",ctypes.c_ulonglong),
                  ("total_page",ctypes.c_ulonglong),("avail_page",ctypes.c_ulonglong),
                  ("total_virtual",ctypes.c_ulonglong),("avail_virtual",ctypes.c_ulonglong),
                  ("extended",ctypes.c_ulonglong)]
    if os.name!="nt": raise RuntimeError("Windows is required")
    data=Status(); data.length=ctypes.sizeof(data)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(data)):
        raise RuntimeError("cannot read system memory")
    return {"free_gib":data.avail_phys/1024**3,
            "commit_percent":100*(data.total_page-data.avail_page)/data.total_page if data.total_page else 100}


def ensure_private_dir(path: Path) -> None:
    path.mkdir(parents=True,exist_ok=True)
    who=subprocess.run(["whoami.exe","/user","/fo","csv","/nh"],capture_output=True,text=True,check=True,timeout=10)
    match=re.search(r"S-1-(?:\d+-)+\d+",who.stdout)
    if not match: raise RuntimeError("cannot establish private directory ACL")
    subprocess.run(["icacls.exe",str(path),"/inheritance:r","/grant:r",f"*{match.group(0)}:(OI)(CI)F"],
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True,timeout=10)


class Probe:
    def __init__(self,args):
        self.args=args; self.run=Path(args.run_dir).resolve(); self.report_file=self.run/"report.json"; self.stop=self.run/"STOP"
        self.executable=Path(args.executable).resolve(strict=True)
        if not args.editor and self.executable.name.lower()=="mrfps.exe":
            for name in ("MRFPS.exe","MRFPS-Win64-Development.exe"):
                native=self.executable.parent/"MRFPS"/"Binaries"/"Win64"/name
                if native.is_file(): self.executable=native; break
        self.admin=Path(args.admin_dir).resolve(strict=True)
        self.issuer=self.admin/"issuer-public.pem"; self.key=self.admin/"issuer.encrypted.json"; self.passphrase=self.admin/"issuer-passphrase.txt"
        self.credentials=self.admin/"basic-credentials.json"; self.issuer_state=self.admin/"cli-issuer"
        self.private=self.admin/("capacity-"+uuid.uuid4().hex); self.exchange_state=self.private/"exchanges"
        self.node=shutil.which("node")
        if not self.node or not self.issuer.is_file() or not self.key.is_file() or not self.passphrase.is_file() or not self.credentials.is_file():
            raise RuntimeError("admin credentials or Node.js are unavailable")
        self.children=[]; self.operations=[]; self.events=[]
        self.control_prefix="MRFPS-Role-"+uuid.uuid4().hex[:8]
        self.report={"schema":1,"status":"running","stage":"preflight","server":args.server,"port":args.port,
                     "container":args.container,"fingerprint":args.fingerprint,"requestedInspectors":args.inspectors,
                     "minimumFreeGB":10,"maximumCommitPercent":75,"checks":{},"instances":[],"limitations":[],"clientsCleaned":False,
                     "credentialStateDirectory":str(self.exchange_state),"exchangeOperations":[],"temporaryCredentialsCleaned":False}

    def persist(self):
        self.report["events"]=self.events[-40:]
        self.report["memory"]=memory_snapshot()
        atomic_json(self.report_file,self.report)

    def check_stop(self):
        if self.stop.exists(): raise StopRequested()
        for item in self.children:
            if item["required"] and item["process"].poll() is not None:
                raise RuntimeError("an accepted client exited during the test")

    def set_stage(self,stage):
        self.check_stop(); self.report["stage"]=stage; self.persist()

    def require_memory(self):
        sample=memory_snapshot()
        # A headless client is started only with a cushion above the guard's
        # hard minimum. The guard independently monitors every descendant.
        if sample["free_gib"]<12.5 or sample["commit_percent"]>72:
            raise ResourceLimit("insufficient headroom for another real client")

    def docker(self,*args):
        command=["wsl.exe","-d","Ubuntu","-u","root","--exec","docker",*args]
        process=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20,creationflags=CREATE_FLAGS)
        if process.returncode: raise RuntimeError("cannot read named container state or logs")
        result=process.stdout+(process.stderr if args and args[0]=="logs" else b"")
        return result.decode("utf-8","replace")

    def logs(self):
        text=self.docker("logs","--tail","5000",self.args.container)
        if JWT_RE.search(text) or "-----BEGIN PRIVATE KEY-----" in text:
            raise RuntimeError("server log failed credential-privacy check")
        return text

    def preflight(self):
        if not FINGERPRINT_RE.fullmatch(self.args.fingerprint) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,62}",self.args.container):
            raise ValueError("invalid fingerprint or container name")
        inspected=json.loads(self.docker("inspect",self.args.container))[0]
        state=inspected.get("State",{})
        if not state.get("Running") or state.get("Health",{}).get("Status")!="healthy":
            raise RuntimeError("named server is not running and healthy")
        server_log=self.logs()
        if "MRFPS_DTLS_SERVER_SHA256="+self.args.fingerprint not in server_log:
            raise RuntimeError("server fingerprint does not match this container")
        self.report["containerId"]=inspected.get("Id")
        self.report["containerStartedAt"]=state.get("StartedAt")
        ensure_private_dir(self.private)
        self.persist()

    def admin_call(self,arguments):
        process=subprocess.run([self.node,str(ADMIN_CLI),*arguments],stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                               timeout=25,creationflags=CREATE_FLAGS)
        if process.returncode: raise RuntimeError("admin code issuance or exchange failed")
        try: return json.loads(process.stdout)
        except ValueError as error: raise RuntimeError("admin command returned invalid JSON") from error

    def issue(self,role):
        self.check_stop()
        common=["--key-file",str(self.key),"--passphrase-file",str(self.passphrase),"--credentials-file",str(self.credentials),
                "--state-dir",str(self.issuer_state),"--fingerprint",self.args.fingerprint,"--role",role]
        self.admin_call(["ensure",*common])
        operation=str(uuid.uuid4()); self.operations.append(operation)
        self.report["exchangeOperations"].append(operation); self.persist()
        result=self.admin_call(["exchange-export","--role",role,"--code-file",str(self.issuer_state/(role+".code")),
            "--public-key-file",str(self.issuer),"--fingerprint",self.args.fingerprint,"--operation-id",operation,
            "--state-dir",str(self.exchange_state)])
        token=Path(result.get("token_file", "")).resolve(strict=True)
        if token.parent!=self.exchange_state.resolve() or token.name!="exchange-"+operation+".jwt":
            raise RuntimeError("exchange returned an unexpected token path")
        return token,safe_subject(token,role,self.args.fingerprint)

    def start(self,role,name,required):
        self.set_stage("starting_"+name); self.require_memory()
        token,subject=self.issue(role)
        log=self.private/(name+".log")
        control_name=self.control_prefix+"-"+name
        arguments=[str(self.executable)]
        if self.args.editor: arguments += [str(ROOT/"MRFPS.uproject"),"-game"]
        arguments += ["/Game/Maps/MRFPSArena",f"-MRFPSServer={self.args.server}:{self.args.port}","-MRFPSDesktop",
                      "-nullrhi","-nohmd","-nosound","-unattended","-NoSplash","-NoLiveCoding",
                      "-ExecCmds=t.MaxFPS 20,t.IdleWhenNotForeground 0",f"-MRFPSWindowName={control_name}"]
        # Smoke produces periodic spectator snapshots but also fires shots for
        # fighters. Never enable it for the synthetic player role.
        if role!="player": arguments += ["-MRFPSSmoke"]
        if role=="inspector": arguments += ["-MRFPSInspector"]
        if role=="admin": arguments += ["-MRFPSAdmin"]
        # UE FParse requires quotes immediately after '=' for spaced paths.
        commandline=subprocess.list2cmdline(arguments)+f' -MRFPSAuthTokenFile="{token}" -MRFPSAuthPublicKeyFile="{self.issuer}" -abslog="{log}"'
        process=subprocess.Popen(commandline,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                                 creationflags=CREATE_FLAGS,close_fds=True)
        item={"name":name,"role":role,"subject":subject,"process":process,"log":log,"required":required}
        self.children.append(item)
        self.report["instances"].append({"name":name,"controlName":control_name,"role":role,"subject":subject,"pid":process.pid,"status":"starting"})
        self.persist()
        return item

    def wait(self,stage,check,timeout=90):
        self.set_stage(stage); deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.check_stop()
            if check(): return
            time.sleep(.5)
        raise RuntimeError(stage+" timed out")

    def accepted(self,item):
        role,subject=item["role"],item["subject"]
        expected="MRFPS_AUTH_ACCEPT role="+role+" subject="+subject
        text=self.logs()
        joined=re.search(r"MRFPS_JOIN role="+re.escape(role)+r" slot=-?\d+ name="+re.escape(subject)+r"(?:\s|$)",text)
        return expected in text and bool(joined)

    def accept(self,role,name):
        item=self.start(role,name,True)
        self.wait("accept_"+name,lambda:self.accepted(item))
        for entry in self.report["instances"]:
            if entry["name"]==name: entry["status"]="accepted"
        self.events.append("accepted "+name+" role="+role); self.persist()
        return item

    def reject_capacity(self,role,name,reason):
        pattern="MRFPS_ROLE_CAPACITY role="+role+" code="+reason
        before=self.logs().count(pattern)
        item=self.start(role,name,False)
        def confirmed():
            if self.logs().count(pattern)<=before or not item["log"].is_file(): return False
            text=item["log"].read_text(encoding="utf-8",errors="replace")
            if JWT_RE.search(text) or "-----BEGIN PRIVATE KEY-----" in text: raise RuntimeError("client log failed credential-privacy check")
            return bool(re.search(r"MRFPS_NETWORK_FAILURE[^\r\n]*\b"+re.escape(reason)+r"\b",text))
        self.wait("reject_"+name,confirmed)
        if self.accepted(item): raise RuntimeError("capacity candidate was accepted")
        if item["process"].poll() is None: item["process"].terminate()
        item["process"].wait(timeout=15)
        for entry in self.report["instances"]:
            if entry["name"]==name: entry["status"]="rejected:"+reason
        self.events.append("rejected "+name+" code="+reason); self.persist()

    def snapshot_players(self,item):
        if not item["log"].is_file(): return None
        text=item["log"].read_text(encoding="utf-8",errors="replace")
        if JWT_RE.search(text) or "-----BEGIN PRIVATE KEY-----" in text: raise RuntimeError("client log failed credential-privacy check")
        matches=re.findall(r"\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]\[\s*\d+\][^\r\n]*MRFPS_SNAPSHOT players=([0-2]) role=inspector",text)
        if not matches: return None
        timestamp,value=matches[-1]
        when=dt.datetime.strptime(timestamp,"%Y.%m.%d-%H.%M.%S:%f").replace(tzinfo=dt.timezone.utc)
        age=(dt.datetime.now(dt.timezone.utc)-when).total_seconds()
        return int(value) if -3<=age<=5 else None

    def run_test(self):
        self.preflight()
        first=self.accept("inspector","inspector1"); observers=[first]
        self.wait("initial_player_count",lambda:self.snapshot_players(first) is not None)
        baseline=self.snapshot_players(first)
        if baseline not in (0,1) or (baseline==1 and not self.args.allow_existing_player):
            raise RuntimeError("server already has players; explicitly allow one existing player or use an empty server")
        self.report["existingPlayersAtStart"]=baseline; self.persist()
        for i in range(2-baseline): self.accept("player","player"+str(i+1))
        self.wait("two_players_replicated",lambda:self.snapshot_players(first)==2)
        self.report["checks"]["twoPlayersAccepted"]=True; self.persist()
        self.reject_capacity("player","extraPlayer","ARENA_FULL")
        self.report["checks"]["thirdPlayerRejected"]=True; self.persist()
        admin=self.accept("admin","admin1")
        self.reject_capacity("admin","extraAdmin","ADMIN_FULL")
        self.report["checks"]["secondAdminRejected"]=True; self.persist()
        for i in range(1,self.args.inspectors): observers.append(self.accept("inspector","inspector"+str(i+1)))
        self.wait("roles_simultaneously_present",lambda:all(self.snapshot_players(item)==2 for item in [admin,*observers]),timeout=20)
        self.report["checks"]["multipleInspectorsAccepted"]=True; self.persist()
        self.report["checksCompleted"]=True; self.persist()
        self.set_stage("holding")
        end=time.monotonic()+self.args.hold_seconds
        while time.monotonic()<end:
            self.check_stop(); time.sleep(.5)
        self.report["status"]="passed"; self.report["stage"]="verified"

    def cleanup(self):
        for item in reversed(self.children):
            process=item["process"]
            if process.poll() is None:
                try: process.terminate()
                except OSError: self.report["limitations"].append("A test process could not be terminated normally")
        for item in self.children:
            try: item["process"].wait(timeout=15)
            except subprocess.TimeoutExpired:
                try: item["process"].kill(); item["process"].wait(timeout=5)
                except (OSError,subprocess.TimeoutExpired): self.report["limitations"].append("A test process required guard cleanup")
        cleaned=True
        for operation in self.operations:
            try:self.admin_call(["exchange-clean","--operation-id",operation,"--state-dir",str(self.exchange_state)])
            except Exception:
                cleaned=False; self.report["limitations"].append("A protected temporary credential may need cleanup")
        self.report["temporaryCredentialsCleaned"]=cleaned
        for item in self.children:
            try:item["log"].unlink(missing_ok=True)
            except OSError:self.report["limitations"].append("A protected temporary engine log may need cleanup")
        self.report["clientsCleaned"]=all(item["process"].poll() is not None for item in self.children)
        self.persist()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest="command",required=True)
    p=sub.add_parser("stop"); p.add_argument("--run-dir",required=True)
    p=sub.add_parser("run"); p.add_argument("--run-dir",required=True); p.add_argument("--executable",required=True)
    p.add_argument("--server",required=True); p.add_argument("--port",type=int,required=True); p.add_argument("--container",required=True)
    p.add_argument("--fingerprint",required=True); p.add_argument("--admin-dir",required=True); p.add_argument("--inspectors",type=int,default=2)
    p.add_argument("--hold-seconds",type=int,default=0); p.add_argument("--editor",action="store_true"); p.add_argument("--allow-existing-player",action="store_true")
    args=parser.parse_args(); run=Path(args.run_dir).resolve()
    if args.command=="stop":
        run.mkdir(parents=True,exist_ok=True); (run/"STOP").touch(exist_ok=True)
        print(json.dumps({"status":"stop_requested","run_dir":str(run)})); return 0
    if not 1<=args.port<=65535 or not 2<=args.inspectors<=8 or not 0<=args.hold_seconds<=1800:
        raise ValueError("invalid limits")
    run.mkdir(parents=True,exist_ok=True)
    with state_lock(run/"probe.json"):
        report=run/"report.json"
        if report.exists():
            previous=json.loads(report.read_text(encoding="utf-8"))
            print(json.dumps({"status":"already_started","prior_status":previous.get("status"),"report":str(report)})); return 0
        probe=Probe(args)
        try:
            probe.persist(); probe.run_test()
        except StopRequested:
            probe.report["status"]="passed" if probe.report.get("checksCompleted") else "stopped"
            probe.report["stage"]="stopped_after_checks" if probe.report.get("checksCompleted") else "stop_requested"
        except ResourceLimit as exc:
            probe.report["status"]="resource_limited"; probe.report["limitations"].append(str(exc))
        except (RuntimeError,ValueError,OSError,subprocess.SubprocessError) as exc:
            probe.report["status"]="failed"; probe.report["limitations"].append(str(exc))
        finally:
            probe.cleanup()
        print(json.dumps({"status":probe.report["status"],"report":str(report)}))
        return 0 if probe.report["status"] in ("passed","stopped") else 2


if __name__=="__main__":
    try: raise SystemExit(main())
    except (ValueError,RuntimeError,OSError,subprocess.SubprocessError) as error:
        print(json.dumps({"status":"error","message":str(error)}),file=sys.stderr)
        raise SystemExit(1)
