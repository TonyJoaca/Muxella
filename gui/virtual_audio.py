"""Own the cable host without running the mixer or its USB driver elevated."""
import os
import json
import shutil
import subprocess
import sys
from pathlib import Path


def device_matches(selected, present):
    selected, present = selected.strip().casefold(), present.strip().casefold()
    if selected == present:
        return True
    aliases = {"muxella main", "muxella spotify", "muxella discord",
               "muxella out b1", "muxella out b2", "muxella out b3"}
    return selected in aliases and (present.startswith(selected + " (") or
                                    "(" + selected + ")" in present)


def resolve_selection(name, endpoint_id, preferred, secondary=()):
    if endpoint_id:
        return next((item for item in list(preferred) + list(secondary)
                     if item.get("id") == endpoint_id), None)
    if name.strip().casefold().startswith("muxella "):
        secondary = ()  # A canonical recording choice never becomes loopback.
    for devices in (preferred, secondary):
        matches = [item for item in devices if device_matches(name, item.get("name", ""))]
        if matches:
            return matches[0] if len(matches) == 1 else None
    return None


def migrate_device_name(name):
    aliases = {
        "MetVoice Main VAIO": "Muxella Main",
        "MetVoice Spotify VAIO": "Muxella Spotify",
        "MetVoice Discord VAIO": "Muxella Discord",
        "MetVoice Out B1": "Muxella Out B1",
        "MetVoice Out B2": "Muxella Out B2",
        "Voicemeeter Out B3": "Muxella Out B3",
    }
    for old, new in aliases.items():
        if name == old or name.startswith(old + " ("):
            return new
    return name


class CableProcess:
    def __init__(self):
        self.process = None
        self.error = ""
        self.driver_seen = False
        self.config_path = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "Muxella" / "virtual_audio.json"
        self.bits = 24
        try:
            bits = json.loads(self.config_path.read_text(encoding="utf-8")).get("bits", 24)
            if bits in (16, 24, 32):
                self.bits = bits
        except (OSError, ValueError, TypeError):
            pass

    @staticmethod
    def driver_installed():
        return bool(shutil.which("usbip.exe")) or any((Path(os.environ.get("ProgramFiles", "")) / folder / "usbip.exe").exists()
                   for folder in ("USBip", "usbip-win2", "usbip"))

    def executable(self):
        roots = [Path(__file__).resolve().parent.parent / "build",
                 Path(__file__).resolve().parent.parent]
        if getattr(sys, "frozen", False):
            roots = [Path(getattr(sys, "_MEIPASS", "")), Path(sys.executable).parent]
        return next((p / "muxella_cables.exe" for p in roots
                     if (p / "muxella_cables.exe").exists()), None)

    def start(self):
        if self.process and self.process.poll() is None:
            return
        exe = self.executable()
        if not exe:
            self.error = "muxella_cables.exe is missing; rebuild the cable helper."
            return
        try:
            self.driver_seen = self.driver_installed()
            self.process = subprocess.Popen(
                [str(exe), "--headless", "--cables", "6", "--bits", str(self.bits), "--buffer-ms", "100"],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        except OSError as exc:
            self.error = str(exc)

    def install_driver(self):
        exe = self.executable()
        if exe:
            subprocess.Popen([str(exe), "--install-driver"],
                             creationflags=subprocess.CREATE_NO_WINDOW)

    def set_bits(self, bits, should_stop=lambda: False):
        if bits not in (16, 24, 32) or bits == self.bits:
            return
        self.stop()
        if should_stop():
            return
        self.bits = bits
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        self.config_path.write_text(json.dumps({"bits": bits}), encoding="utf-8")
        self.start()

    def refresh(self):
        # Installing the transport after Muxella starts must also attach devices.
        installed = self.driver_installed()
        if installed and not self.driver_seen and self.process and self.process.poll() is None:
            self.sync()
        self.driver_seen = installed

    def sync(self):
        if self.process and self.process.poll() is None:
            try:
                self.process.stdin.write(b"SYNC\n")
                self.process.stdin.flush()
            except (OSError, ValueError):
                pass

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=2)
        self.process = None
