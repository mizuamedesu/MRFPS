import errno
import ctypes
import io
from contextlib import nullcontext, redirect_stdout
from ctypes import wintypes
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import uuid

import keepalive
import linux_keeper


class FakeClock:
    def __init__(self): self.now=0
    def time(self): return self.now
    def sleep(self,seconds): self.now+=seconds


class FakeBackend:
    def __init__(self):
        self.launch_count=0; self.stop_count=0
        self.linux='gone'; self.windows='gone'; self.stop_changes=True
        self.launch_error=False; self.inspect_error=False; self.partial_start=False
    def linux_path(self,path): return '/mnt/c/owned/'+path.name
    def launch(self,root,keeper_id,linux_path):
        self.launch_count+=1
        if self.launch_error:
            self.linux=self.windows='running'  # May have started before the reply was lost.
            raise RuntimeError('lost launch reply')
        record={'version':1,'id':keeper_id,'pid':1234,'start_ticks':5678,
                'boot_id':'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'}
        (root/(keeper_id+'.stdout')).write_text(json.dumps(record)+('' if self.partial_start else '\n'),encoding='utf-8')
        identity={'pid':4321,'created':9876}
        keepalive.capture.atomic_json(root/(keeper_id+'.windows.json'),identity)
        self.linux=self.windows='running'
        return identity
    def windows_status(self,state): return {'status':self.windows,'in_job':False}
    def inspect_linux(self,root,state,stop=False):
        keepalive.verified_helper(root,state)
        if self.inspect_error: raise RuntimeError('helper unavailable')
        if stop:
            self.stop_count+=1
            if self.stop_changes: self.linux=self.windows='gone'
            return {'status':'stop_sent'}
        return {'status':self.linux}


class OwnedTempTest(unittest.TestCase):
    def setUp(self):
        self.temp_base=Path(tempfile.gettempdir()).resolve(strict=True)
        self.root=Path(tempfile.mkdtemp(prefix='mrfps-wsl-keepalive-test-',dir=self.temp_base)).resolve(strict=True)
        if self.root.parent!=self.temp_base or not self.root.name.startswith('mrfps-wsl-keepalive-test-'):
            raise RuntimeError('unsafe temporary directory')
        self.keep_dir=False
    def tearDown(self):
        if self.keep_dir: return
        resolved=self.root.resolve(strict=True)
        if resolved!=self.root or resolved.parent!=self.temp_base or not resolved.name.startswith('mrfps-wsl-keepalive-test-'):
            raise RuntimeError('unsafe temporary cleanup')
        shutil.rmtree(resolved)


class LifecycleTests(OwnedTempTest):
    def setUp(self):
        super().setUp()
        self.backend=FakeBackend(); self.clock=FakeClock()
        self.manager=keepalive.Manager(self.root,self.backend,clock=self.clock.time,sleep=self.clock.sleep)
    def test_ensure_is_idempotent(self):
        self.assertEqual(self.manager.ensure()['status'],'started')
        self.assertEqual(self.manager.ensure()['status'],'already_running')
        self.assertEqual(self.manager.status()['status'],'running')
        self.assertEqual(self.backend.launch_count,1)
    def test_stop_is_idempotent_and_new_run_can_start(self):
        self.manager.ensure(); self.assertEqual(self.manager.stop()['status'],'stopped')
        self.assertEqual(self.manager.stop()['status'],'already_stopped')
        self.assertEqual(self.backend.stop_count,1)
        self.assertEqual(self.manager.ensure()['status'],'started')
        self.assertEqual(self.backend.launch_count,2)
    def test_lost_launch_response_does_not_repeat(self):
        self.backend.launch_error=True
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.backend.launch_count,1); self.assertEqual(self.backend.stop_count,0)
    def test_recover_interrupted_start_with_recorded_owned_identity(self):
        self.manager.ensure(); state=keepalive.read_state(self.root)
        state['stage']='starting'; state.pop('linux'); keepalive.save_state(self.root,state)
        self.assertEqual(self.manager.status()['status'],'running')
        self.assertEqual(self.manager.ensure()['status'],'already_running')
        self.assertEqual(self.backend.launch_count,1)
    def test_durable_windows_receipt_recovers_interrupted_start(self):
        self.manager.ensure(); state=keepalive.read_state(self.root)
        state['stage']='starting'; state.pop('windows'); state.pop('linux'); keepalive.save_state(self.root,state)
        self.assertEqual(self.manager.status()['status'],'running')
        self.assertEqual(self.manager.ensure()['status'],'already_running')
        self.assertEqual(self.backend.launch_count,1); self.assertEqual(self.backend.stop_count,0)
    def test_missing_windows_receipt_stays_unknown_without_effect(self):
        self.manager.ensure(); state=keepalive.read_state(self.root)
        receipt=self.root/(state['id']+'.windows.json'); receipt.unlink()
        state['stage']='starting'; state.pop('windows'); state.pop('linux'); keepalive.save_state(self.root,state)
        self.assertEqual(self.manager.status()['status'],'unknown')
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.backend.launch_count,1); self.assertEqual(self.backend.stop_count,0)
    def test_incomplete_windows_receipt_remains_unknown_without_effect(self):
        self.manager.ensure(); state=keepalive.read_state(self.root)
        receipt=self.root/(state['id']+'.windows.json'); receipt.write_text('{"pid":',encoding='utf-8')
        state['stage']='starting'; state.pop('windows'); state.pop('linux'); keepalive.save_state(self.root,state)
        self.assertEqual(self.manager.status()['status'],'unknown')
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.backend.launch_count,1); self.assertEqual(self.backend.stop_count,0)
    def test_incomplete_start_receipt_waits_and_can_recover(self):
        self.backend.partial_start=True
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.status()['status'],'unknown')
        state=keepalive.read_state(self.root); path=self.root/(state['id']+'.stdout')
        path.write_text(path.read_text(encoding='utf-8')+'\n',encoding='utf-8')
        self.assertEqual(self.manager.status()['status'],'running')
        self.assertEqual(self.manager.ensure()['status'],'already_running')
        self.assertEqual(self.backend.launch_count,1)
    def test_lost_windows_parent_does_not_duplicate_linux_keeper(self):
        self.manager.ensure(); self.backend.windows='gone'
        result=self.manager.ensure()
        self.assertEqual(result['status'],'already_running'); self.assertEqual(result['windows'],'gone')
        self.assertEqual(self.backend.launch_count,1)
    def test_linux_keeper_can_stop_after_its_windows_parent_exits(self):
        self.manager.ensure(); self.backend.windows='gone'
        self.assertEqual(self.manager.stop()['status'],'stopped')
        self.assertEqual(self.backend.stop_count,1)
    def test_unknown_inspection_does_not_signal_or_relaunch(self):
        self.manager.ensure(); self.backend.inspect_error=True
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.backend.launch_count,1); self.assertEqual(self.backend.stop_count,0)
    def test_unconfirmed_stop_is_not_repeated(self):
        self.manager.ensure(); self.backend.stop_changes=False
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.manager.status()['status'],'unknown')
        self.assertEqual(self.backend.stop_count,1)
        self.backend.linux=self.backend.windows='gone'
        self.assertEqual(self.manager.status()['status'],'stopped')
        self.assertEqual(self.manager.stop()['status'],'already_stopped')
        self.assertEqual(self.backend.stop_count,1)
    def test_linux_gone_with_windows_alive_is_unknown(self):
        self.manager.ensure(); self.backend.linux='gone'
        self.assertEqual(self.manager.ensure()['status'],'unknown')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.backend.launch_count,1); self.assertEqual(self.backend.stop_count,0)
    def test_both_gone_can_start_a_new_keeper(self):
        self.manager.ensure(); self.backend.linux=self.backend.windows='gone'
        self.assertEqual(self.manager.ensure()['status'],'started')
        self.assertEqual(self.backend.launch_count,2)
    def test_changed_helper_fails_closed(self):
        self.manager.ensure(); state=keepalive.read_state(self.root)
        (self.root/(state['id']+'.py')).write_bytes(b'changed')
        self.assertEqual(self.manager.stop()['status'],'unknown')
        self.assertEqual(self.backend.stop_count,0)
    def test_corrupt_state_is_not_adopted(self):
        (self.root/'state.json').write_text('{"schema":1,"stage":"running","distro":"Ubuntu","id":"bad"}',encoding='utf-8')
        with self.assertRaises(ValueError): self.manager.ensure()
        self.assertEqual(self.backend.launch_count,0)


class LinuxIdentityTests(unittest.TestCase):
    def setUp(self):
        self.keeper_id=str(uuid.uuid4()); self.boot='aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
    def test_matching_identity_signals_only_pinned_pidfd(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=self.boot), \
             mock.patch.object(linux_keeper,'start_ticks',return_value=42), \
             mock.patch.object(linux_keeper,'same_command',return_value=True), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True,return_value=77) as opened, \
             mock.patch.object(linux_keeper.signal,'pidfd_send_signal',create=True) as sent, \
             mock.patch.object(linux_keeper.os,'close') as closed:
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'stop_sent'); opened.assert_called_once_with(1234,0)
        self.assertEqual(sent.call_args_list,[mock.call(77,0),mock.call(77,linux_keeper.signal.SIGTERM)])
        closed.assert_called_once_with(77)
    def test_reused_pid_is_never_signalled(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=self.boot), \
             mock.patch.object(linux_keeper,'start_ticks',return_value=43), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True,return_value=77), \
             mock.patch.object(linux_keeper.signal,'pidfd_send_signal',create=True) as sent, \
             mock.patch.object(linux_keeper.os,'close'):
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'gone'); sent.assert_not_called()
    def test_other_command_is_never_signalled(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=self.boot), \
             mock.patch.object(linux_keeper,'start_ticks',return_value=42), \
             mock.patch.object(linux_keeper,'same_command',return_value=False), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True,return_value=77), \
             mock.patch.object(linux_keeper.signal,'pidfd_send_signal',create=True) as sent, \
             mock.patch.object(linux_keeper.os,'close'):
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'unknown'); sent.assert_not_called()
    def test_changed_boot_never_opens_or_signals_pid(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=str(uuid.uuid4())), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True) as opened:
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'gone'); opened.assert_not_called()
    def test_identity_change_during_inspection_never_signals(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=self.boot), \
             mock.patch.object(linux_keeper,'start_ticks',side_effect=[42,43]), \
             mock.patch.object(linux_keeper,'same_command',return_value=True), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True,return_value=77), \
             mock.patch.object(linux_keeper.signal,'pidfd_send_signal',create=True) as sent, \
             mock.patch.object(linux_keeper.os,'close'):
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'unknown'); sent.assert_not_called()
    def test_no_pidfd_support_never_falls_back_to_pid_kill(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=self.boot), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True,new=None), \
             mock.patch.object(linux_keeper.os,'kill') as killed:
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'unknown'); self.assertEqual(result['reason'],'pidfd_unavailable')
        killed.assert_not_called()
    def test_missing_pid_is_gone_without_signal(self):
        with mock.patch.object(linux_keeper,'boot_id',return_value=self.boot), \
             mock.patch.object(linux_keeper.os,'pidfd_open',create=True,side_effect=OSError(errno.ESRCH,'gone')), \
             mock.patch.object(linux_keeper.signal,'pidfd_send_signal',create=True) as sent:
            result=linux_keeper.inspect(1234,42,self.boot,self.keeper_id,True)
        self.assertEqual(result['status'],'gone'); sent.assert_not_called()
    def test_stat_parses_comm_with_parentheses_and_spaces(self):
        fields=['S']+['0']*18+['123456']+['0']*5
        raw='123 (strange ) process) '+' '.join(fields)
        with mock.patch.object(Path,'read_text',return_value=raw):
            self.assertEqual(linux_keeper.start_ticks(123),123456)


class CommandExitTests(unittest.TestCase):
    def test_running_status_is_successful(self):
        manager=mock.Mock()
        manager.status.return_value={'status':'running','windows':'running'}
        with mock.patch.object(sys,'argv',['keepalive.py','status']), \
             mock.patch.object(keepalive,'root_directory',return_value=Path('unused')), \
             mock.patch.object(keepalive,'prepare_directory'), \
             mock.patch.object(keepalive.capture,'directory_lock',return_value=nullcontext()), \
             mock.patch.object(keepalive,'WindowsBackend'), \
             mock.patch.object(keepalive,'Manager',return_value=manager), \
             redirect_stdout(io.StringIO()):
            self.assertEqual(keepalive.main(),0)


class WindowsArgumentTests(unittest.TestCase):
    @unittest.skipUnless(os.name=='nt','Windows command line parsing')
    def test_command_line_with_spaces_roundtrips(self):
        args=[r'C:\\Windows\\System32\\wsl.exe','-d','Ubuntu','-u','root','--exec','/usr/bin/python3',
              '/mnt/c/Users/a user/helper.py','serve',str(uuid.uuid4())]
        self.assertEqual(keepalive.WindowsBackend.arguments(subprocess.list2cmdline(args)),args)
    @unittest.skipUnless(os.name=='nt','Windows process identity')
    def test_unrelated_process_and_reused_pid_fail_closed(self):
        k=ctypes.WinDLL('kernel32',use_last_error=True)
        k.GetCurrentProcess.restype=wintypes.HANDLE
        k.GetProcessTimes.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME)]
        k.GetProcessTimes.restype=wintypes.BOOL
        vals=[wintypes.FILETIME() for _ in range(4)]
        self.assertTrue(k.GetProcessTimes(k.GetCurrentProcess(),*(ctypes.byref(x) for x in vals)))
        created=(vals[0].dwHighDateTime<<32)|vals[0].dwLowDateTime
        state={'windows':{'pid':os.getpid(),'created':created},'linux_path':'/not-used','id':str(uuid.uuid4())}
        backend=keepalive.WindowsBackend(Path(tempfile.gettempdir()).resolve())
        self.assertEqual(backend.windows_status(state)['status'],'unknown')
        state['windows']['created']+=1
        self.assertEqual(backend.windows_status(state)['status'],'gone')


class WindowsSpoolTests(OwnedTempTest):
    @unittest.skipUnless(os.name=='nt','Windows process and inherited handle fixture')
    def test_launcher_returns_while_owned_descendant_keeps_handles(self):
        # Reproduce Start-Process with a hidden long-lived child and redirected
        # streams. The child stays alive longer than the launcher timeout.
        done=self.root/'child.done'; child=self.root/'child.py'; launcher=self.root/'launcher.ps1'
        child.write_text("import time\nfrom pathlib import Path\ntime.sleep(4)\nPath("+repr(str(done))+").write_text('done')\n",encoding='utf-8')
        for name in ('stdin','stdout','stderr'): (self.root/name).write_bytes(b'')
        launcher.write_text("""param([string]$Python,[string]$Child,[string]$Root)
$ErrorActionPreference='Stop'
$p=Start-Process -FilePath $Python -ArgumentList @(('"'+$Child+'"')) -WindowStyle Hidden -PassThru `
 -RedirectStandardInput (Join-Path $Root 'stdin') -RedirectStandardOutput (Join-Path $Root 'stdout') -RedirectStandardError (Join-Path $Root 'stderr')
[Console]::Out.WriteLine($p.Id)
""",encoding='utf-8')
        backend=keepalive.WindowsBackend(self.root)
        started=time.monotonic()
        try:
            result=backend.run([backend.powershell,'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
                '-File',launcher,'-Python',sys.executable,'-Child',child,'-Root',self.root],timeout=2)
            elapsed=time.monotonic()-started
            self.assertEqual(result.returncode,0)
            self.assertTrue(result.stdout.strip().isdigit())
            self.assertLess(elapsed,3.0,'launcher was blocked by its descendant')
            self.assertFalse(done.exists(),'fixture must still be running when launcher returns')
        finally:
            deadline=time.monotonic()+10
            while not done.exists() and time.monotonic()<deadline: time.sleep(.1)
            if not done.exists():
                self.keep_dir=True
                self.fail('owned descendant did not finish; fixture retained')
    @unittest.skipUnless(os.name=='nt','Windows bounded process wait')
    def test_timeout_kills_only_direct_test_process_and_returns(self):
        backend=keepalive.WindowsBackend(self.root); started=time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired):
            backend.run([sys.executable,'-c','import time; time.sleep(5)'],timeout=.25)
        self.assertLess(time.monotonic()-started,3)


if __name__=='__main__': unittest.main()
