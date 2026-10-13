import ctypes
from ctypes import wintypes
import json
import logging
import time
from typing import Optional, Dict, Any

logger = logging.getLogger("metvoice.pipe")

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


class MetVoicePipeClient:
    def __init__(self, pipe_name: str = r"\\.\pipe\MetVoice_Control"):
        self.pipe_name = pipe_name
        self.connected = False

    def send_command(self, cmd_text: str) -> Optional[str]:
        h_pipe = CreateFileW(
            self.pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            None,
            OPEN_EXISTING,
            0,
            None
        )

        if h_pipe == INVALID_HANDLE_VALUE:
            self.connected = False
            return None

        self.connected = True
        try:
            mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
            SetNamedPipeHandleState(h_pipe, ctypes.byref(mode), None, None)

            # Write request
            cmd_bytes = cmd_text.encode("utf-8")
            written = wintypes.DWORD(0)
            success = WriteFile(h_pipe, cmd_bytes, len(cmd_bytes), ctypes.byref(written), None)
            if not success:
                return None

            # Read response
            buffer = ctypes.create_string_buffer(8192)
            bytes_read = wintypes.DWORD(0)
            success = ReadFile(h_pipe, buffer, len(buffer) - 1, ctypes.byref(bytes_read), None)
            if success and bytes_read.value > 0:
                raw_resp = buffer.raw[:bytes_read.value].decode("utf-8", errors="replace")
                return raw_resp
            return None
        finally:
            CloseHandle(h_pipe)

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
