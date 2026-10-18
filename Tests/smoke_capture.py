"""Native capture-engine integration checks. No mouse or keyboard automation.
Run: python Tests/smoke_capture.py [Release|Debug]
"""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys
import time

if sys.platform != "win32":
    raise SystemExit("These integration checks require Windows.")

u = c.WinDLL("user32", use_last_error=True)
k = c.WinDLL("kernel32", use_last_error=True)
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [CALLBACK, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetPropW.argtypes = [w.HWND, w.LPCWSTR]
u.GetPropW.restype = w.HANDLE
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
u.SendMessageTimeoutW.restype = w.LPARAM
k.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
k.CreateJobObjectW.restype = w.HANDLE
k.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
k.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
k.CloseHandle.argtypes = [w.HANDLE]

class BASIC(c.Structure):
    _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64), ("flags", w.DWORD),
                ("min_working_set", c.c_size_t), ("max_working_set", c.c_size_t),
                ("active_processes", w.DWORD), ("affinity", c.c_size_t),
                ("priority", w.DWORD), ("scheduling", w.DWORD)]

class IO(c.Structure):
    _fields_ = [(name, c.c_uint64) for name in ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

class EXTENDED(c.Structure):
    _fields_ = [("basic", BASIC), ("io", IO), ("process_memory", c.c_size_t),
                ("job_memory", c.c_size_t), ("peak_process_memory", c.c_size_t), ("peak_job_memory", c.c_size_t)]

root = Path(__file__).resolve().parents[1]
configuration = sys.argv[1] if len(sys.argv) > 1 else "Release"
assert configuration in ("Release", "Debug")
exe = root / "artifacts" / "x64" / configuration / "ScreenShotPlus.Host.exe"
assert exe.is_file(), f"Build {configuration}|x64 first: {exe}"

def launch():
    return subprocess.Popen([str(exe), "--aelura-host"], cwd=exe.parent)

def ready(process):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        found = []
        @CALLBACK
        def callback(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, c.byref(pid))
            if pid.value == process.pid and u.GetPropW(hwnd, "ScreenShotPlus.Ready"):
                found.append(hwnd)
            return True
        u.EnumWindows(callback, 0)
        if found:
            return found[0]
        assert process.poll() is None, f"Engine exited early: {process.returncode}"
        time.sleep(.05)
    raise AssertionError("Engine did not become ready")

def request(hwnd, command):
    result = c.c_size_t()
    if not u.SendMessageTimeoutW(hwnd, 0x8000 + 120, command, 0, 3, 2000, c.byref(result)):
        raise c.WinError(c.get_last_error())
    return result.value

# Exercise real window dispatch and graceful component shutdown without capturing the screen.
process = launch()
try:
    hwnd = ready(process)
    name = c.create_unicode_buffer(256)
    u.GetClassNameW(hwnd, name, len(name))
    assert name.value == "AeluraTool.ScreenShotPlus.Panel", name.value
    assert not u.GetPropW(hwnd, "ScreenShotPlus.PrimaryInstance"), "Must not impersonate the standalone app"
    assert request(hwnd, 5) == 1, "Idle engine should accept capture"
    assert request(hwnd, 999) == 0, "Unknown commands must be rejected"
    assert request(hwnd, 4) == 1, "Shutdown must be acknowledged"
    assert process.wait(timeout=5) == 0
    print("PASS: isolated engine identity, ready handshake, unknown command, graceful shutdown")
finally:
    if process.poll() is None:
        process.kill()
        process.wait(timeout=5)

# Verify the exact OS job policy used by the toolbox also covers a ready engine.
job = k.CreateJobObjectW(None, None)
assert job, c.WinError(c.get_last_error())
process = None
try:
    limits = EXTENDED()
    limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    assert k.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)), c.WinError(c.get_last_error())
    process = launch()
    assert k.AssignProcessToJobObject(job, w.HANDLE(int(process._handle))), c.WinError(c.get_last_error())
    ready(process)
    k.CloseHandle(job)
    job = None
    process.wait(timeout=5)
    print("PASS: owner job close terminates the managed capture process")
finally:
    if job:
        k.CloseHandle(job)
    if process and process.poll() is None:
        process.kill()
        process.wait(timeout=5)
