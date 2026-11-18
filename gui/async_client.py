"""Serialized IPC worker with cached reads and coalesced control updates."""
import json
import threading
import time
from collections import OrderedDict
from PyQt6.QtCore import QObject, pyqtSignal
from gui.pipe_client import MuxellaPipeClient


class AsyncPipeClient(QObject, MuxellaPipeClient):
    devices_ready = pyqtSignal(object)
    task_finished = pyqtSignal(str, object)
    attachment_finished = pyqtSignal(object)
    reply_ready = pyqtSignal(str, object)

    def __init__(self, raw):
        QObject.__init__(self)
        self.raw = raw
        self.connected = raw.connected
        self._condition = threading.Condition()
        self._pending = OrderedDict()
        self._cache = {}
        self._cache_time = {}
        self._active = None
        self._stopping = False
        self._thread = threading.Thread(target=self._run, name="Muxella IPC", daemon=True)
        self._thread.start()

    def _enqueue(self, key, command):
        with self._condition:
            if self._stopping or self._active == (key, command):
                return
            self._pending[key] = command
            self._condition.notify()

    def send_command(self, command):
        words = command.split()
        if not words:
            return None
        verb = words[0]
        cached_read = verb in ("status", "devices", "meters", "health", "apps", "record_status", "get_bus_eq")
        key = command if cached_read else " ".join(words[:3 if verb == "route" else 2])
        self._enqueue(key, command)
        with self._condition:
            if not cached_read:
                return None
            lifetime = 1.0 if verb in ("meters", "health", "apps", "record_status") else 10.0
            if time.monotonic() - self._cache_time.get(command, 0) > lifetime:
                return None
            return self._cache.get(command)

    def attach_input(self, index, query):
        self.send_command(f"attach_input {index} {query}")
        return None  # Pending, never report a queued attachment as successful.

    def attach_output(self, index, query):
        self.send_command(f"attach_output {index} {query}")
        return None

    def submit_task(self, name, callback):
        self._enqueue("task:" + name, callback)

    def _run(self):
        while True:
            with self._condition:
                self._condition.wait_for(lambda: self._pending or self._stopping)
                if self._stopping:
                    return
                key, command = self._pending.popitem(last=False)
                self._active = (key, command)
            try:
                if callable(command):
                    result = command()
                    self.task_finished.emit(key[5:], result)
                else:
                    response = self.raw.send_command(command, timeout=20.0 if command == "record_stop" else 3.0)
                    self.connected = self.raw.connected
                    if command.split()[0] in ("apps", "app_set", "record_start", "record_stop", "record_status"):
                        self.reply_ready.emit(command.split()[0], json.loads(response) if response else
                                              {"status": "error", "message": "Engine did not respond"})
                    if command.startswith(("attach_input ", "attach_output ")):
                        fields = command.split(" ", 2)
                        ok = bool(response and json.loads(response).get("status") == "ok")
                        self.attachment_finished.emit({"kind": fields[0][7:], "index": int(fields[1]),
                                                       "query": fields[2], "ok": ok})
                    if response and command.split()[0] in ("status", "devices", "meters", "health", "apps", "record_status", "get_bus_eq"):
                        with self._condition:
                            self._cache[command] = response
                            self._cache_time[command] = time.monotonic()
                        if command == "devices":
                            self.devices_ready.emit(json.loads(response))
                    if command.startswith(("attach_", "detach_")):
                        self._enqueue("devices", "devices")
            except Exception as exc:
                if callable(command):
                    self.task_finished.emit(key[5:], {"error": str(exc)})
                elif command.split()[0] in ("apps", "app_set", "record_start", "record_stop", "record_status"):
                    self.reply_ready.emit(command.split()[0], {"status": "error", "message": str(exc)})
            finally:
                with self._condition:
                    self._active = None

    def close(self):
        with self._condition:
            self._stopping = True
            self._pending.clear()
            self._condition.notify()
        self._thread.join(timeout=15)
