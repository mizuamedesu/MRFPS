#!/usr/bin/env python3
"""Local-only HTTPS fixture for Unreal's HTTP certificate verification.

The optional UE run must be launched inside the project's resource guard.
Certificates are throwaway, never installed in the OS store, and the UE-only
CA override is process-scoped. No device code or production endpoint is used.
"""
from __future__ import annotations

import argparse
import datetime
import json
import math
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

BODY=b'MRFPS_TLS_FIXTURE_OK'
TEST_NAME='MRFPSAuthFixture.PeerVerification'


def certs(directory: Path, name: str) -> tuple[Path,Path,Path]:
    now=datetime.datetime.now(datetime.timezone.utc)
    ca_key=ec.generate_private_key(ec.SECP256R1())
    ca_name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'MRFPS local test CA '+name)])
    ca=(x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name).public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=2))
        .not_valid_after(now+datetime.timedelta(hours=2))
        .add_extension(x509.BasicConstraints(ca=True,path_length=0),critical=True)
        .sign(ca_key,hashes.SHA256()))
    leaf_key=ec.generate_private_key(ec.SECP256R1())
    leaf_name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')])
    leaf=(x509.CertificateBuilder().subject_name(leaf_name).issuer_name(ca_name).public_key(leaf_key.public_key())
          .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=2))
          .not_valid_after(now+datetime.timedelta(hours=1))
          .add_extension(x509.BasicConstraints(ca=False,path_length=None),critical=True)
          .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]),critical=False)
          .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),critical=False)
          .sign(ca_key,hashes.SHA256()))
    ca_path=directory/(name+'-ca.pem'); cert_path=directory/(name+'-server.pem'); key_path=directory/(name+'-server.key')
    ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    cert_path.write_bytes(leaf.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(leaf_key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,
                                               serialization.NoEncryption()))
    return ca_path,cert_path,key_path


class FixtureServer(ThreadingHTTPServer):
    daemon_threads=True
    allow_reuse_address=False

    def __init__(self,cert: Path,key: Path):
        super().__init__(('127.0.0.1',0),FixtureHandler)
        self.context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(str(cert),str(key))
        self.lock=threading.Lock(); self.accepted=0; self.requests=0
        self.thread=threading.Thread(target=self.serve_forever,kwargs={'poll_interval':.05},daemon=True)

    def get_request(self):
        sock,address=super().get_request()
        with self.lock: self.accepted+=1
        try:
            sock.settimeout(5)
            return self.context.wrap_socket(sock,server_side=True),address
        except (OSError,ssl.SSLError):
            sock.close(); raise

    def start(self): self.thread.start()
    def stop(self): self.shutdown(); self.server_close(); self.thread.join(timeout=5)
    @property
    def port(self): return self.server_address[1]


class FixtureHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        with self.server.lock: self.server.requests+=1
        if self.path!='/fixture': self.send_error(404); return
        self.send_response(200); self.send_header('Content-Type','text/plain'); self.send_header('Content-Length',str(len(BODY)))
        self.end_headers(); self.wfile.write(BODY)

    def log_message(self,*_args): pass


def get(url: str, ca: Path) -> bytes:
    context=ssl.create_default_context(cafile=str(ca))
    # Ignore operating-system proxies: all fixture traffic must stay loopback.
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),urllib.request.HTTPSHandler(context=context))
    with opener.open(url,timeout=5) as response: return response.read(1024)


def self_test(trusted: FixtureServer,untrusted: FixtureServer,ca: Path) -> None:
    if get(f'https://localhost:{trusted.port}/fixture',ca)!=BODY: raise RuntimeError('FIXTURE_POSITIVE_FAILED')
    for url in (f'https://127.0.0.1:{trusted.port}/fixture',f'https://localhost:{untrusted.port}/fixture'):
        try: get(url,ca)
        except urllib.error.URLError as error:
            if not isinstance(error.reason,ssl.SSLCertVerificationError): raise
        else: raise RuntimeError('INVALID_CERTIFICATE_ACCEPTED')
    if trusted.accepted<2 or untrusted.accepted<1: raise RuntimeError('FIXTURE_NOT_REACHED')


def require_guard(path: Path) -> None:
    if os.name!='nt': raise ValueError('WINDOWS_RESOURCE_GUARD_REQUIRED')
    import ctypes
    import time
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetCurrentProcess.restype=ctypes.c_void_p
    kernel.IsProcessInJob.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)]
    kernel.IsProcessInJob.restype=ctypes.c_int
    assigned=ctypes.c_int()
    if not kernel.IsProcessInJob(kernel.GetCurrentProcess(),None,ctypes.byref(assigned)) or not assigned.value:
        raise ValueError('WINDOWS_RESOURCE_GUARD_REQUIRED')
    path=path.resolve(strict=True)
    if not path.is_file() or path.stat().st_size>1024*1024 or time.time()-path.stat().st_mtime>15:
        raise ValueError('RESOURCE_GUARD_REPORT_INVALID')
    report=json.loads(path.read_text(encoding='utf-8-sig')); sample=report.get('lastSample') or {}
    free,commit=sample.get('availablePhysicalGB'),sample.get('commitPercent')
    if (report.get('status')!='running' or report.get('jobAssigned') is not True or sample.get('pressureReason') or
        type(free) not in (int,float) or type(commit) not in (int,float) or
        not math.isfinite(free) or not math.isfinite(commit) or free<10 or commit<0 or commit>75):
        raise ValueError('RESOURCE_GUARD_NOT_RUNNING')


def run_editor(editor: Path,project: Path,report_dir: Path,guard: Path,trusted: FixtureServer,untrusted: FixtureServer,ca: Path) -> None:
    require_guard(guard)
    editor=editor.resolve(strict=True); project=project.resolve(strict=True)
    if not editor.is_file() or not project.is_file() or project.suffix.lower()!='.uproject': raise ValueError('INVALID_EDITOR_OR_PROJECT')
    # UE's command-line INI override uses comma as a separator.
    if any(c in str(ca) for c in ',[]'): raise ValueError('TEST_CA_PATH_UNSUPPORTED')
    report_dir.mkdir(parents=True,exist_ok=False)
    args=[str(editor),str(project),'-unattended','-NullRHI','-nohmd','-nosound','-NoSplash','-NoLiveCoding','-nop4',
          '-stdout','-FullStdOutLogOutput','-ExecCmds=Automation RunTests '+TEST_NAME,
          '-TestExit=Automation Test Queue Empty','-ReportExportPath='+str(report_dir),
          '-abslog='+str(report_dir/'engine.log'),'-MRFPSTlsTrustedPort='+str(trusted.port),
          '-MRFPSTlsUntrustedPort='+str(untrusted.port),'-ini:Engine:[SSL]:OverrideCertificateBundlePath='+str(ca)]
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0) if os.name=='nt' else 0
    with (report_dir/'stdout.log').open('xb') as stdout,(report_dir/'stderr.log').open('xb') as stderr:
        process=subprocess.Popen(args,cwd=project.parent,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,creationflags=flags)
        try: code=process.wait(timeout=300)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=10); raise RuntimeError('UE_FIXTURE_TIMED_OUT') from None
    if code: raise RuntimeError('UE_FIXTURE_PROCESS_FAILED')
    index=report_dir/'index.json'
    if not index.is_file(): raise RuntimeError('UE_FIXTURE_REPORT_MISSING')
    result=json.loads(index.read_text(encoding='utf-8-sig'))
    tests=[item for item in result.get('tests',[]) if item.get('fullTestPath')==TEST_NAME]
    if len(tests)!=1 or tests[0].get('state')!='Success' or tests[0].get('errors')!=0:
        raise RuntimeError('UE_FIXTURE_TEST_FAILED_OR_MISSING')
    if trusted.accepted<2 or untrusted.accepted<1 or trusted.requests!=1 or untrusted.requests!=0:
        raise RuntimeError('UE_FIXTURE_LIVENESS_OR_VERIFICATION_FAILED')


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    mode=parser.add_subparsers(dest='mode',required=True)
    mode.add_parser('self-test')
    run=mode.add_parser('run'); run.add_argument('--editor',type=Path,required=True)
    run.add_argument('--project',type=Path,required=True); run.add_argument('--report-dir',type=Path,required=True)
    run.add_argument('--guard-report',type=Path,required=True)
    args=parser.parse_args()
    if args.mode=='run': require_guard(args.guard_report)
    with tempfile.TemporaryDirectory(prefix='mrfps-tls-fixture-') as temp:
        directory=Path(temp).resolve(); ca,cert,key=certs(directory,'trusted'); _other,other_cert,other_key=certs(directory,'other')
        trusted=FixtureServer(cert,key); untrusted=FixtureServer(other_cert,other_key)
        trusted.start(); untrusted.start()
        try:
            if args.mode=='self-test': self_test(trusted,untrusted,ca)
            else: run_editor(args.editor,args.project,args.report_dir,args.guard_report,trusted,untrusted,ca)
        finally:
            trusted.stop(); untrusted.stop()
    print(json.dumps({'status':'passed','scope':'Python fixture' if args.mode=='self-test' else 'UE editor HTTP fixture'}))


if __name__=='__main__': main()
