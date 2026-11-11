"""Read-only Windows process accounting, sampled by the existing background worker."""
import ctypes
from ctypes import wintypes
import os
import time


class ProcessEntry(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('pid', wintypes.DWORD),
                ('heap', ctypes.c_size_t), ('module', wintypes.DWORD), ('threads', wintypes.DWORD),
                ('parent', wintypes.DWORD), ('priority', wintypes.LONG), ('flags', wintypes.DWORD),
                ('exe', wintypes.WCHAR * 260)]


class MemoryCounters(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
        'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage',
        'PeakPagefileUsage', 'PrivateUsage')]


class PerformanceSampler:
    def __init__(self):
        self.previous = {}
        self.previous_time = None
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.psapi = ctypes.WinDLL('psapi', use_last_error=True)
        self.kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        self.kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        for name in ('Process32FirstW', 'Process32NextW'):
            call = getattr(self.kernel, name)
            call.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
            call.restype = wintypes.BOOL
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = wintypes.BOOL
        self.psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
        self.psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    @staticmethod
    def ticks(value):
        return (value.dwHighDateTime << 32) | value.dwLowDateTime

    def sample(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            return {'error': 'Process statistics unavailable'}
        processes = {}
        try:
            entry = ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            ok = self.kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while ok:
                processes[entry.pid] = (entry.parent, entry.exe.lower())
                ok = self.kernel.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            self.kernel.CloseHandle(snapshot)
        selected = {os.getpid()}
        parent = processes.get(os.getpid(), (0, ''))[0]
        if processes.get(parent, (0, ''))[1] == 'muxella.exe':
            selected.add(parent)  # Include the one-file bootloader.
        # Include the elevated cable broker even if UAC reparents it.
        selected.update(pid for pid, (_, exe) in processes.items()
                        if exe in ('metvoice_server.exe', 'muxella_cables.exe'))
        while True:
            children = {pid for pid, (parent, _) in processes.items() if parent in selected}
            if children <= selected:
                break
            selected.update(children)
        current = {}
        working_set = 0
        memory_readable = 0
        for pid in selected:
            handle = self.kernel.OpenProcess(0x1000 | 0x10, False, pid)  # limited query + VM_READ
            if not handle:
                handle = self.kernel.OpenProcess(0x1000, False, pid)
            if not handle:
                continue
            try:
                created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
                if self.kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                               ctypes.byref(kernel), ctypes.byref(user)):
                    current[pid] = (self.ticks(created), self.ticks(kernel) + self.ticks(user))
                counters = MemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                if self.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                    working_set += counters.WorkingSetSize
                    memory_readable += 1
            finally:
                self.kernel.CloseHandle(handle)
        now = time.monotonic()
        cpu = None
        if self.previous_time is not None and current:
            elapsed = now - self.previous_time
            used = sum(max(0, ticks - self.previous[pid][1]) for pid, (created, ticks) in current.items()
                       if pid in self.previous and created == self.previous[pid][0])
            if elapsed > 0:
                cpu = min(100.0, used / 10_000_000 / elapsed / max(1, os.cpu_count() or 1) * 100)
        self.previous, self.previous_time = current, now
        return {'cpu': cpu, 'ram_bytes': working_set if memory_readable else None,
                'processes': len(selected), 'cpu_readable': len(current), 'ram_readable': memory_readable,
                'partial': len(current) < len(selected) or memory_readable < len(selected)}
