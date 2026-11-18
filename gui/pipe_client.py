import ctypes
from ctypes import wintypes
import json
import logging
import time
import threading
from typing import Optional, Dict, Any

logger = logging.getLogger("muxella.pipe")

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PIPE_READMODE_MESSAGE = 0x00000002

kernel32 = ctypes.windll.kernel32

CreateFileW = kernel32.CreateFileW
CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
    ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE
]
CreateFileW.restype = wintypes.HANDLE

WriteFile = kernel32.WriteFile
WriteFile.argtypes = [
    wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p
]
WriteFile.restype = wintypes.BOOL

ReadFile = kernel32.ReadFile
ReadFile.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p
]
ReadFile.restype = wintypes.BOOL

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL

SetNamedPipeHandleState = kernel32.SetNamedPipeHandleState
SetNamedPipeHandleState.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p, ctypes.c_void_p
]
SetNamedPipeHandleState.restype = wintypes.BOOL


ERROR_PIPE_BUSY = 231

WaitNamedPipeW = kernel32.WaitNamedPipeW
WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
WaitNamedPipeW.restype = wintypes.BOOL

class _Overlapped(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", wintypes.HANDLE)]

kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Overlapped), ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
kernel32.GetOverlappedResult.restype = wintypes.BOOL
kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Overlapped)]
kernel32.CancelIoEx.restype = wintypes.BOOL

def _transfer(handle, buffer, size, write, deadline):
    operation = _Overlapped()
    operation.hEvent = kernel32.CreateEventW(None, True, False, None)
    if not operation.hEvent:
        return 0, kernel32.GetLastError()
    try:
        call = WriteFile if write else ReadFile
        ok = call(handle, buffer, size, None, ctypes.byref(operation))
        code = 0 if ok else kernel32.GetLastError()
        if code not in (0, 997, 234):
            return 0, code
        if code == 997:
            remaining = max(0, int((deadline - time.monotonic()) * 1000))
            if kernel32.WaitForSingleObject(operation.hEvent, remaining) != 0:
                kernel32.CancelIoEx(handle, ctypes.byref(operation))
                count = wintypes.DWORD()
                kernel32.GetOverlappedResult(handle, ctypes.byref(operation), ctypes.byref(count), True)
                return 0, 1460
        count = wintypes.DWORD()
        ok = kernel32.GetOverlappedResult(handle, ctypes.byref(operation), ctypes.byref(count), False)
        return count.value, 0 if ok else kernel32.GetLastError()
    finally:
        CloseHandle(operation.hEvent)

class MuxellaPipeClient:
    def __init__(self, pipe_name: str = r"\\.\pipe\Muxella_Control"):
        self.pipe_name = pipe_name
        self.connected = False
        self._lock = threading.Lock()

    def send_command(self, cmd_text: str, timeout: float = 3.0) -> Optional[str]:
        # RPC runs on the GUI's worker. Overlapped I/O gives each transaction a
        # real deadline and supports replies larger than one buffer.
        with self._lock:
            handle = INVALID_HANDLE_VALUE
            for _ in range(5):
                handle = CreateFileW(self.pipe_name, GENERIC_READ | GENERIC_WRITE,
                                     0, None, OPEN_EXISTING, 0x40000000, None)
                if handle != INVALID_HANDLE_VALUE:
                    break
                if kernel32.GetLastError() == ERROR_PIPE_BUSY:
                    WaitNamedPipeW(self.pipe_name, 100)
                else:
                    time.sleep(0.01)
            if handle == INVALID_HANDLE_VALUE:
                self.connected = False
                return None
            self.connected = True
            deadline = time.monotonic() + timeout
            try:
                mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
                SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None)
                data = cmd_text.encode("utf-8")
                payload = ctypes.create_string_buffer(data)
                _, code = _transfer(handle, payload, len(data), True, deadline)
                if code:
                    return None
                chunks = []
                total = 0
                while total < 1024 * 1024:
                    buffer = ctypes.create_string_buffer(8192)
                    count, code = _transfer(handle, buffer, len(buffer), False, deadline)
                    if code not in (0, 234):  # ERROR_MORE_DATA
                        return None
                    chunks.append(buffer.raw[:count])
                    total += count
                    if code == 0:
                        ack = ctypes.create_string_buffer(b"!")
                        _transfer(handle, ack, 1, True, deadline)
                        return b"".join(chunks).decode("utf-8", errors="replace")
                return None
            finally:
                CloseHandle(handle)

    def get_meters(self) -> Optional[Dict[str, Any]]:
        resp = self.send_command("meters")
        if not resp:
            return None
        try:
            return json.loads(resp)
        except Exception:
            return None

    def set_gain(self, strip_idx: int, gain_db: float):
        self.send_command(f"gain {strip_idx} {gain_db:.1f}")

    def set_pan(self, strip_idx: int, pan: float):
        self.send_command(f"pan {strip_idx} {pan:.2f}")

    def set_mute(self, strip_idx: int, mute: bool):
        self.send_command(f"mute {strip_idx} {1 if mute else 0}")

    def set_solo(self, strip_idx: int, solo: bool):
        self.send_command(f"solo {strip_idx} {1 if solo else 0}")

    def set_route(self, strip_idx: int, bus_idx: int, enabled: bool, gain_db: float = 0.0):
        self.send_command(f"route {strip_idx} {bus_idx} {1 if enabled else 0} {gain_db:.1f}")

    def set_bus_gain(self, bus_idx: int, gain_db: float):
        self.send_command(f"bus_gain {bus_idx} {gain_db:.1f}")

    def set_bus_mute(self, bus_idx: int, mute: bool):
        self.send_command(f"bus_mute {bus_idx} {1 if mute else 0}")

    def set_eq(self, strip_idx: int, low_db: float, mid_db: float, high_db: float):
        self.send_command(f"eq {strip_idx} {low_db:.1f} {mid_db:.1f} {high_db:.1f}")

    def set_comp(self, strip_idx: int, amount: float):
        self.send_command(f"comp {strip_idx} {amount:.1f}")

    def set_gate(self, strip_idx: int, amount: float):
        self.send_command(f"gate {strip_idx} {amount:.1f}")

    def set_denoise(self, strip_idx: int, amount: float):
        self.send_command(f"denoise {strip_idx} {amount:.1f}")

    def set_reverb(self, strip_idx: int, amount: float):
        self.send_command(f"reverb {strip_idx} {amount:.1f}")

    def set_duck(self, strip_idx: int, trigger_idx: int, amount: float):
        self.send_command(f"duck {strip_idx} {int(trigger_idx)} {amount:.1f}")

    def set_bus_delay(self, bus_idx: int, delay_ms: float):
        self.send_command(f"bus_delay {bus_idx} {delay_ms:.1f}")

    def set_strip_mono(self, strip_idx: int, mono: bool):
        self.send_command(f"strip_mono {strip_idx} {1 if mono else 0}")

    def set_bus_mono(self, bus_idx: int, mono: bool):
        self.send_command(f"bus_mono {bus_idx} {1 if mono else 0}")

    def set_bus_eq(self, bus_idx: int, eq: bool):
        self.send_command(f"bus_eq {bus_idx} {1 if eq else 0}")

    def set_bus_eq_params(self, bus_idx: int, low_db: float, mid_db: float, high_db: float,
                          low_freq: float = 100.0, mid_freq: float = 1000.0, high_freq: float = 8000.0, mid_q: float = 1.0):
        self.send_command(f"bus_set_eq {bus_idx} {low_db:.1f} {mid_db:.1f} {high_db:.1f} {low_freq:.1f} {mid_freq:.1f} {high_freq:.1f} {mid_q:.2f}")

    def get_bus_eq_params(self, bus_idx: int) -> Optional[Dict[str, Any]]:
        resp = self.send_command(f"get_bus_eq {bus_idx}")
        if resp:
            try:
                return json.loads(resp)
            except Exception:
                pass
        return None

    def get_devices(self) -> Optional[Dict[str, Any]]:
        resp = self.send_command("devices")
        if not resp:
            return None
        try:
            return json.loads(resp)
        except Exception:
            return None

    def rename_strip(self, strip_idx: int, name: str):
        self.send_command(f"rename_strip {strip_idx} {name}")

    def rename_bus(self, bus_idx: int, name: str):
        self.send_command(f"rename_bus {bus_idx} {name}")

    def attach_input(self, strip_idx: int, device_query: str) -> bool:
        response = self.send_command(f"attach_input {strip_idx} {device_query}")
        if not response:
            return False
        try:
            return json.loads(response).get("status") == "ok"
        except (ValueError, TypeError):
            return False

    def attach_output(self, bus_idx: int, device_query: str) -> bool:
        response = self.send_command(f"attach_output {bus_idx} {device_query}")
        if not response:
            return False
        try:
            return json.loads(response).get("status") == "ok"
        except (ValueError, TypeError):
            return False


# Backward compatibility alias
MuxellaPipeClient = MuxellaPipeClient


