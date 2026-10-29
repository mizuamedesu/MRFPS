#!/usr/bin/env python3
"""Retry-safe lifecycle for one MRFPS-owned Ubuntu keepalive, never all of WSL."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent/'window_capture'))
import capture  # Existing restricted directory, lock, and atomic journal helpers.

CREATE_NO_WINDOW=getattr(subprocess,'CREATE_NO_WINDOW',0)
SCHEMA=1


def valid_id(value):
    return capture.valid_id(value)


def root_directory():
    base=os.environ.get('LOCALAPPDATA','')
    if os.name!='nt' or not base or not Path(base).is_absolute():
        raise RuntimeError('Windows LOCALAPPDATA is required')
    return Path(base)/'MRFPS'/'WslKeepalive'


def prepare_directory(root):
    if not root.is_absolute():
        raise ValueError('absolute state directory required')
    capture.reject_reparse(root.parent)
    capture.reject_reparse(root)
    root.mkdir(parents=True,exist_ok=True,mode=0o700)
    capture.reject_reparse(root)
    capture.restrict_directories([root])


def read_state(root):
    path=root/'state.json'
    if not path.exists():
        return None
    state=capture.read_json(path)
    if (type(state.get('schema')) is not int or state.get('schema')!=SCHEMA or state.get('distro')!='Ubuntu' or
        state.get('stage') not in ('starting','running','stopping','stopped')):
        raise RuntimeError('keeper journal is invalid')
    valid_id(state.get('id'))
    return state


def save_state(root,state):
    capture.atomic_json(root/'state.json',state)


def create_private(path,data=b''):
    capture.reject_reparse(path)
    flags=os.O_WRONLY|os.O_CREAT|os.O_EXCL|getattr(os,'O_BINARY',0)
    with os.fdopen(os.open(path,flags,0o600),'wb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def verified_helper(root,state):
    keeper_id=valid_id(state.get('id'))
    path=root/(keeper_id+'.py')
    capture.reject_reparse(path)
    if not path.is_file() or path.stat().st_size>32768:
        raise RuntimeError('owned helper is unavailable')
    data=path.read_bytes()
    if state.get('helper_sha256')!=sha256(data):
        raise RuntimeError('owned helper changed')
    return path


def windows_record_valid(value):
    return (isinstance(value,dict) and set(value)=={'pid','created'} and
            type(value['pid']) is int and 0<value['pid']<2**32 and
            type(value['created']) is int and 0<value['created']<2**64)


def linux_record_valid(value,keeper_id):
    if not isinstance(value,dict) or set(value)!={'version','id','pid','start_ticks','boot_id'}:
        return False
    if type(value.get('version')) is not int or value.get('version')!=1 or value.get('id')!=keeper_id:
        return False
    if type(value.get('pid')) is not int or not 1<value['pid']<2**31 or type(value.get('start_ticks')) is not int or not 0<=value['start_ticks']<2**64:
        return False
    try:
        return str(uuid.UUID(value['boot_id']))==value['boot_id']
    except (ValueError,TypeError,KeyError,AttributeError):
        return False


def wait_owned(process,timeout):
    # Waiting only for this direct process is bounded even if a descendant
    # inherits stdout/stderr handles. Never use communicate() on such pipes.
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try: process.kill()
        except OSError: pass
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: raise RuntimeError('owned process did not exit') from None
        raise


class WindowsBackend:
    def __init__(self,root):
        if os.name!='nt': raise RuntimeError('Windows is required')
        self.root=root
        win=Path(os.environ['WINDIR'])/'System32'
        self.wsl=win/'wsl.exe'
        self.powershell=win/'WindowsPowerShell'/'v1.0'/'powershell.exe'
        if not self.wsl.is_file() or not self.powershell.is_file():
            raise RuntimeError('WSL or Windows PowerShell is unavailable')

    def run(self,args,timeout=5):
        command=[str(x) for x in args]
        # These files inherit the private state directory ACL. A long-lived
        # descendant may retain a file handle, but cannot hold a pipe open or
        # prevent a bounded wait for the direct child.
        with tempfile.TemporaryFile(dir=self.root) as out, tempfile.TemporaryFile(dir=self.root) as err:
            process=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=err,
                                     creationflags=CREATE_NO_WINDOW,close_fds=True)
            code=wait_owned(process,timeout)
            out.seek(0); stdout=out.read(32769); err.seek(0); stderr=err.read(32769)
            if len(stdout)>32768 or len(stderr)>32768: raise RuntimeError('owned process output exceeds its bound')
            return subprocess.CompletedProcess(command,code,stdout,stderr)

    def linux_path(self,path):
        result=self.run([self.wsl,'-d','Ubuntu','-u','root','--exec','wslpath','-a',str(path).replace('\\','/')])
        if result.returncode:
            raise RuntimeError('cannot locate WSL helper path')
        value=result.stdout.decode('utf-8','strict').strip()
        if not value.startswith('/') or any(x in value for x in ('"','\\','\r','\n','\x00')) or len(value)>4096:
            raise RuntimeError('invalid WSL helper path')
        return value

    def distro_running(self):
        result=self.run([self.wsl,'--list','--running','--quiet'])
        if result.returncode:
            raise RuntimeError('cannot inspect running WSL distributions')
        try:
            raw=result.stdout
            value=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-16le' if b'\0' in raw else 'utf-8')
        except UnicodeError:
            raise RuntimeError('invalid WSL distribution status') from None
        return 'Ubuntu' in [line.strip().lstrip('\ufeff') for line in value.splitlines()]

    def launch(self,root,keeper_id,linux_path):
        command=[self.powershell,'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
                 '-File',HERE/'launch.ps1','-Id',keeper_id,'-LinuxScript',linux_path,'-Directory',root]
        out_path=root/(keeper_id+'.launcher.stdout'); err_path=root/(keeper_id+'.launcher.stderr')
        capture.reject_reparse(out_path); capture.reject_reparse(err_path)
        # Persistent per-launch logs are under the restricted root. The WSL
        # child may inherit their handles, but the launcher wait never waits
        # for those handles to close.
        with out_path.open('r+b',buffering=0) as out, err_path.open('r+b',buffering=0) as err:
            process=subprocess.Popen([str(x) for x in command],stdin=subprocess.DEVNULL,stdout=out,stderr=err,
                                     creationflags=CREATE_NO_WINDOW,close_fds=True)
            if wait_owned(process,12)!=0: raise RuntimeError('keeper launch outcome is unknown')
        value=capture.read_json(root/(keeper_id+'.windows.json'),limit=4096)
        if not windows_record_valid(value): raise RuntimeError('keeper launch identity is invalid')
        return value

    def command_line(self,pid):
        # pid has already been validated as a positive integer; only this PID
        # is queried. The handle remains open throughout the WMI lookup.
        script=("$ErrorActionPreference='Stop'; [Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false); "
                f"$p=Get-CimInstance -ClassName Win32_Process -Filter 'ProcessId={pid}'; "
                "if($null -eq $p -or $null -eq $p.CommandLine){exit 2}; [Console]::Out.Write($p.CommandLine)")
        result=self.run([self.powershell,'-NoProfile','-NonInteractive','-Command',script],timeout=5)
        if result.returncode:
            raise RuntimeError('cannot inspect owned process arguments')
        return result.stdout.decode('utf-8','strict')

    @staticmethod
    def arguments(command_line):
        shell=ctypes.WinDLL('shell32',use_last_error=True)
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        shell.CommandLineToArgvW.argtypes=[wintypes.LPCWSTR,ctypes.POINTER(ctypes.c_int)]
        shell.CommandLineToArgvW.restype=ctypes.POINTER(wintypes.LPWSTR)
        kernel.LocalFree.argtypes=[ctypes.c_void_p]; kernel.LocalFree.restype=ctypes.c_void_p
        count=ctypes.c_int(); ptr=shell.CommandLineToArgvW(command_line,ctypes.byref(count))
        if not ptr: raise RuntimeError('cannot parse process arguments')
        try: return [ptr[i] for i in range(count.value)]
        finally: kernel.LocalFree(ctypes.cast(ptr,ctypes.c_void_p))

    def windows_status(self,state):
        identity=state.get('windows')
        if not isinstance(identity,dict) or type(identity.get('pid')) is not int or type(identity.get('created')) is not int:
            return {'status':'unknown'}
        pid,expected=identity['pid'],identity['created']
        if not 0<pid<2**32 or not 0<expected<2**64: return {'status':'unknown'}
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]; kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]; kernel.CloseHandle.restype=wintypes.BOOL
        kernel.GetExitCodeProcess.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)]; kernel.GetExitCodeProcess.restype=wintypes.BOOL
        kernel.GetProcessTimes.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME)]; kernel.GetProcessTimes.restype=wintypes.BOOL
        kernel.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]; kernel.QueryFullProcessImageNameW.restype=wintypes.BOOL
        kernel.IsProcessInJob.argtypes=[wintypes.HANDLE,wintypes.HANDLE,ctypes.POINTER(wintypes.BOOL)]; kernel.IsProcessInJob.restype=wintypes.BOOL
        ctypes.set_last_error(0); handle=kernel.OpenProcess(0x1000,False,pid)
        if not handle:
            return {'status':'gone' if ctypes.get_last_error()==87 else 'unknown'}
        try:
            code=wintypes.DWORD(); times=[wintypes.FILETIME() for _ in range(4)]
            if not kernel.GetExitCodeProcess(handle,ctypes.byref(code)): return {'status':'unknown'}
            if code.value!=259: return {'status':'gone'}
            if not kernel.GetProcessTimes(handle,*(ctypes.byref(x) for x in times)): return {'status':'unknown'}
            created=(times[0].dwHighDateTime<<32)|times[0].dwLowDateTime
            if created!=expected: return {'status':'gone'}
            size=wintypes.DWORD(32768); image=ctypes.create_unicode_buffer(size.value)
            if not kernel.QueryFullProcessImageNameW(handle,0,image,ctypes.byref(size)): return {'status':'unknown'}
            if os.path.normcase(os.path.abspath(image.value))!=os.path.normcase(os.path.abspath(self.wsl)):
                return {'status':'unknown'}
            args=self.arguments(self.command_line(pid))
            expected_args=['-d','Ubuntu','-u','root','--exec','/usr/bin/python3',state['linux_path'],'serve',state['id']]
            if not args or os.path.normcase(os.path.abspath(args[0]))!=os.path.normcase(os.path.abspath(self.wsl)) or args[1:]!=expected_args:
                return {'status':'unknown'}
            if not kernel.GetExitCodeProcess(handle,ctypes.byref(code)): return {'status':'unknown'}
            if code.value!=259: return {'status':'gone'}
            in_job=wintypes.BOOL()
            if not kernel.IsProcessInJob(handle,None,ctypes.byref(in_job)): return {'status':'unknown'}
            return {'status':'running','in_job':bool(in_job.value)}
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
            return {'status':'unknown'}
        finally:
            kernel.CloseHandle(handle)

    def inspect_linux(self,root,state,stop=False):
        helper=verified_helper(root,state)
        if not self.distro_running(): return {'status':'gone','reason':'distribution_not_running'}
        if self.linux_path(helper)!=state.get('linux_path'): return {'status':'unknown'}
        identity=state.get('linux')
        if not linux_record_valid(identity,state['id']): return {'status':'unknown'}
        command='stop' if stop else 'inspect'
        result=self.run([self.wsl,'-d','Ubuntu','-u','root','--exec','/usr/bin/python3',state['linux_path'],
                         command,state['id'],identity['pid'],identity['start_ticks'],identity['boot_id']])
        try: value=json.loads(result.stdout.decode('utf-8','strict'))
        except (UnicodeError,ValueError): return {'status':'unknown'}
        if result.returncode not in (0,2) or not isinstance(value,dict) or value.get('status') not in ('running','gone','unknown','stop_sent'):
            return {'status':'unknown'}
        return value


class Manager:
    def __init__(self,root,backend,*,clock=time.monotonic,sleep=time.sleep):
        self.root,self.backend,self.clock,self.sleep=root,backend,clock,sleep

    def observe(self,state):
        try:
            linux=self.backend.inspect_linux(self.root,state)
            windows=self.backend.windows_status(state)
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
            return {'status':'unknown','reason':'inspection_failed'}
        if linux.get('status')=='running' and windows.get('status') in ('running','gone'):
            return {'status':'running','windows':windows['status'],'windows_in_job':windows.get('in_job')}
        if linux.get('status')=='gone' and windows.get('status')=='gone':
            return {'status':'stopped'}
        return {'status':'unknown','reason':'process_state_uncertain'}

    def recover_start(self,state):
        # A launch interrupted after its Windows identity was recorded can be
        # recovered only from this ID's own complete Linux startup receipt.
        try:
            if not windows_record_valid(state.get('windows')):
                receipt=self.root/(valid_id(state['id'])+'.windows.json')
                if not receipt.exists(): return {'status':'unknown','reason':'launch_interrupted'}
                windows=capture.read_json(receipt,limit=4096)
                if not windows_record_valid(windows): return {'status':'unknown','reason':'launch_interrupted'}
                state['windows']=windows
            path=self.root/(valid_id(state['id'])+'.stdout'); capture.reject_reparse(path)
            if not path.is_file() or path.stat().st_size>4096: return {'status':'unknown','reason':'launch_interrupted'}
            raw=path.read_text(encoding='utf-8')
            if not raw.endswith('\n'): return {'status':'unknown','reason':'launch_interrupted'}
            record=json.loads(raw.strip())
            if not linux_record_valid(record,state['id']): return {'status':'unknown','reason':'launch_interrupted'}
            state['linux']=record
            observed=self.observe(state)
            if observed['status'] in ('running','stopped'):
                state['stage']=observed['status']; save_state(self.root,state)
            return observed
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
            return {'status':'unknown','reason':'launch_interrupted'}

    def status(self):
        state=read_state(self.root)
        if state is None or state['stage']=='stopped': return {'status':'stopped'}
        if state['stage']=='starting': return self.recover_start(state)
        observed=self.observe(state)
        if observed['status']=='stopped':
            state['stage']='stopped'; save_state(self.root,state)
            return observed
        if state['stage']=='stopping': return {'status':'unknown','reason':'stop_outcome_pending'}
        return observed

    def ensure(self):
        state=read_state(self.root)
        if state is not None and state['stage']!='stopped':
            observed=self.recover_start(state) if state['stage']=='starting' else self.observe(state)
            if observed['status']=='running' and state['stage']=='running':
                return {'status':'already_running',**{k:v for k,v in observed.items() if k!='status'}}
            if observed['status']!='stopped': return {'status':'unknown','reason':'previous_process_uncertain'}
            state['stage']='stopped'; save_state(self.root,state)
        keeper_id=str(uuid.uuid4())
        helper=(HERE/'linux_keeper.py').read_bytes()
        if not helper or len(helper)>32768: raise RuntimeError('local helper is invalid')
        helper_path=self.root/(keeper_id+'.py')
        create_private(helper_path,helper)
        for suffix in ('.stdin','.stdout','.stderr','.launcher.stdout','.launcher.stderr'): create_private(self.root/(keeper_id+suffix))
        linux_path=self.backend.linux_path(helper_path)
        state={'schema':SCHEMA,'stage':'starting','distro':'Ubuntu','id':keeper_id,
               'linux_path':linux_path,'helper_sha256':sha256(helper)}
        save_state(self.root,state)  # Persist intent before Start-Process.
        try:
            state['windows']=self.backend.launch(self.root,keeper_id,linux_path)
            save_state(self.root,state)
            deadline=self.clock()+20
            while self.clock()<deadline:
                path=self.root/(keeper_id+'.stdout'); capture.reject_reparse(path)
                if path.stat().st_size>4096: break
                raw=path.read_text(encoding='utf-8')
                if raw and raw.endswith('\n'):
                    try: record=json.loads(raw.strip())
                    except ValueError: break
                    if not linux_record_valid(record,keeper_id): break
                    state['linux']=record
                    observed=self.observe(state)
                    if observed['status']=='running':
                        state['stage']='running'; save_state(self.root,state)
                        return {'status':'started','windows':observed['windows'],'windows_in_job':observed['windows_in_job']}
                    break
                self.sleep(.2)
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
            pass
        return {'status':'unknown','reason':'launch_unconfirmed'}

    def stop(self):
        state=read_state(self.root)
        if state is None or state['stage']=='stopped': return {'status':'already_stopped'}
        observed=self.recover_start(state) if state['stage']=='starting' else self.observe(state)
        if observed['status']=='stopped':
            state['stage']='stopped'; save_state(self.root,state); return {'status':'already_stopped'}
        if state['stage']=='stopping': return {'status':'unknown','reason':'stop_outcome_pending'}
        if observed['status']!='running': return {'status':'unknown','reason':'process_state_uncertain'}
        state['stage']='stopping'; save_state(self.root,state)  # Record before signaling.
        try:
            result=self.backend.inspect_linux(self.root,state,stop=True)
            if result.get('status') not in ('stop_sent','gone'): return {'status':'unknown','reason':'stop_unconfirmed'}
            deadline=self.clock()+10
            while self.clock()<deadline:
                observed=self.observe(state)
                if observed['status']=='stopped':
                    state['stage']='stopped'; save_state(self.root,state); return {'status':'stopped'}
                self.sleep(.25)
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
            pass
        return {'status':'unknown','reason':'stop_unconfirmed'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('ensure','status','stop'))
    args=parser.parse_args()
    root=root_directory(); prepare_directory(root)
    with capture.directory_lock(root):
        manager=Manager(root,WindowsBackend(root))
        result=getattr(manager,args.command)()
    print(json.dumps(result,ensure_ascii=True,separators=(',',':')))
    return 0 if result['status'] in ('started','already_running','running','stopped','already_stopped') else 1


if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):
        print(json.dumps({'status':'unknown','reason':'operation_failed'}),file=sys.stderr)
        raise SystemExit(1)
