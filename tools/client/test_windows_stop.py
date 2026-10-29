"""Exercise guarded, ancestry-checked WM_CLOSE using a hidden native test window."""
import ctypes
from ctypes import wintypes
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

if sys.platform != "win32":
    raise SystemExit("Windows only")
spec = importlib.util.spec_from_file_location("device_login", Path(__file__).with_name("device_login.py"))
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
guard_script = client.ROOT / "Scripts" / "Invoke-MRFPSResourceLimited.ps1"

fixture = r'''
import ctypes, sys
from ctypes import wintypes
u = ctypes.WinDLL("user32", use_last_error=True)
k = ctypes.WinDLL("kernel32", use_last_error=True)
callback = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
u.DefWindowProcW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM]
u.DefWindowProcW.restype=ctypes.c_ssize_t
u.DestroyWindow.argtypes=[wintypes.HWND]
u.PostQuitMessage.argtypes=[ctypes.c_int]
@callback
def proc(hwnd, message, wparam, lparam):
    if message==0x10: u.DestroyWindow(hwnd); return 0
    if message==0x02: u.PostQuitMessage(0); return 0
    return u.DefWindowProcW(hwnd,message,wparam,lparam)
class WNDCLASS(ctypes.Structure):
    _fields_=[("style",wintypes.UINT),("proc",callback),("cls_extra",ctypes.c_int),("wnd_extra",ctypes.c_int),
              ("instance",wintypes.HINSTANCE),("icon",wintypes.HICON),("cursor",wintypes.HANDLE),
              ("background",wintypes.HBRUSH),("menu",wintypes.LPCWSTR),("class_name",wintypes.LPCWSTR)]
k.GetModuleHandleW.argtypes=[wintypes.LPCWSTR]
k.GetModuleHandleW.restype=wintypes.HMODULE
instance=k.GetModuleHandleW(None)
cls=WNDCLASS(); cls.proc=proc; cls.instance=instance; cls.class_name="MRFPSStopFixtureClass"
u.RegisterClassW.argtypes=[ctypes.POINTER(WNDCLASS)]
if not u.RegisterClassW(ctypes.byref(cls)): raise SystemExit(2)
u.CreateWindowExW.argtypes=[wintypes.DWORD,wintypes.LPCWSTR,wintypes.LPCWSTR,wintypes.DWORD,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,wintypes.HWND,wintypes.HANDLE,wintypes.HINSTANCE,wintypes.LPVOID]
u.CreateWindowExW.restype=wintypes.HWND
window=u.CreateWindowExW(0,cls.class_name,sys.argv[1],0,0,0,1,1,None,None,instance,None)
if not window: raise SystemExit(3)
message=wintypes.MSG()
while u.GetMessageW(ctypes.byref(message),None,0,0)>0:
    u.TranslateMessage(ctypes.byref(message)); u.DispatchMessageW(ctypes.byref(message))
'''


def quoted(value):
    return "'" + str(value).replace("'", "''") + "'"


def creation_time(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME),ctypes.POINTER(wintypes.FILETIME)]
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    handle=kernel.OpenProcess(0x1000,False,pid)
    if not handle: raise RuntimeError("guard exited before identity check")
    try:
        created,exited,system,user=(wintypes.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle,ctypes.byref(created),ctypes.byref(exited),ctypes.byref(system),ctypes.byref(user)):
            raise RuntimeError("cannot read guard creation time")
        return (created.dwHighDateTime<<32)|created.dwLowDateTime
    finally:
        kernel.CloseHandle(handle)


with tempfile.TemporaryDirectory(prefix="mrfps-stop-") as folder:
    root=Path(folder)
    name="MRFPS-StopProbe-"+uuid.uuid4().hex[:8]
    window_script=root/"window.py"; window_script.write_text(fixture,encoding="utf-8")
    child_script=root/"child.ps1"; child_script.write_text("& "+quoted(sys.executable)+" "+quoted(window_script)+" "+quoted(name)+"\nexit $LASTEXITCODE\n",encoding="utf-8")
    report=root/"guard"/"report.json"
    outer_script=root/"outer.ps1"; outer_script.write_text("& "+quoted(guard_script)+" -ScriptFile "+quoted(child_script)+" -MinimumFreeGB 10 -MaximumCommitPercent 75 -ReportPath "+quoted(report)+"\n",encoding="utf-8")
    process=subprocess.Popen(["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",str(outer_script)],creationflags=subprocess.CREATE_NO_WINDOW,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        state_path=root/"Saved"/"DeviceAuth"/("windows-"+name+".json")
        client.save_state(state_path,{"pid":process.pid,"created":creation_time(process.pid),"report":str(report)})
        user32=ctypes.WinDLL("user32",use_last_error=True)
        user32.FindWindowW.argtypes=[wintypes.LPCWSTR,wintypes.LPCWSTR]; user32.FindWindowW.restype=wintypes.HWND
        deadline=time.monotonic()+30
        while not user32.FindWindowW(None,name):
            if process.poll() is not None: raise RuntimeError("fixture failed before window creation")
            if time.monotonic()>deadline: raise RuntimeError("fixture window timed out")
            time.sleep(.1)
        client.ROOT=root
        result=client.stop_windows(type("Args",(),{"name":name})())
        if result["status"]!="stopped" or process.wait(timeout=10)!=0:
            raise RuntimeError("guarded stop did not complete cleanly")
        if client.stop_windows(type("Args",(),{"name":name})())["status"]!="already_stopped":
            raise RuntimeError("repeated stop was not idempotent")
        data=json.loads(report.read_text(encoding="utf-8-sig"))
        if data["status"]!="completed" or not data["jobAssigned"] or data["minimumFreeGB"]!=10 or data["maximumCommitPercent"]!=75:
            raise RuntimeError("resource guard did not supervise fixture")
        print("Guarded window stop passed; repeat is a no-op.")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
