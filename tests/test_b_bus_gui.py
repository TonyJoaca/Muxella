"""Run with python -m unittest discover -s tests -p test_b_bus_gui.py."""
import os
import sys
from pathlib import Path

# Ensure project root is in sys.path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import unittest
from types import SimpleNamespace
from PyQt6.QtWidgets import QApplication
from gui.main_window import MasterBusStrip, MuxellaMainWindow, VirtualInputStrip
from gui.pipe_client import MuxellaPipeClient


class Client:
    def __init__(self):
        self.outputs = []
        self.inputs = []
        self.commands = []
        self.succeed = True

    def attach_output(self, bus, name):
        self.outputs.append((bus, name))
        return self.succeed

    def attach_input(self, strip, name):
        self.inputs.append((strip, name))

    def send_command(self, command):
        self.commands.append(command)

    def set_bus_delay(self, bus_idx, delay_ms):
        self.commands.append(f"bus_delay {bus_idx} {delay_ms:.1f}")


class BusDeviceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.client = Client()
        self.devices = {
            "captures": [{"name": f"Muxella Out B{i}"} for i in (1, 2, 3)],
            "renders": [{"name": "Speakers"}, {"name": "Main VAIO"},
                        {"name": "Muxella Out B1"}, {"name": "Muxella Out B2"}, {"name": "Muxella Out B3"}],
            "attached_outputs": ["None"] * 8,
        }
        self.logs = []
        self.widgets = []

    def tearDown(self):
        for widget in self.widgets:
            widget.deleteLater()
        self.app.processEvents()

    def bus(self, index, virtual=True):
        widget = MasterBusStrip(index, f"B{index - 4}", "Select Device", virtual,
                                self.client, lambda: self.devices, self.logs.append)
        self.widgets.append(widget)
        return widget

    def test_all_b_buses_select_capture_devices_without_red_border(self):
        for index in (5, 6, 7):
            with self.subTest(bus=index):
                bus = self.bus(index)
                name = f"Muxella Out B{index - 4}"
                bus._on_device_selected(name)
                self.assertEqual(self.client.outputs[-1], (index, name))
                self.assertFalse(bus.is_offline)
                self.assertFalse(bus.dev_btn.is_offline)

    def test_failed_backend_attachment_is_reported(self):
        self.client.succeed = False
        bus = self.bus(5)
        bus._on_device_selected("Muxella Out B1")
        self.assertTrue(bus.is_offline)
        self.assertIn("device found but output attachment failed", self.logs[-1])

    def test_b_bus_reconnects_from_capture_list(self):
        bus = self.bus(5)
        bus.cached_device = "Muxella Out B1"
        bus.set_device_connected(False)
        window = SimpleNamespace(master_buses=[bus], client=self.client,
                                 log_message=self.logs.append)
        MuxellaMainWindow._check_and_reconnect_devices(window, self.devices)
        self.assertFalse(bus.is_offline)
        self.assertEqual(self.client.outputs, [(5, "Muxella Out B1")])
        self.devices["attached_outputs"][5] = "Muxella Out B1 Link"
        MuxellaMainWindow._check_and_reconnect_devices(window, self.devices)
        self.assertEqual(len(self.client.outputs), 1)
        self.devices["renders"] = []
        self.devices["captures"] = []
        MuxellaMainWindow._check_and_reconnect_devices(window, self.devices)
        self.assertTrue(bus.is_offline)

    def test_a_bus_still_selects_playback_devices(self):
        bus = self.bus(0, virtual=False)
        bus._on_device_selected("Speakers")
        self.assertEqual(self.client.outputs[-1], (0, "Speakers"))
        self.assertFalse(bus.is_offline)

    def test_virtual_input_handler_uses_playback_list(self):
        # Exercise the actual handler without starting an engine or creating
        # the full workstation. VirtualInputStrip has no is_virtual attribute.
        strip = SimpleNamespace(strip_id=5, title="Main", client=self.client,
                                get_devices_func=lambda: self.devices,
                                log_callback=self.logs.append,
                                set_device_connected=lambda value: self.logs.append(value))
        VirtualInputStrip._on_device_selected(strip, "Main VAIO")
        self.assertEqual(self.client.inputs, [(5, "Main VAIO")])

    def test_pipe_reply_controls_attachment_success(self):
        client = MuxellaPipeClient()
        for reply, expected in [(' {"status":"ok"}', True),
                                ('{"status":"error"}', False), (None, False),
                                ('invalid response', False)]:
            client.send_command = lambda command, value=reply: value
            self.assertEqual(client.attach_output(5, "Muxella Out B1"), expected)

    def test_app_loopback_asio_and_exclusive_selection(self):
        self.devices["apps"] = [{"pid": 4321, "id": "app:Discord.exe", "name": "Discord.exe"}]
        self.devices["asios"] = [{"id": "asio:Focusrite USB ASIO", "name": "Focusrite USB ASIO"}]
        strip = SimpleNamespace(strip_id=1, title="Mic 2", client=self.client,
                                get_devices_func=lambda: self.devices,
                                log_callback=self.logs.append,
                                set_device_connected=lambda value: self.logs.append(value))
        VirtualInputStrip._on_device_selected(strip, "[APP] Discord.exe")
        self.assertEqual(self.client.inputs[-1], (1, "app:Discord.exe"))

        bus = self.bus(0, virtual=False)
        self.devices["renders"] = [{"id": "dev-1", "name": "Speakers"}]
        bus.dev_btn.selected_device_id = "exclusive:dev-1"
        bus._on_device_selected("[EX] Speakers")
        self.assertEqual(self.client.outputs[-1], (0, "exclusive:id:dev-1"))

        bus.dev_btn.selected_device_id = "asio:Focusrite USB ASIO"
        bus._on_device_selected("[ASIO] Focusrite USB ASIO")
        self.assertEqual(self.client.outputs[-1], (0, "asio:Focusrite USB ASIO"))

    def test_bus_delay_and_lufs_ui(self):
        bus = self.bus(0, virtual=False)
        bus.delay_btn.set_delay_ms(45.0, emit=True)
        self.assertIn("bus_delay 0 45.0", self.client.commands)
        bus.set_lufs(-14.2)
        self.assertEqual(bus.lufs_lbl.text(), "-14.2 LUFS")
        bus.set_lufs(-70.0)
        self.assertEqual(bus.lufs_lbl.text(), "-inf LUFS")


if __name__ == "__main__":
    unittest.main()
