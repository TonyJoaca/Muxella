from gui.channel_enable import ChannelEnableMixin
from gui.virtual_audio import device_matches, resolve_selection
import os
import sys
import json
import subprocess
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Any, Optional

from PyQt6.QtCore import Qt, QTimer, QSize, QPoint, QRect, QEvent, QObject
from PyQt6.QtGui import QIcon, QPixmap, QColor, QFont, QKeySequence, QShortcut, QCursor
from PyQt6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QLabel, QPushButton, QSlider, QFrame, QGroupBox, QPlainTextEdit,
    QCheckBox, QScrollArea, QSplitter, QApplication, QMenu, QLineEdit,
    QFileDialog, QSystemTrayIcon
)

if sys.platform == "win32":
    import ctypes

class FramelessResizeFilter(QObject):
    """
    Application-wide event filter that provides native Windows directional resize cursors
    and hardware-accelerated edge resizing for frameless windows without overriding nativeEvent.
    """
    def __init__(self, window: QMainWindow):
        super().__init__()
        self.win = window
        self.margin = 8
        self._current_edge = None

    def _get_edge(self, pos: QPoint):
        if self.win.isMaximized():
            return None, None
        w, h = self.win.width(), self.win.height()
        lx, ly = pos.x(), pos.y()
        m = self.margin

        if lx < 0 or ly < 0 or lx > w or ly > h:
            return None, None

        left = lx < m
        right = lx >= w - m
        top = ly < m
        bottom = ly >= h - m

        # Win32 HT constants
        if top and left: return 13, Qt.CursorShape.SizeFDiagCursor      # HTTOPLEFT
        if top and right: return 14, Qt.CursorShape.SizeBDiagCursor     # HTTOPRIGHT
        if bottom and left: return 16, Qt.CursorShape.SizeBDiagCursor   # HTBOTTOMLEFT
        if bottom and right: return 17, Qt.CursorShape.SizeFDiagCursor  # HTBOTTOMRIGHT
        if left: return 10, Qt.CursorShape.SizeHorCursor                # HTLEFT
        if right: return 11, Qt.CursorShape.SizeHorCursor               # HTRIGHT
        if top: return 12, Qt.CursorShape.SizeVerCursor                 # HTTOP
        if bottom: return 15, Qt.CursorShape.SizeVerCursor              # HTBOTTOM
        return None, None

    def eventFilter(self, obj, event):
        etype = event.type()
        if etype in (QEvent.Type.MouseMove, QEvent.Type.HoverMove):
            global_pos = QCursor.pos()
            local = self.win.mapFromGlobal(global_pos)
            ht, cursor = self._get_edge(local)
            if cursor:
                if self._current_edge != ht:
                    if self._current_edge is not None:
                        QApplication.restoreOverrideCursor()
                    self._current_edge = ht
                    QApplication.setOverrideCursor(cursor)
            else:
                if self._current_edge is not None:
                    self._current_edge = None
                    QApplication.restoreOverrideCursor()
        elif etype == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
            if self._current_edge is not None:
                ht = self._current_edge
                self._current_edge = None
                QApplication.restoreOverrideCursor()
                if sys.platform == "win32":
                    ctypes.windll.user32.ReleaseCapture()
                    ctypes.windll.user32.SendMessageW(int(self.win.winId()), 0x00A1, ht, 0)
                return True
        elif etype in (QEvent.Type.MouseButtonRelease, QEvent.Type.Leave):
            if self._current_edge is not None:
                self._current_edge = None
                QApplication.restoreOverrideCursor()
        return False

def get_settings_path() -> Path:
    appdata = os.environ.get("APPDATA")
    if appdata:
        base_dir = Path(appdata) / "Muxella"
        old_dir = Path(appdata) / "MetVoice"
    else:
        base_dir = Path.home() / ".config" / "Muxella"
        old_dir = Path.home() / ".config" / "MetVoice"
    base_dir.mkdir(parents=True, exist_ok=True)
    target = base_dir / "settings.json"

    # Migrate any settings or presets from old MetVoice directory to Muxella
    if old_dir.exists():
        try:
            import shutil
            if not target.exists() and (old_dir / "settings.json").exists():
                shutil.copy2(old_dir / "settings.json", target)
            for item in old_dir.iterdir():
                dest_item = base_dir / item.name
                if not dest_item.exists() and item.is_file():
                    try:
                        shutil.copy2(item, dest_item)
                    except Exception:
                        pass
        except Exception:
            pass

    return target

def is_start_with_windows_enabled() -> bool:
    if sys.platform != "win32":
        return False
    try:
        import winreg
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
            0,
            winreg.KEY_READ
        )
        val = None
        for k in ["Muxella", "MetVoice"]:
            try:
                val, _ = winreg.QueryValueEx(key, k)
                if val: break
            except FileNotFoundError:
                pass
        winreg.CloseKey(key)
        return bool(val)
    except Exception:
        return False

def set_start_with_windows(enable: bool) -> bool:
    if sys.platform != "win32":
        return False
    try:
        import winreg
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
            0,
            winreg.KEY_SET_VALUE
        )
        if enable:
            if getattr(sys, "frozen", False):
                exe_path = Path(sys.executable).resolve()
                cmd = f'"{exe_path}" --minimized'
            else:
                root_dir = Path(__file__).resolve().parent.parent
                chosen_exe = root_dir / "Muxella.exe"
                if chosen_exe.exists():
                    cmd = f'"{chosen_exe.resolve()}" --minimized'
                else:
                    script = root_dir / "muxella_gui.py"
                    cmd = f'"{sys.executable}" "{script.resolve()}" --minimized'
            winreg.SetValueEx(key, "Muxella", 0, winreg.REG_SZ, cmd)
            # Remove legacy key if present
            try:
                winreg.DeleteValue(key, "MetVoice")
            except FileNotFoundError:
                pass
        else:
            for k in ["Muxella", "MetVoice"]:
                try:
                    winreg.DeleteValue(key, k)
                except FileNotFoundError:
                    pass
        winreg.CloseKey(key)
        return True
    except Exception as e:
        print(f"Error updating autostart: {e}")
        return False

from .widgets import BlockyVuMeter, BlockyKnob, CyberSlider, DbScaleWidget, CrtScanlineOverlay, CyberEqButton
from .eq_dialog import CyberEqualizerDialog
from .engine_dialog import AudioEngineConfigDialog
from .pipe_client import MetVoicePipeClient
from .theme import (
    ACCENT_PURPLE, ACCENT_GREEN, ACCENT_CYAN, ACCENT_MAGENTA, ACCENT_RED,
    TEXT_PRIMARY, TEXT_MUTED, TEXT_DIM, BG_BLACK, SURFACE_CARD, SURFACE_VIRTUAL,
    SURFACE_MASTER, BORDER_OUTLINE, BORDER_STRIP, BORDER_STRIP_HOVER, BORDER_SUBTLE,
    FONT_PIXEL, FONT_BODY, get_resource_dir
)


class EditableTitle(QLineEdit):
    """
    In-place editable title that looks like a technical cyber readout label,
    becoming an editable field upon click or focus.
    """
    def __init__(self, default_text: str, on_changed_callback=None, text_color: str = ACCENT_PURPLE, parent=None):
        super().__init__(default_text, parent)
        self.on_changed_callback = on_changed_callback
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setStyleSheet(f"""
            QLineEdit {{
                background: transparent;
                border: 1px solid transparent;
                color: {text_color};
                font-weight: bold;
                font-size: 8pt;
                padding: 1px 2px;
            }}
            QLineEdit:hover {{
                border: 1px dashed #581C87;
                background: #120424;
            }}
            QLineEdit:focus {{
                border: 1.5px solid {ACCENT_GREEN};
                background: #0d0217;
                color: {ACCENT_GREEN};
            }}
        """)
        self.returnPressed.connect(self._on_submit)
        self.editingFinished.connect(self._on_submit)

    def _on_submit(self):
        self.clearFocus()
        if self.on_changed_callback:
            self.on_changed_callback(self.text())


class CyberTitleBar(QWidget):
    """
    Sleek custom cyberpunk title bar with native Aero-snap dragging,
    double-click maximize, and styled neon minimize/maximize/close controls.
    """
    def __init__(self, title: str, parent: QMainWindow):
        super().__init__(parent)
        self.parent_window = parent
        self.setFixedHeight(32)
        self.setStyleSheet(f"""
            CyberTitleBar {{
                background-color: #0B0217;
                border-bottom: 1px solid #330B54;
            }}
        """)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 0, 0, 0)
        layout.setSpacing(8)

        # 1. Mini App Icon
        icon_path = get_resource_dir() / "icons" / "app.png"
        if not icon_path.exists():
            icon_path = get_resource_dir() / "icons" / "app.ico"
        icon_lbl = QLabel(self)
        if icon_path.exists():
            icon_lbl.setPixmap(QPixmap(str(icon_path)).scaled(18, 18, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation))
        layout.addWidget(icon_lbl)

        # 2. Stylized Title Label
        self.title_lbl = QLabel(title, self)
        self.title_lbl.setStyleSheet(f"""
            font-family: '{FONT_PIXEL}', Consolas, monospace;
            color: {ACCENT_PURPLE};
            font-size: 8pt;
            font-weight: bold;
            letter-spacing: 1px;
        """)
        layout.addWidget(self.title_lbl)

        # 3. Status Tag Dot
        tag = QLabel("● PRO MATRIX", self)
        tag.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 6.5pt; font-family: Consolas, monospace; font-weight: bold; letter-spacing: 0.5px;")
        layout.addWidget(tag)

        layout.addStretch()

        # 4. Styled Window Control Buttons
        btn_style = f"""
            QPushButton {{
                background: transparent;
                border: none;
                color: {TEXT_MUTED};
                font-family: 'Segoe UI', Consolas, sans-serif;
                font-size: 9pt;
                font-weight: bold;
                width: 44px;
                height: 32px;
            }}
            QPushButton:hover {{
                background-color: #24083B;
                color: {TEXT_PRIMARY};
            }}
            QPushButton:pressed {{
                background-color: #3B0D66;
            }}
        """

        self.min_btn = QPushButton("🗕", self)
        self.min_btn.setStyleSheet(btn_style)
        self.min_btn.setToolTip("Minimize")
        self.min_btn.clicked.connect(self.parent_window.showMinimized)
        layout.addWidget(self.min_btn)

        self.max_btn = QPushButton("🗖", self)
        self.max_btn.setStyleSheet(btn_style)
        self.max_btn.setToolTip("Maximize / Restore")
        self.max_btn.clicked.connect(self._toggle_maximize)
        layout.addWidget(self.max_btn)

        self.close_btn = QPushButton("✕", self)
        self.close_btn.setStyleSheet(f"""
            QPushButton {{
                background: transparent;
                border: none;
                color: {TEXT_MUTED};
                font-family: 'Segoe UI', Consolas, sans-serif;
                font-size: 9pt;
                font-weight: bold;
                width: 46px;
                height: 32px;
            }}
            QPushButton:hover {{
                background-color: #FF1A53;
                color: #FFFFFF;
            }}
            QPushButton:pressed {{
                background-color: #B3002D;
            }}
        """)
        self.close_btn.setToolTip("Close (Minimize to Tray)")
        self.close_btn.clicked.connect(self.parent_window.close)
        layout.addWidget(self.close_btn)

    def _toggle_maximize(self):
        if self.parent_window.isMaximized():
            self.parent_window.showNormal()
            self.max_btn.setText("🗖")
        else:
            self.parent_window.showMaximized()
            self.max_btn.setText("🗗")

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            if sys.platform == "win32":
                import ctypes
                ctypes.windll.user32.ReleaseCapture()
                ctypes.windll.user32.SendMessageW(int(self.parent_window.winId()), 0x00A1, 2, 0)
            else:
                window_handle = self.parent_window.windowHandle()
                if window_handle:
                    window_handle.startSystemMove()
        super().mousePressEvent(event)

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._toggle_maximize()
        super().mouseDoubleClickEvent(event)


class DeviceSelectorButton(QPushButton):
    """
    Clickable device dropdown button that mimics hardware selector menus
    without harsh label truncation, featuring hover tooltips.
    """
    def __init__(self, initial_label: str, is_render: bool, get_devices_func, on_selected_func, parent=None, is_virtual_bus: bool = False):
        self.selected_device_id = None
        self.selected_dev = initial_label
        self.cached_name = initial_label if initial_label not in ["Select Device", "None", ""] else ""
        self.is_offline = False
        short = initial_label.replace("[WDM] ", "").replace("[VAIO] ", "").strip()
        display = (short[:15] + "…") if len(short) > 16 else short
        super().__init__(f"{display} ▾", parent)
        self.is_render = is_render
        self.is_virtual_bus = is_virtual_bus
        self.get_devices_func = get_devices_func
        self.on_selected_func = on_selected_func
        self.setToolTip(initial_label)

        self.setFixedHeight(22)
        self._apply_normal_style()
        self.clicked.connect(self._open_menu)

    def _apply_normal_style(self):
        self.setStyleSheet(f"""
            QPushButton {{
                background-color: #0b0214;
                color: {ACCENT_PURPLE};
                border: 1px solid #3b1166;
                font-size: 7.5pt;
                padding: 1px 4px;
                text-align: center;
            }}
            QPushButton:hover {{
                background-color: #1a052e;
                color: {ACCENT_GREEN};
                border: 1px solid {ACCENT_GREEN};
            }}
            QPushButton:pressed {{
                background-color: #3b0d66;
            }}
        """)

    def _apply_offline_style(self):
        self.setStyleSheet("""
            QPushButton {
                background-color: #26050b;
                color: #FF6677;
                border: 1.5px solid #FF1744;
                font-size: 7.5pt;
                font-weight: bold;
                padding: 1px 4px;
                text-align: center;
            }
            QPushButton:hover {
                background-color: #3d0a14;
                color: #FFAAAA;
                border: 1.5px solid #FF5277;
            }
            QPushButton:pressed {
                background-color: #5c0f1e;
            }
        """)

    def set_offline_state(self, is_offline: bool, cached_name: Optional[str] = None):
        self.is_offline = is_offline
        if cached_name is not None and cached_name not in ["Select Device", "None", ""]:
            self.cached_name = cached_name
            self.selected_dev = cached_name

        if self.is_offline:
            self._apply_offline_style()
            name_to_show = self.cached_name or self.selected_dev or "Device"
            short = name_to_show.replace("[WDM] ", "").replace("[VAIO] ", "").strip()
            display = (short[:15] + "…") if len(short) > 16 else short
            self.setText(f"{display} ▾")
            self.setToolTip(f"{name_to_show}\n[DISCONNECTED - AWAITING CONNECTION]")
        else:
            self._apply_normal_style()
            cur = self.selected_dev or self.cached_name or "Select Device"
            short = cur.replace("[WDM] ", "").replace("[VAIO] ", "").strip()
            display = (short[:15] + "…") if len(short) > 16 else short
            self.setText(f"{display} ▾")
            self.setToolTip(cur)

    def _open_menu(self):
        menu = QMenu(self)
        menu.setStyleSheet(f"""
            QMenu {{
                background-color: #080110;
                border: 1.5px solid {ACCENT_PURPLE};
                color: {TEXT_PRIMARY};
                padding: 4px;
                font-size: 8pt;
            }}
            QMenu::item {{
                padding: 5px 14px;
                border-radius: 0px;
            }}
            QMenu::item:selected {{
                background-color: #24083b;
                color: {ACCENT_GREEN};
            }}
            QMenu::separator {{
                height: 1px;
                background: #3b1166;
                margin: 4px 0px;
            }}
        """)

        devices_data = self.get_devices_func()

        all_devices = (devices_data.get("captures", []) + devices_data.get("renders", [])) if devices_data else []
        def device_label(prefix, device):
            name = device.get("name", "Audio Device")
            duplicates = sum(item.get("name") == name for item in all_devices)
            suffix = ""
            if duplicates > 1 and device.get("id"):
                suffix = " [" + device["id"].rstrip("}")[-8:] + "]"
            return prefix + " " + name + suffix

        if self.is_virtual_bus:
            # B buses feed a cable's playback side; its recording side is used
            # as the microphone in Discord/OBS and other receiving programs.
            renders = devices_data.get("renders", []) if devices_data else []
            hdr = menu.addAction("--- CABLE PLAYBACK ENDPOINTS ---")
            hdr.setEnabled(False)
            for dev in renders:
                d_name = dev.get("name", "Audio Device")
                act = menu.addAction(device_label("[WDM]", dev))
                act.triggered.connect(lambda _, n=d_name, endpoint_id=dev.get("id"): self._handle_selection(n, endpoint_id))

        elif self.is_render:
            # A strips: Physical WASAPI playback hardware
            renders = devices_data.get("renders", []) if devices_data else []
            if not renders:
                renders = [
                    {"name": "Speakers (Realtek Audio)"},
                    {"name": "Headphones (Realtek Audio)"},
                    {"name": "Digital Audio (HDMI)"},
                    {"name": "USB Audio Device Out"},
                ]

            hw_renders = []
            virt_renders = []
            for dev in renders:
                d_name = dev.get("name", "Audio Device")
                name_lower = d_name.lower()
                if any(k in name_lower for k in ["muxella", "vaio", "link", "cable"]):
                    virt_renders.append(dev)
                else:
                    hw_renders.append(dev)

            hdr = menu.addAction("--- WASAPI PLAYBACK HARDWARE ---")
            hdr.setEnabled(False)
            for dev in (hw_renders if hw_renders else renders):
                d_name = dev.get("name", "Audio Device")
                act = menu.addAction(device_label("[WDM]", dev))
                act.triggered.connect(lambda _, n=d_name, endpoint_id=dev.get("id"): self._handle_selection(n, endpoint_id))

            if hw_renders and virt_renders:
                menu.addSeparator()
                hdr2 = menu.addAction("--- VIRTUAL PLAYBACK / AUX ---")
                hdr2.setEnabled(False)
                for dev in virt_renders:
                    d_name = dev.get("name", "Audio Device")
                    act = menu.addAction(device_label("[VAIO]", dev))
                    act.triggered.connect(lambda _, n=d_name, endpoint_id=dev.get("id"): self._handle_selection(n, endpoint_id))

        else:
            # Input Strips: Recording Hardware & Virtual Playback Loopback
            captures = devices_data.get("captures", []) if devices_data else []
            if not captures:
                captures = [
                    {"name": "Microphone Array (Realtek Audio)"},
                    {"name": "Headset Microphone (Realtek Audio)"},
                    {"name": "Stereo Mix (Realtek Audio)"},
                    {"name": "USB Audio Device Microphone"},
                ]

            hdr = menu.addAction("--- RECORDING / MIC HARDWARE ---")
            hdr.setEnabled(False)
            for dev in captures:
                d_name = dev.get("name", "Audio Device")
                act = menu.addAction(device_label("[WDM]", dev))
                act.triggered.connect(lambda _, n=d_name, endpoint_id=dev.get("id"): self._handle_selection(n, endpoint_id))

            render_devs = devices_data.get("renders", []) if devices_data else []
            if render_devs:
                menu.addSeparator()
                hdr2 = menu.addAction("--- VIRTUAL PLAYBACK / LOOPBACK ---")
                hdr2.setEnabled(False)
                for dev in render_devs:
                    d_name = dev.get("name", "Audio Device")
                    act = menu.addAction(device_label("[VAIO]", dev))
                    act.triggered.connect(lambda _, n=d_name, endpoint_id=dev.get("id"): self._handle_selection(n, endpoint_id))

        menu.addSeparator()
        disc_act = menu.addAction("[X] - Disconnect Device -")
        disc_act.triggered.connect(lambda: self._handle_selection("Select Device"))

        menu.exec(self.mapToGlobal(QPoint(0, self.height())))

    def _handle_selection(self, full_name: str, endpoint_id=None):
        self.selected_device_id = endpoint_id
        self.selected_dev = full_name
        if full_name not in ["Select Device", "None", ""]:
            self.cached_name = full_name
        else:
            self.cached_name = ""
        self.set_offline_state(False)
        self.on_selected_func(full_name)

    def set_selected_device(self, full_name: str, trigger_callback: bool = True, endpoint_id=None):
        self.selected_device_id = endpoint_id
        self.selected_dev = full_name
        if full_name not in ["Select Device", "None", ""]:
            self.cached_name = full_name
        else:
            self.cached_name = ""
        self.set_offline_state(False)
        if trigger_callback and self.on_selected_func:
            self.on_selected_func(full_name)


class PhysicalInputStrip(ChannelEnableMixin, QFrame):
    """
    Physical Input Strip (Strips 1..5) with clean ergonomics:
    - Standardized strip width (118px)
    - Enlarged FX Knobs with breathing room (COMP, GATE, DENOISE, REVERB, DELAY)
    - Grouped Routing Matrix (A1..A5 cluster / B1..B3 cluster)
    - Elongated Fader travel with double-click reset to 0.0 dB
    - Discrete dB Scale Markings (+12, 0, -10, -20, -40, -inf)
    - Decoupled Mute button (outlined neutral until engaged red glow)
    """
    def __init__(self, strip_id: int, default_title: str, default_dev: str, client: MetVoicePipeClient, get_devices_func, log_callback, parent=None):
        super().__init__(parent)
        self.strip_id = strip_id
        self.title = default_title
        self.client = client
        self.get_devices_func = get_devices_func
        self.log_callback = log_callback
        self.cached_device: Optional[str] = None
        self.is_offline: bool = False

        self.setFixedWidth(118)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet(f"""
            PhysicalInputStrip {{
                background-color: {SURFACE_CARD};
                border: 1px solid {BORDER_STRIP};
                border-radius: 2px;
            }}
            PhysicalInputStrip:hover {{
                border: 1px solid {BORDER_STRIP_HOVER};
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 5, 4, 5)
        layout.setSpacing(4)

        # 1. Strip Header & Title
        hdr_tag = QLabel(f"STRIP {strip_id + 1}")
        hdr_tag.setStyleSheet("color: #7E58A8; font-size: 6.5pt; font-weight: bold; letter-spacing: 0.5px;")
        hdr_tag.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(hdr_tag)

        self.title_edit = EditableTitle(default_title, self._on_title_edited, TEXT_PRIMARY, self)
        layout.addWidget(self.title_edit)

        # 2. Clickable Device Dropdown
        self.dev_btn = DeviceSelectorButton(default_dev, is_render=False,
                                            get_devices_func=self.get_devices_func,
                                            on_selected_func=self._on_device_selected,
                                            parent=self)
        layout.addWidget(self.dev_btn)
        self.init_channel_enable(layout, output=False)

        # 3. FX Knobs Section (Row 1: Comp, Gate, Denoise; Row 2: Reverb, Delay)
        fx_container = QVBoxLayout()
        fx_container.setSpacing(2)

        knobs_row1 = QHBoxLayout()
        knobs_row1.setSpacing(2)
        self.comp_knob = BlockyKnob("COMP", 0.0, 10.0, default_val=0.0, step=0.1, parent=self)
        self.comp_knob.valueChanged.connect(self._on_comp_changed)
        knobs_row1.addWidget(self.comp_knob)

        self.gate_knob = BlockyKnob("GATE", 0.0, 10.0, default_val=0.0, step=0.1, parent=self)
        self.gate_knob.valueChanged.connect(self._on_gate_changed)
        knobs_row1.addWidget(self.gate_knob)
        fx_container.addLayout(knobs_row1)

        knobs_row2 = QHBoxLayout()
        knobs_row2.setSpacing(2)
        self.denoise_knob = BlockyKnob("DENOISE", 0.0, 10.0, default_val=0.0, step=0.1, parent=self)
        self.denoise_knob.valueChanged.connect(self._on_denoise_changed)
        knobs_row2.addWidget(self.denoise_knob)

        self.rev_knob = BlockyKnob("REVERB", 0.0, 10.0, default_val=0.0, step=0.1, parent=self)
        self.rev_knob.valueChanged.connect(self._on_reverb_changed)
        knobs_row2.addWidget(self.rev_knob)
        fx_container.addLayout(knobs_row2)

        layout.addLayout(fx_container)

        # 4. Grouped Routing Matrix (Distinct Clusters for A1..A5 and B1..B3)
        route_lbl = QLabel("BUS ROUTING")
        route_lbl.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 6.5pt; font-weight: bold; margin-top: 2px;")
        route_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(route_lbl)

        # Row A: A1..A5
        row_a = QHBoxLayout()
        row_a.setSpacing(2)
        self.route_buttons: List[QPushButton] = []

        for b_idx in range(5):
            b_name = f"A{b_idx + 1}"
            btn = QPushButton(b_name)
            btn.setCheckable(True)
            btn.setFixedSize(19, 18)
            btn.setStyleSheet(f"""
                QPushButton {{
                    font-size: 7pt; padding: 0px;
                    border: 1px solid #240A38;
                    background-color: #0A0314;
                    color: {TEXT_DIM};
                }}
                QPushButton:hover {{
                    border: 1px solid {ACCENT_PURPLE};
                    color: {TEXT_PRIMARY};
                }}
                QPushButton:checked {{
                    border: 1.5px solid {ACCENT_GREEN};
                    background-color: #0B2915;
                    color: {ACCENT_GREEN};
                    font-weight: bold;
                }}
            """)
            btn.clicked.connect(lambda chk, b=b_idx: self._on_route_clicked(b, chk))
            row_a.addWidget(btn)
            self.route_buttons.append(btn)
        layout.addLayout(row_a)

        # Row B: B1..B3
        row_b = QHBoxLayout()
        row_b.setSpacing(3)
        for b_idx in range(5, 8):
            b_name = f"B{b_idx - 4}"
            btn = QPushButton(b_name)
            btn.setCheckable(True)
            btn.setFixedSize(24, 18)
            btn.setStyleSheet(f"""
                QPushButton {{
                    font-size: 7pt; padding: 0px;
                    border: 1px solid #240A38;
                    background-color: #0A0314;
                    color: {TEXT_DIM};
                }}
                QPushButton:hover {{
                    border: 1px solid {ACCENT_CYAN};
                    color: {TEXT_PRIMARY};
                }}
                QPushButton:checked {{
                    border: 1.5px solid {ACCENT_CYAN};
                    background-color: #082630;
                    color: {ACCENT_CYAN};
                    font-weight: bold;
                }}
            """)
            if strip_id == 0 and b_name in ["B1", "B2"]:
                btn.setChecked(True)
            elif strip_id > 0 and b_name == "B1":
                btn.setChecked(True)

            btn.clicked.connect(lambda chk, b=b_idx: self._on_route_clicked(b, chk))
            row_b.addWidget(btn)
            self.route_buttons.append(btn)
        layout.addLayout(row_b)

        # 5. Elongated Fader, dB Scale & Peak VU Meter
        fader_row = QHBoxLayout()
        fader_row.setSpacing(1)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        self.db_scale = DbScaleWidget(self)
        fader_row.addWidget(self.db_scale)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(1)

        self.fader = CyberSlider(Qt.Orientation.Vertical, default_value=0,
                                 title=f"STRIP {strip_id + 1} GAIN", min_display=-60.0, max_display=12.0,
                                 display_scale=0.1, unit="dB", parent=self)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.setToolTip("Double-click to reset (0.0 dB) | Right-click to type value")
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet(f"color: {ACCENT_PURPLE}; font-size: 7.5pt; font-weight: bold;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row, stretch=1)

        # 6. Quick Toggles: MONO / SOLO / MUTE
        toggles_layout = QVBoxLayout()
        toggles_layout.setSpacing(2)

        sub_row = QHBoxLayout()
        sub_row.setSpacing(2)

        self.mono_btn = QPushButton("MONO")
        self.mono_btn.setCheckable(True)
        self.mono_btn.setFixedHeight(18)
        self.mono_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.mono_btn.clicked.connect(self._on_mono_clicked)
        sub_row.addWidget(self.mono_btn)

        self.solo_btn = QPushButton("SOLO")
        self.solo_btn.setCheckable(True)
        self.solo_btn.setFixedHeight(18)
        self.solo_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.solo_btn.clicked.connect(self._on_solo_clicked)
        sub_row.addWidget(self.solo_btn)
        toggles_layout.addLayout(sub_row)

        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setFixedHeight(22)
        self._update_mute_style(False)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        toggles_layout.addWidget(self.mute_btn)

        layout.addLayout(toggles_layout)

    def _on_title_edited(self, new_text: str):
        self.title = new_text
        self.client.rename_strip(self.strip_id, new_text)
        self.log_callback(f"Strip {self.strip_id + 1} renamed to: {new_text}")

    def set_device_connected(self, connected: bool):
        if not self.processing_enabled:
            connected = True
        if not connected and self.cached_device:
            if not self.is_offline:
                self.is_offline = True
                self.setStyleSheet("""
                    PhysicalInputStrip {
                        background-color: #140307;
                        border: 1.5px solid #FF1744;
                        border-radius: 2px;
                    }
                    PhysicalInputStrip:hover {
                        border: 1.5px solid #FF5277;
                    }
                """)
                self.dev_btn.set_offline_state(True, self.cached_device)
        else:
            if self.is_offline or not connected:
                self.is_offline = False
                self.setStyleSheet(f"""
                    PhysicalInputStrip {{
                        background-color: {SURFACE_CARD};
                        border: 1px solid {BORDER_STRIP};
                        border-radius: 2px;
                    }}
                    PhysicalInputStrip:hover {{
                        border: 1px solid {BORDER_STRIP_HOVER};
                    }}
                """)
                self.dev_btn.set_offline_state(False)

    def _on_device_selected(self, dev_name: str):
        cleaned = dev_name.replace("[WDM] ", "").replace("[VAIO] ", "").strip()
        from gui.virtual_audio import migrate_device_name
        cleaned = migrate_device_name(cleaned)
        self.cached_device_id = self.dev_btn.selected_device_id
        if cleaned in ("Select Device", "None", "") or "Disconnect" in cleaned:
            self.cached_device = None
            self.cached_device_id = None
            self.client.send_command(f"detach_input {self.strip_id}")
            self.set_device_connected(True)
            return
        self.cached_device = cleaned
        if not self.processing_enabled:
            self.set_device_connected(True)
            return
        self._attach_retry_at = 0
        devs = self.get_devices_func() or {}
        device = resolve_selection(cleaned, self.cached_device_id, devs.get("captures", []), devs.get("renders", []))
        if device:
            self.cached_device_id = device.get("id")
            self.dev_btn.selected_device_id = self.cached_device_id
            query = "id:" + self.cached_device_id if self.cached_device_id else device["name"]
            attached = self.client.attach_input(self.strip_id, query)
            if attached is not None:
                self.set_device_connected(attached)
            self.log_callback(f"Device connection requested: {device['name']}")
        else:
            self.client.send_command(f"detach_input {self.strip_id}")
            self.set_device_connected(False)
            self.log_callback(f"Selected device unavailable or ambiguous: {cleaned}")

    def _on_route_clicked(self, bus_idx: int, enabled: bool):
        self.client.set_route(self.strip_id, bus_idx, enabled, 0.0)
        bus_name = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"][bus_idx]
        self.log_callback(f"Strip {self.strip_id + 1} -> Bus {bus_name}: {'ON' if enabled else 'OFF'}")

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_gain(self.strip_id, db)

    def _update_mute_style(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #FF1A53; color: #FFFFFF; border: 1.5px solid #FF3377; font-weight: bold; font-size: 7.5pt;")
        else:
            self.mute_btn.setStyleSheet(f"background-color: #0E0317; color: {TEXT_MUTED}; border: 1px solid #331047; font-size: 7.5pt;")

    def _on_mute_clicked(self, checked: bool):
        self._update_mute_style(checked)
        self.client.set_mute(self.strip_id, checked)
        self.log_callback(f"{self.title} Mute: {checked}")

    def _on_mono_clicked(self, checked: bool):
        if checked:
            self.mono_btn.setStyleSheet("background-color: #0B2915; color: #00FF66; border: 1.5px solid #00FF66; font-weight: bold; font-size: 7pt;")
        else:
            self.mono_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.client.set_strip_mono(self.strip_id, checked)
        self.log_callback(f"Strip {self.strip_id + 1} [{self.title}] Mono: {'ON' if checked else 'OFF'}")

    def _on_solo_clicked(self, checked: bool):
        if checked:
            self.solo_btn.setStyleSheet("background-color: #FFCC00; color: #000000; border: 1.5px solid #FFCC00; font-weight: bold;")
        else:
            self.solo_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.client.set_solo(self.strip_id, checked)
        self.log_callback(f"{self.title} Solo: {checked}")

    def _on_comp_changed(self, val: float):
        self.client.set_comp(self.strip_id, val)
        self.log_callback(f"{self.title} Comp: {val:.1f}")

    def _on_gate_changed(self, val: float):
        self.client.set_gate(self.strip_id, val)
        self.log_callback(f"{self.title} Gate: {val:.1f}")

    def _on_denoise_changed(self, val: float):
        self.client.set_denoise(self.strip_id, val)
        self.log_callback(f"{self.title} Denoise: {val:.1f}")

    def _on_reverb_changed(self, val: float):
        self.client.set_reverb(self.strip_id, val)
        self.log_callback(f"{self.title} Reverb: {val:.1f}")

    def get_state(self) -> Dict[str, Any]:
        return {
            "enabled": self.processing_enabled,
            "title": self.title_edit.text(),
            "device": self.cached_device or getattr(self.dev_btn, "selected_dev", ""),
            "device_id": getattr(self, "cached_device_id", None) or self.dev_btn.selected_device_id,
            "comp": float(self.comp_knob.value),
            "gate": float(self.gate_knob.value),
            "denoise": float(self.denoise_knob.value),
            "reverb": float(self.rev_knob.value),
            "gain_db": self.fader.value() / 10.0,
            "routes": [btn.isChecked() for btn in self.route_buttons],
            "mono": self.mono_btn.isChecked(),
            "solo": self.solo_btn.isChecked(),
            "mute": self.mute_btn.isChecked(),
        }

    def set_state(self, state: Dict[str, Any]):
        self.set_processing_enabled(state.get("enabled", True), reconnect=False)
        if "title" in state:
            t = str(state["title"])
            self.title = t
            self.title_edit.setText(t)
            self.client.rename_strip(self.strip_id, t)

        if "comp" in state:
            c = float(state["comp"])
            self.comp_knob.set_value(c)
            self._on_comp_changed(c)

        if "gate" in state:
            g = float(state["gate"])
            self.gate_knob.set_value(g)
            self._on_gate_changed(g)

        if "denoise" in state:
            d = float(state["denoise"])
            self.denoise_knob.set_value(d)
            self._on_denoise_changed(d)

        if "reverb" in state:
            r = float(state["reverb"])
            self.rev_knob.set_value(r)
            self._on_reverb_changed(r)

        if "gain_db" in state:
            gain = float(state["gain_db"])
            self.fader.setValue(int(round(gain * 10)))
            self._on_fader_changed(int(round(gain * 10)))

        if "routes" in state:
            routes = state["routes"]
            for b_idx, chk in enumerate(routes):
                if b_idx < len(self.route_buttons):
                    btn = self.route_buttons[b_idx]
                    btn.setChecked(bool(chk))
                    self._on_route_clicked(b_idx, bool(chk))

        if "mono" in state:
            m = bool(state["mono"])
            self.mono_btn.setChecked(m)
            self._on_mono_clicked(m)

        if "solo" in state:
            s = bool(state["solo"])
            self.solo_btn.setChecked(s)
            self._on_solo_clicked(s)

        if "mute" in state:
            mu = bool(state["mute"])
            self.mute_btn.setChecked(mu)
            self._on_mute_clicked(mu)

        if "device" in state and state["device"]:
            dev = str(state["device"])
            if dev not in ["Select Device", "None", ""]:
                self.cached_device = dev
            self.dev_btn.set_selected_device(dev, trigger_callback=True, endpoint_id=state.get("device_id"))



class VirtualInputStrip(ChannelEnableMixin, QFrame):
    """
    Virtual Input Strip (Strips 6..8 / VAIO 1..3):
    - Standardized strip width (118px)
    - 3-Band Parametric EQ Knobs (BASS, MID, TREBLE) with double-click reset
    - Pan Slider with double-click reset to Center
    - Grouped Routing Matrix (A1..A5 cluster / B1..B3 cluster)
    - Elongated Fader travel with dB markings & double-click reset
    - Outlined MUTE button (engages solid red neon glow only when active)
    """
    def __init__(self, strip_id: int, default_title: str, default_dev: str, client: MetVoicePipeClient, get_devices_func, log_callback, parent=None):
        super().__init__(parent)
        self.strip_id = strip_id
        self.title = default_title
        self.client = client
        self.get_devices_func = get_devices_func
        self.log_callback = log_callback
        self.cached_device = default_dev
        self.is_offline = False

        self.setFixedWidth(118)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet(f"""
            VirtualInputStrip {{
                background-color: {SURFACE_VIRTUAL};
                border: 1px solid #10331D;
                border-radius: 2px;
            }}
            VirtualInputStrip:hover {{
                border: 1px solid #00AA44;
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 5, 4, 5)
        layout.setSpacing(4)

        # 1. Strip Header & Title
        vaio_num = strip_id - 4
        hdr_tag = QLabel(f"VAIO {vaio_num}")
        hdr_tag.setStyleSheet("color: #00AA44; font-size: 6.5pt; font-weight: bold; letter-spacing: 0.5px;")
        hdr_tag.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(hdr_tag)

        self.title_edit = EditableTitle(default_title, self._on_title_edited, ACCENT_GREEN, self)
        layout.addWidget(self.title_edit)

        # 2. Clickable Device Dropdown
        self.dev_btn = DeviceSelectorButton(default_dev, is_render=False,
                                            get_devices_func=self.get_devices_func,
                                            on_selected_func=self._on_device_selected,
                                            parent=self)
        layout.addWidget(self.dev_btn)
        self.init_channel_enable(layout, output=False)

        # 3. 3-Band Parametric EQ Knobs (BASS, MID, TREBLE) with breathing room
        eq_box = QHBoxLayout()
        eq_box.setSpacing(2)

        self.bass_knob = BlockyKnob("BASS", -12.0, 12.0, default_val=0.0, step=0.5, unit="dB", parent=self)
        self.bass_knob.valueChanged.connect(self._on_eq_changed)
        eq_box.addWidget(self.bass_knob)

        self.mid_knob = BlockyKnob("MID", -12.0, 12.0, default_val=0.0, step=0.5, unit="dB", parent=self)
        self.mid_knob.valueChanged.connect(self._on_eq_changed)
        eq_box.addWidget(self.mid_knob)
        layout.addLayout(eq_box)

        eq_box2 = QHBoxLayout()
        eq_box2.setSpacing(2)
        self.treble_knob = BlockyKnob("TREBLE", -12.0, 12.0, default_val=0.0, step=0.5, unit="dB", parent=self)
        self.treble_knob.valueChanged.connect(self._on_eq_changed)
        eq_box2.addWidget(self.treble_knob)
        layout.addLayout(eq_box2)

        # Balance / Pan Slider with double-click reset to Center
        pan_row = QHBoxLayout()
        pan_lbl_l = QLabel("L")
        pan_lbl_l.setStyleSheet("color: #00AA44; font-size: 7pt;")
        self.pan_slider = CyberSlider(Qt.Orientation.Horizontal, default_value=0,
                                      title=f"VAIO {vaio_num} PAN", min_display=-100.0, max_display=100.0,
                                      display_scale=1.0, unit="%", parent=self)
        self.pan_slider.setRange(-100, 100)
        self.pan_slider.setValue(0)
        self.pan_slider.setToolTip("Double-click to reset (Center) | Right-click to type value")
        self.pan_slider.valueChanged.connect(lambda v: self.client.set_pan(self.strip_id, v / 100.0))
        pan_lbl_r = QLabel("R")
        pan_lbl_r.setStyleSheet("color: #00AA44; font-size: 7pt;")

        pan_row.addWidget(pan_lbl_l)
        pan_row.addWidget(self.pan_slider)
        pan_row.addWidget(pan_lbl_r)
        layout.addLayout(pan_row)

        # 4. Grouped Routing Matrix (A1..A5 cluster / B1..B3 cluster)
        route_lbl = QLabel("BUS ROUTING")
        route_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 6.5pt; font-weight: bold; margin-top: 2px;")
        route_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(route_lbl)

        # Row A: A1..A5
        row_a = QHBoxLayout()
        row_a.setSpacing(2)
        self.route_buttons: List[QPushButton] = []

        for b_idx in range(5):
            b_name = f"A{b_idx + 1}"
            btn = QPushButton(b_name)
            btn.setCheckable(True)
            btn.setFixedSize(19, 18)
            btn.setStyleSheet(f"""
                QPushButton {{
                    font-size: 7pt; padding: 0px;
                    border: 1px solid #144D28;
                    background-color: #041007;
                    color: #00AA44;
                }}
                QPushButton:hover {{
                    border: 1px solid {ACCENT_GREEN};
                    color: #FFFFFF;
                }}
                QPushButton:checked {{
                    border: 1.5px solid {ACCENT_GREEN};
                    background-color: #0B2915;
                    color: {ACCENT_GREEN};
                    font-weight: bold;
                }}
            """)
            if b_name == "A1":
                btn.setChecked(True)
            btn.clicked.connect(lambda chk, b=b_idx: self._on_route_clicked(b, chk))
            row_a.addWidget(btn)
            self.route_buttons.append(btn)
        layout.addLayout(row_a)

        # Row B: B1..B3
        row_b = QHBoxLayout()
        row_b.setSpacing(3)
        for b_idx in range(5, 8):
            b_name = f"B{b_idx - 4}"
            btn = QPushButton(b_name)
            btn.setCheckable(True)
            btn.setFixedSize(24, 18)
            btn.setStyleSheet(f"""
                QPushButton {{
                    font-size: 7pt; padding: 0px;
                    border: 1px solid #144D28;
                    background-color: #041007;
                    color: #00AA44;
                }}
                QPushButton:hover {{
                    border: 1px solid {ACCENT_CYAN};
                    color: #FFFFFF;
                }}
                QPushButton:checked {{
                    border: 1.5px solid {ACCENT_CYAN};
                    background-color: #082630;
                    color: {ACCENT_CYAN};
                    font-weight: bold;
                }}
            """)
            if strip_id == 6 and b_name == "B1":
                btn.setChecked(True)
            btn.clicked.connect(lambda chk, b=b_idx: self._on_route_clicked(b, chk))
            row_b.addWidget(btn)
            self.route_buttons.append(btn)
        layout.addLayout(row_b)

        # 5. Elongated Fader, dB Scale & Peak VU Meter
        fader_row = QHBoxLayout()
        fader_row.setSpacing(1)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        self.db_scale = DbScaleWidget(self)
        fader_row.addWidget(self.db_scale)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(1)

        self.fader = CyberSlider(Qt.Orientation.Vertical, default_value=0,
                                 title=f"VAIO {vaio_num} GAIN", min_display=-60.0, max_display=12.0,
                                 display_scale=0.1, unit="dB", parent=self)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.setToolTip("Double-click to reset (0.0 dB) | Right-click to type value")
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 7.5pt; font-weight: bold;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row, stretch=1)

        # 6. Quick Toggles: M.C / SOLO / MUTE
        toggles_layout = QVBoxLayout()
        toggles_layout.setSpacing(2)

        sub_row = QHBoxLayout()
        sub_row.setSpacing(2)

        self.mc_btn = QPushButton("M.C")
        self.mc_btn.setCheckable(True)
        self.mc_btn.setFixedHeight(18)
        self.mc_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {ACCENT_GREEN}; border: 1px solid #144D28;")
        self.mc_btn.clicked.connect(self._on_mc_clicked)
        sub_row.addWidget(self.mc_btn)

        self.solo_btn = QPushButton("SOLO")
        self.solo_btn.setCheckable(True)
        self.solo_btn.setFixedHeight(18)
        self.solo_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {ACCENT_GREEN}; border: 1px solid #144D28;")
        self.solo_btn.clicked.connect(self._on_solo_clicked)
        sub_row.addWidget(self.solo_btn)
        toggles_layout.addLayout(sub_row)

        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setFixedHeight(22)
        self._update_mute_style(False)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        toggles_layout.addWidget(self.mute_btn)

        layout.addLayout(toggles_layout)

    def _on_title_edited(self, new_text: str):
        self.title = new_text
        self.client.rename_strip(self.strip_id, new_text)
        self.log_callback(f"Virtual Strip {self.strip_id + 1} renamed to: {new_text}")

    def set_device_connected(self, connected: bool):
        if not self.processing_enabled:
            connected = True
        if not connected:
            if not self.is_offline:
                self.is_offline = True
                self.setStyleSheet("""
                    VirtualInputStrip {
                        background-color: #140307;
                        border: 1.5px solid #FF1744;
                        border-radius: 2px;
                    }
                    VirtualInputStrip:hover {
                        border: 1.5px solid #FF5277;
                    }
                """)
                self.dev_btn.set_offline_state(True, self.cached_device)
        else:
            if self.is_offline or not connected:
                self.is_offline = False
                self.setStyleSheet(f"""
                    VirtualInputStrip {{
                        background-color: {SURFACE_VIRTUAL};
                        border: 1px solid #10331D;
                        border-radius: 2px;
                    }}
                    VirtualInputStrip:hover {{
                        border: 1px solid #00AA44;
                    }}
                """)
                self.dev_btn.set_offline_state(False)

    def _on_device_selected(self, dev_name: str):
        cleaned = dev_name.replace("[WDM] ", "").replace("[VAIO] ", "").strip()
        from gui.virtual_audio import migrate_device_name
        cleaned = migrate_device_name(cleaned)
        self.cached_device_id = self.dev_btn.selected_device_id
        if cleaned in ("Select Device", "None", "") or "Disconnect" in cleaned:
            self.cached_device = None
            self.cached_device_id = None
            self.client.send_command(f"detach_input {self.strip_id}")
            self.set_device_connected(True)
            return
        self.cached_device = cleaned
        if not self.processing_enabled:
            self.set_device_connected(True)
            return
        self._attach_retry_at = 0
        devs = self.get_devices_func() or {}
        device = resolve_selection(cleaned, self.cached_device_id, devs.get("captures", []), devs.get("renders", []))
        if device:
            self.cached_device_id = device.get("id")
            self.dev_btn.selected_device_id = self.cached_device_id
            query = "id:" + self.cached_device_id if self.cached_device_id else device["name"]
            attached = self.client.attach_input(self.strip_id, query)
            if attached is not None:
                self.set_device_connected(attached)
            self.log_callback(f"Device connection requested: {device['name']}")
        else:
            self.client.send_command(f"detach_input {self.strip_id}")
            self.set_device_connected(False)
            self.log_callback(f"Selected device unavailable or ambiguous: {cleaned}")

    def _on_eq_changed(self):
        low = self.bass_knob.value
        mid = self.mid_knob.value
        high = self.treble_knob.value
        self.client.set_eq(self.strip_id, low, mid, high)

    def _on_route_clicked(self, bus_idx: int, enabled: bool):
        self.client.set_route(self.strip_id, bus_idx, enabled, 0.0)
        bus_name = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"][bus_idx]
        self.log_callback(f"{self.title} -> Bus {bus_name}: {'ON' if enabled else 'OFF'}")

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_gain(self.strip_id, db)

    def _update_mute_style(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #FF1A53; color: #FFFFFF; border: 1.5px solid #FF3377; font-weight: bold; font-size: 7.5pt;")
        else:
            self.mute_btn.setStyleSheet("background-color: #071A0F; color: #00AA44; border: 1px solid #144D28; font-size: 7.5pt;")

    def _on_mute_clicked(self, checked: bool):
        self._update_mute_style(checked)
        self.client.set_mute(self.strip_id, checked)
        self.log_callback(f"{self.title} Mute: {checked}")

    def _on_mc_clicked(self, checked: bool):
        if checked:
            self.mc_btn.setStyleSheet("background-color: #0B2915; color: #00FF66; border: 1.5px solid #00FF66; font-weight: bold; font-size: 7pt;")
        else:
            self.mc_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {ACCENT_GREEN}; border: 1px solid #144D28;")
        self.client.set_strip_mono(self.strip_id, checked)
        self.log_callback(f"Virtual Strip {self.strip_id + 1} [{self.title}] M.C (Mono Center): {'ON' if checked else 'OFF'}")

    def _on_solo_clicked(self, checked: bool):
        if checked:
            self.solo_btn.setStyleSheet("background-color: #FFCC00; color: #000000; border: 1.5px solid #FFCC00; font-weight: bold;")
        else:
            self.solo_btn.setStyleSheet(f"font-size: 7pt; padding: 1px; color: {ACCENT_GREEN}; border: 1px solid #144D28;")
        self.client.set_solo(self.strip_id, checked)
        self.log_callback(f"{self.title} Solo: {checked}")

    def get_state(self) -> Dict[str, Any]:
        return {
            "enabled": self.processing_enabled,
            "title": self.title_edit.text(),
            "device": getattr(self.dev_btn, "selected_dev", ""),
            "device_id": getattr(self, "cached_device_id", None) or self.dev_btn.selected_device_id,
            "bass": float(self.bass_knob.value),
            "mid": float(self.mid_knob.value),
            "treble": float(self.treble_knob.value),
            "pan": float(self.pan_slider.value()),
            "gain_db": self.fader.value() / 10.0,
            "routes": [btn.isChecked() for btn in self.route_buttons],
            "mc": self.mc_btn.isChecked(),
            "solo": self.solo_btn.isChecked(),
            "mute": self.mute_btn.isChecked(),
        }

    def set_state(self, state: Dict[str, Any]):
        self.set_processing_enabled(state.get("enabled", True), reconnect=False)
        if "title" in state:
            t = str(state["title"])
            self.title = t
            self.title_edit.setText(t)
            self.client.rename_strip(self.strip_id, t)

        if "bass" in state or "mid" in state or "treble" in state:
            b = float(state.get("bass", self.bass_knob.value))
            m = float(state.get("mid", self.mid_knob.value))
            tr = float(state.get("treble", self.treble_knob.value))
            self.bass_knob.set_value(b)
            self.mid_knob.set_value(m)
            self.treble_knob.set_value(tr)
            self._on_eq_changed()

        if "pan" in state:
            p = int(round(float(state["pan"])))
            self.pan_slider.setValue(p)
            self.client.set_pan(self.strip_id, p / 100.0)

        if "gain_db" in state:
            gain = float(state["gain_db"])
            self.fader.setValue(int(round(gain * 10)))
            self._on_fader_changed(int(round(gain * 10)))

        if "routes" in state:
            routes = state["routes"]
            for b_idx, chk in enumerate(routes):
                if b_idx < len(self.route_buttons):
                    btn = self.route_buttons[b_idx]
                    btn.setChecked(bool(chk))
                    self._on_route_clicked(b_idx, bool(chk))

        if "mc" in state:
            mc = bool(state["mc"])
            self.mc_btn.setChecked(mc)
            self._on_mc_clicked(mc)

        if "solo" in state:
            s = bool(state["solo"])
            self.solo_btn.setChecked(s)
            self._on_solo_clicked(s)

        if "mute" in state:
            mu = bool(state["mute"])
            self.mute_btn.setChecked(mu)
            self._on_mute_clicked(mu)

        if "device" in state and state["device"]:
            dev = str(state["device"])
            if dev not in ["Select Device", "None", ""]:
                self.cached_device = dev
            self.dev_btn.set_selected_device(dev, trigger_callback=True, endpoint_id=state.get("device_id"))



class MasterBusStrip(ChannelEnableMixin, QFrame):
    """
    Master Output Bus Strip (A1..A5 Physical, B1..B3 Virtual):
    - Balanced strip width (104px, expanded from cramped 88px)
    - Device dropdown with unclipped labels and hover tooltip
    - Elongated Fader travel with dB scale markings & double-click reset
    - Decoupled Mute button (starts engaged red for safety, transitions to neutral when active)
    """
    def __init__(self, bus_id: int, bus_code: str, default_name: str, is_virtual: bool,
                 client: MetVoicePipeClient, get_devices_func, log_callback, parent=None):
        super().__init__(parent)
        self.bus_id = bus_id
        self.bus_code = bus_code
        self.name = default_name
        self.is_virtual = is_virtual
        self.client = client
        self.get_devices_func = get_devices_func
        self.log_callback = log_callback
        self.cached_device: Optional[str] = default_name if is_virtual else None
        self.is_offline: bool = False

        self.eq_params = {
            "enabled": False,
            "low_gain": 0.0,
            "low_freq": 100.0,
            "mid_gain": 0.0,
            "mid_freq": 1000.0,
            "mid_q": 1.0,
            "high_gain": 0.0,
            "high_freq": 8000.0
        }

        self.setFixedWidth(104)
        self.setFrameShape(QFrame.Shape.NoFrame)
        border_col = "#144D28" if is_virtual else BORDER_STRIP
        hover_col = "#00FF66" if is_virtual else BORDER_STRIP_HOVER
        text_col = ACCENT_GREEN if is_virtual else ACCENT_PURPLE

        self.setStyleSheet(f"""
            MasterBusStrip {{
                background-color: {SURFACE_MASTER};
                border: 1px solid {border_col};
                border-radius: 2px;
            }}
            MasterBusStrip:hover {{
                border: 1px solid {hover_col};
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 5, 4, 5)
        layout.setSpacing(4)

        # 1. Bus Tag & Editable Name
        code_lbl = QLabel(f"[{bus_code}]")
        code_lbl.setStyleSheet(f"color: {text_col}; font-weight: bold; font-size: 8pt; letter-spacing: 0.5px;")
        code_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(code_lbl)

        self.title_edit = EditableTitle(default_name, self._on_title_edited, text_col, self)
        layout.addWidget(self.title_edit)

        # 2. Hardware / Virtual Device Dropdown
        self.dev_btn = DeviceSelectorButton(default_name,
                                            is_render=(not is_virtual),
                                            get_devices_func=self.get_devices_func,
                                            on_selected_func=self._on_device_selected,
                                            parent=self,
                                            is_virtual_bus=is_virtual)
        layout.addWidget(self.dev_btn)
        self.init_channel_enable(layout, output=True)

        # 3. Mode Toggles (MONO / EQ)
        mode_box = QHBoxLayout()
        mode_box.setSpacing(2)

        self.mono_btn = QPushButton("MONO")
        self.mono_btn.setCheckable(True)
        self.mono_btn.setFixedHeight(18)
        self.mono_btn.setStyleSheet(f"font-size: 6.5pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.mono_btn.clicked.connect(self._on_mono_clicked)
        mode_box.addWidget(self.mono_btn)

        self.eq_dialog: Optional[CyberEqualizerDialog] = None
        self.eq_btn = CyberEqButton("EQ", self)
        self.eq_btn.setCheckable(True)
        self.eq_btn.setFixedHeight(18)
        self.eq_btn.setStyleSheet(f"font-size: 6.5pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.eq_btn.clicked.connect(self._on_eq_clicked)
        self.eq_btn.rightClicked.connect(self._open_eq_window)
        mode_box.addWidget(self.eq_btn)
        layout.addLayout(mode_box)

        # 4. Elongated Fader, dB Scale & Peak VU Meter
        fader_row = QHBoxLayout()
        fader_row.setSpacing(1)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        self.db_scale = DbScaleWidget(self)
        fader_row.addWidget(self.db_scale)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(1)

        self.fader = CyberSlider(Qt.Orientation.Vertical, default_value=0,
                                 title=f"BUS [{self.bus_code}] GAIN", min_display=-60.0, max_display=12.0,
                                 display_scale=0.1, unit="dB", parent=self)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.setToolTip("Double-click to reset (0.0 dB) | Right-click to type value")
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet(f"color: {text_col}; font-size: 7.5pt; font-weight: bold;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row, stretch=1)

        # 5. Master MUTE (Starts engaged for safety, transitions to neutral when unmuted)
        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setChecked(True)
        self._update_mute_style(True)
        self.mute_btn.setFixedHeight(22)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        layout.addWidget(self.mute_btn)

        # Optional output limiting; a fresh bus is transparent.
        self.limiter_btn = QPushButton("LIMITER OFF")
        self.limiter_btn.setCheckable(True)
        self.limiter_btn.setStyleSheet(f"color: {text_col}; font-size: 6.5pt; border: 1px solid {border_col}; padding: 1px;")
        self.limiter_btn.clicked.connect(self._on_limiter_clicked)
        layout.addWidget(self.limiter_btn)

    def _on_limiter_clicked(self, enabled):
        self.limiter_btn.setText("LIMITER ON" if enabled else "LIMITER OFF")
        self.client.send_command(f"bus_limiter {self.bus_id} {int(enabled)}")

    def _on_title_edited(self, new_text: str):
        self.name = new_text
        self.client.rename_bus(self.bus_id, new_text)
        if self.eq_dialog is not None:
            self.eq_dialog.bus_name = new_text
            self.eq_dialog.name_lbl.setText(new_text)
            self.eq_dialog.setWindowTitle(f"MUXELLA // EQUALIZER - {self.bus_code} ({new_text})")
        self.log_callback(f"Master Bus {self.bus_code} renamed to: {new_text}")

    def set_device_connected(self, connected: bool):
        if not self.processing_enabled:
            connected = True
        if not connected and self.cached_device:
            if not self.is_offline:
                self.is_offline = True
                self.setStyleSheet("""
                    MasterBusStrip {
                        background-color: #140307;
                        border: 1.5px solid #FF1744;
                        border-radius: 2px;
                    }
                    MasterBusStrip:hover {
                        border: 1.5px solid #FF5277;
                    }
                """)
                self.dev_btn.set_offline_state(True, self.cached_device)
        else:
            if self.is_offline or not connected:
                self.is_offline = False
                border_col = "#144D28" if self.is_virtual else BORDER_STRIP
                hover_col = "#00FF66" if self.is_virtual else BORDER_STRIP_HOVER
                self.setStyleSheet(f"""
                    MasterBusStrip {{
                        background-color: {SURFACE_MASTER};
                        border: 1px solid {border_col};
                        border-radius: 2px;
                    }}
                    MasterBusStrip:hover {{
                        border: 1px solid {hover_col};
                    }}
                """)
                self.dev_btn.set_offline_state(False)

    def _on_device_selected(self, dev_name: str):
        cleaned = dev_name.replace("[WDM] ", "").replace("[VAIO] ", "").strip()
        from gui.virtual_audio import migrate_device_name
        cleaned = migrate_device_name(cleaned)
        self.cached_device_id = self.dev_btn.selected_device_id
        if cleaned in ("Select Device", "None", "") or "Disconnect" in cleaned:
            self.cached_device = None
            self.cached_device_id = None
            self.client.send_command(f"detach_output {self.bus_id}")
            self.set_device_connected(True)
            return
        self.cached_device = cleaned
        if not self.processing_enabled:
            self.set_device_connected(True)
            return
        self._attach_retry_at = 0
        devs = self.get_devices_func() or {}
        device = resolve_selection(cleaned, self.cached_device_id, devs.get("renders", []), [])
        if device:
            self.cached_device_id = device.get("id")
            self.dev_btn.selected_device_id = self.cached_device_id
            query = "id:" + self.cached_device_id if self.cached_device_id else device["name"]
            attached = self.client.attach_output(self.bus_id, query)
            if attached is not None:
                self.set_device_connected(attached)
            self.log_callback(f"Device connection requested: {device['name']}")
        else:
            self.client.send_command(f"detach_output {self.bus_id}")
            self.set_device_connected(False)
            self.log_callback(f"Selected device unavailable or ambiguous: {cleaned}")

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_bus_gain(self.bus_id, db)

    def _update_mute_style(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #FF1A53; color: #FFFFFF; border: 1.5px solid #FF3377; font-weight: bold; font-size: 7.5pt;")
        else:
            self.mute_btn.setStyleSheet(f"background-color: #0E0317; color: {TEXT_MUTED}; border: 1px solid #331047; font-size: 7.5pt;")

    def _on_mute_clicked(self, checked: bool):
        self._update_mute_style(checked)
        self.client.set_bus_mute(self.bus_id, checked)
        self.log_callback(f"Master Bus {self.bus_code} Mute: {checked}")

    def _on_mono_clicked(self, checked: bool):
        if checked:
            self.mono_btn.setStyleSheet("background-color: #0B2915; color: #00FF66; border: 1.5px solid #00FF66; font-weight: bold; font-size: 6.5pt;")
        else:
            self.mono_btn.setStyleSheet(f"font-size: 6.5pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")
        self.client.set_bus_mono(self.bus_id, checked)
        self.log_callback(f"Master Bus [{self.bus_code}] Mono: {'ON' if checked else 'OFF'}")

    def _update_eq_btn_style(self, checked: bool):
        if checked:
            self.eq_btn.setStyleSheet("background-color: #082630; color: #00E5FF; border: 1.5px solid #00E5FF; font-weight: bold; font-size: 6.5pt;")
        else:
            self.eq_btn.setStyleSheet(f"font-size: 6.5pt; padding: 1px; color: {TEXT_MUTED}; border: 1px solid #2B0C47;")

    def _open_eq_window(self):
        if self.eq_dialog is None:
            self.eq_dialog = CyberEqualizerDialog(
                self.bus_id, self.bus_code, self.name,
                self.client, on_state_changed=self._on_eq_dialog_state_changed, parent=self
            )
            self.eq_dialog.setWindowTitle(f"MUXELLA // EQUALIZER - {self.bus_code} ({self.name})")
            ep = self.eq_params
            self.eq_dialog.is_enabled = ep.get("enabled", self.eq_btn.isChecked())
            self.eq_dialog.low_gain = float(ep.get("low_gain", 0.0))
            self.eq_dialog.low_freq = float(ep.get("low_freq", 100.0))
            self.eq_dialog.mid_gain = float(ep.get("mid_gain", 0.0))
            self.eq_dialog.mid_freq = float(ep.get("mid_freq", 1000.0))
            self.eq_dialog.mid_q = float(ep.get("mid_q", 1.0))
            self.eq_dialog.high_gain = float(ep.get("high_gain", 0.0))
            self.eq_dialog.high_freq = float(ep.get("high_freq", 8000.0))
            self.eq_dialog.bypass_btn.setChecked(self.eq_dialog.is_enabled)
            self.eq_dialog._update_bypass_btn_style()
            self.eq_dialog.low_gain_slider.setValue(int(round(self.eq_dialog.low_gain * 10)))
            self.eq_dialog.mid_gain_slider.setValue(int(round(self.eq_dialog.mid_gain * 10)))
            self.eq_dialog.high_gain_slider.setValue(int(round(self.eq_dialog.high_gain * 10)))
            self.eq_dialog.low_freq_knob.set_value(self.eq_dialog.low_freq)
            self.eq_dialog.mid_freq_knob.set_value(self.eq_dialog.mid_freq)
            self.eq_dialog.high_freq_knob.set_value(self.eq_dialog.high_freq)
            self.eq_dialog.mid_q_knob.set_value(self.eq_dialog.mid_q)
            self.eq_dialog.canvas.set_parameters(
                self.eq_dialog.low_gain, self.eq_dialog.low_freq,
                self.eq_dialog.mid_gain, self.eq_dialog.mid_freq, self.eq_dialog.mid_q,
                self.eq_dialog.high_gain, self.eq_dialog.high_freq, self.eq_dialog.is_enabled
            )
        self.eq_dialog.show()
        self.eq_dialog.raise_()
        self.eq_dialog.activateWindow()

    def _on_eq_dialog_state_changed(self, is_enabled: bool):
        self.eq_params["enabled"] = is_enabled
        self.eq_btn.setChecked(is_enabled)
        self._update_eq_btn_style(is_enabled)
        self.log_callback(f"Master Bus [{self.bus_code}] EQ: {'ON' if is_enabled else 'OFF'}")

    def _on_eq_clicked(self, checked: bool):
        self.eq_params["enabled"] = checked
        self._update_eq_btn_style(checked)
        self.client.set_bus_eq(self.bus_id, checked)
        if self.eq_dialog is not None:
            self.eq_dialog.is_enabled = checked
            self.eq_dialog.bypass_btn.setChecked(checked)
            self.eq_dialog._update_bypass_btn_style()
            self.eq_dialog.canvas.set_parameters(
                self.eq_dialog.low_gain, self.eq_dialog.low_freq,
                self.eq_dialog.mid_gain, self.eq_dialog.mid_freq, self.eq_dialog.mid_q,
                self.eq_dialog.high_gain, self.eq_dialog.high_freq, checked
            )
        self.log_callback(f"Master Bus [{self.bus_code}] EQ: {'ON' if checked else 'OFF'}")

    def get_state(self) -> Dict[str, Any]:
        eq_state = dict(self.eq_params)
        if self.eq_dialog is not None:
            eq_state = {
                "enabled": self.eq_dialog.is_enabled,
                "low_gain": float(self.eq_dialog.low_gain),
                "low_freq": float(self.eq_dialog.low_freq),
                "mid_gain": float(self.eq_dialog.mid_gain),
                "mid_freq": float(self.eq_dialog.mid_freq),
                "mid_q": float(self.eq_dialog.mid_q),
                "high_gain": float(self.eq_dialog.high_gain),
                "high_freq": float(self.eq_dialog.high_freq),
            }
        else:
            eq_state["enabled"] = self.eq_btn.isChecked()

        return {
            "enabled": self.processing_enabled,
            "name": self.title_edit.text(),
            "bus_code": self.bus_code,
            "device": self.cached_device or getattr(self.dev_btn, "selected_dev", ""),
            "device_id": getattr(self, "cached_device_id", None) or self.dev_btn.selected_device_id,
            "mono": self.mono_btn.isChecked(),
            "gain_db": self.fader.value() / 10.0,
            "mute": self.mute_btn.isChecked(),
            "limiter": self.limiter_btn.isChecked(),
            "eq": eq_state
        }

    def set_state(self, state: Dict[str, Any]):
        self.set_processing_enabled(state.get("enabled", True), reconnect=False)
        limiter = bool(state.get("limiter", False))
        self.limiter_btn.setChecked(limiter)
        self._on_limiter_clicked(limiter)

        if "name" in state:
            n = str(state["name"])
            self.name = n
            self.title_edit.setText(n)
            self._on_title_edited(n)

        if "mono" in state:
            m = bool(state["mono"])
            self.mono_btn.setChecked(m)
            self._on_mono_clicked(m)

        if "gain_db" in state:
            gain = float(state["gain_db"])
            self.fader.setValue(int(round(gain * 10)))
            self._on_fader_changed(int(round(gain * 10)))

        if "mute" in state:
            mu = bool(state["mute"])
            self.mute_btn.setChecked(mu)
            self._on_mute_clicked(mu)

        if "eq" in state:
            eq_info = state["eq"]
            self.eq_params.update(eq_info)
            eq_en = bool(eq_info.get("enabled", False))
            self.eq_btn.setChecked(eq_en)
            self._update_eq_btn_style(eq_en)
            self.client.set_bus_eq(self.bus_id, eq_en)

            lg = float(eq_info.get("low_gain", self.eq_params["low_gain"]))
            mg = float(eq_info.get("mid_gain", self.eq_params["mid_gain"]))
            hg = float(eq_info.get("high_gain", self.eq_params["high_gain"]))
            lf = float(eq_info.get("low_freq", self.eq_params["low_freq"]))
            mf = float(eq_info.get("mid_freq", self.eq_params["mid_freq"]))
            hf = float(eq_info.get("high_freq", self.eq_params["high_freq"]))
            mq = float(eq_info.get("mid_q", self.eq_params["mid_q"]))

            self.client.set_bus_eq_params(self.bus_id, lg, mg, hg, lf, mf, hf, mq)

            if self.eq_dialog is not None:
                self.eq_dialog.is_enabled = eq_en
                self.eq_dialog.low_gain = lg
                self.eq_dialog.mid_gain = mg
                self.eq_dialog.high_gain = hg
                self.eq_dialog.low_freq = lf
                self.eq_dialog.mid_freq = mf
                self.eq_dialog.high_freq = hf
                self.eq_dialog.mid_q = mq
                self.eq_dialog._update_bypass_btn_style()
                self.eq_dialog.low_gain_slider.setValue(int(round(lg * 10)))
                self.eq_dialog.mid_gain_slider.setValue(int(round(mg * 10)))
                self.eq_dialog.high_gain_slider.setValue(int(round(hg * 10)))
                self.eq_dialog.low_freq_knob.set_value(lf)
                self.eq_dialog.mid_freq_knob.set_value(mf)
                self.eq_dialog.high_freq_knob.set_value(hf)
                self.eq_dialog.mid_q_knob.set_value(mq)
                self.eq_dialog.canvas.set_parameters(lg, lf, mg, mf, mq, hg, hf, eq_en)

        if "device" in state and state["device"]:
            dev = str(state["device"])
            if dev not in ["Select Device", "None", ""]:
                self.cached_device = dev
            self.dev_btn.set_selected_device(dev, trigger_callback=True, endpoint_id=state.get("device_id"))



class MuxellaMainWindow(QMainWindow):
    """
    Main Muxella Workstation Window:
    - Decluttered layout without redundant nested borders
    - Collapsible/Dockable Diagnostic Terminal drawer
    - Clean typography hierarchy
    """
    def __init__(self):
        super().__init__()
        self.setWindowTitle("MUXELLA")
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window)
        self.setMouseTracking(True)
        self.resize(1640, 840)
        self.setMinimumSize(960, 600)

        self.client = MetVoicePipeClient()
        self.engine_process = None
        self._cached_devices = None
        self._force_relaunch = False
        self._pill_is_online = None
        self.apps_panel = None
        self.recording_panel = None
        self.recording_preferences = {}
        self.performance_snapshot = {}
        from gui.performance import PerformanceSampler
        self.performance_sampler = PerformanceSampler()

        self.sample_rate = 48000
        self.buffer_frames = 480
        self._preload_engine_settings()

        from gui.virtual_audio import CableProcess
        self.cable_process = CableProcess()
        self.cable_process.start()
        from gui.async_client import AsyncPipeClient
        self.client = AsyncPipeClient(self.client)
        self._is_quitting = False
        self.client.devices_ready.connect(self._apply_device_cache)
        self.client.task_finished.connect(self._on_background_task)
        self.client.attachment_finished.connect(self._on_attachment_finished)
        self.client.reply_ready.connect(self._on_feature_reply)
        self._restart_pending = True
        self.client.submit_task("start-engine", lambda: self._ensure_engine_running(client=self.client.raw))
        self._init_ui()

        from gui.hotkeys import GlobalHotkeys
        self.hotkeys = GlobalHotkeys(self)
        self.health_panel = None

        # Telemetry Polling Timer (30 FPS)
        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll_telemetry)
        self.poll_timer.start(33)

        # Device Cache Timer (refresh every 3 seconds)
        self.dev_timer = QTimer(self)
        self.dev_timer.timeout.connect(self._refresh_device_cache)
        self.dev_timer.start(3000)
        self._refresh_device_cache()
        self.record_timer = QTimer(self)
        self.record_timer.timeout.connect(lambda: self.client.send_command("record_status"))
        self.record_timer.start(1000)
        self.perf_timer = QTimer(self)
        self.perf_timer.timeout.connect(lambda: self.client.submit_task("performance", self.performance_sampler.sample))
        self.perf_timer.start(2000)
        self._update_polling_rate()

        # Keyboard shortcut for saving settings: Ctrl+S
        self.save_shortcut = QShortcut(QKeySequence("Ctrl+S"), self)
        self.save_shortcut.activated.connect(lambda: self.save_settings())

        # System Tray Icon Support
        self._is_quitting = False
        self._notified_tray = False
        self._init_tray_icon()

        # Auto-load saved settings from recommended location if present
        QTimer.singleShot(150, self.load_settings)

        # Frameless edge resizing filter
        self.resize_filter = FramelessResizeFilter(self)
        QApplication.instance().installEventFilter(self.resize_filter)

    def _preload_engine_settings(self):
        try:
            path = get_settings_path()
            if path.exists():
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if "audio_engine" in data:
                    ae = data["audio_engine"]
                    if "sample_rate" in ae:
                        self.sample_rate = int(ae["sample_rate"])
                    if "buffer_frames" in ae:
                        self.buffer_frames = int(ae["buffer_frames"])
        except Exception:
            pass

    def _find_server_exe(self) -> Optional[Path]:
        if getattr(sys, 'frozen', False):
            meipass = getattr(sys, '_MEIPASS', None)
            if meipass:
                candidate = Path(meipass) / "muxella_server.exe"
                if candidate.exists():
                    return candidate
            candidate = Path(sys.executable).parent / "muxella_server.exe"
            if candidate.exists():
                return candidate
        root_dir = Path(__file__).resolve().parent.parent
        for rel in ["build/muxella_server.exe", "muxella_server.exe", "build/Release/muxella_server.exe"]:
            candidate = root_dir / rel
            if candidate.exists():
                return candidate
        return None

    def _ensure_engine_running(self, client=None):
        import time
        control = client or self.client
        if getattr(self.client, "_stopping", False):
            return False
        response = control.send_command("status", timeout=0.5)
        if response:
            try:
                compatible = json.loads(response).get("engine_version") == "cables-v9"
            except (ValueError, TypeError):
                compatible = False
            if not self._force_relaunch and compatible:
                return True
            # An older bundled engine may still own the shared control pipe.
            # Stop it cleanly before starting this GUI's matching engine.
            control.send_command("record_stop", timeout=20.0)
            control.send_command("shutdown")
            import time
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if getattr(self.client, "_stopping", False):
                    return False
                if not control.send_command("status", timeout=0.5):
                    break
                time.sleep(0.05)
            else:
                print("Previous audio engine has not shut down; replacement was not started.")
                return False

        self._force_relaunch = False
        if getattr(self.client, "_stopping", False):
            return False
        server_exe = self._find_server_exe()
        if server_exe and server_exe.exists():
            try:
                flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
                cmd = [
                    str(server_exe),
                    "--sample-rate", str(self.sample_rate),
                    "--buffer", str(self.buffer_frames)
                ]
                self.engine_process = subprocess.Popen(
                    cmd,
                    creationflags=flags
                )
                import time
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if getattr(self.client, "_stopping", False):
                        return False
                    time.sleep(0.1)
                    if control.send_command("status", timeout=0.5):
                        return True
            except Exception as e:
                print(f"Failed to start audio engine: {e}")
        return False

    def _refresh_device_cache(self):
        self.client.submit_task("refresh-cables", self.cable_process.refresh)
        self.client.get_devices()

    def _apply_device_cache(self, devs):
        if getattr(self, "_is_quitting", False):
            return
        self._cached_devices = devs
        self._sync_device_buttons(devs)
        self._check_and_reconnect_devices(devs)

    def _on_attachment_finished(self, result):
        if self._is_quitting:
            return
        strips = self.physical_strips + self.virtual_strips if result["kind"] == "input" else self.master_buses
        index = result["index"]
        if index >= len(strips):
            return
        strip = strips[index]
        if not strip.processing_enabled:
            return
        endpoint_id = getattr(strip, "cached_device_id", None)
        if endpoint_id and result["query"] == "id:" + endpoint_id:
            strip.set_device_connected(result["ok"])
            import time
            strip._attach_retry_at = 0 if result["ok"] else time.monotonic() + 3
            if not result["ok"]:
                self.log_message(f"Device attachment failed: {strip.cached_device}")

    def _sync_device_buttons(self, devs: Dict[str, Any]):
        groups = [(getattr(self, "physical_strips", []) + getattr(self, "virtual_strips", []),
                   devs.get("attached_inputs", []), devs.get("attached_input_ids", [])),
                  (getattr(self, "master_buses", []), devs.get("attached_outputs", []),
                   devs.get("attached_output_ids", []))]
        for strips, names, ids in groups:
            for index, strip in enumerate(strips):
                if not strip.processing_enabled:
                    continue
                if index >= len(names) or names[index] in ("", "None"):
                    continue
                selected_id = getattr(strip, "cached_device_id", None)
                if selected_id and (index >= len(ids) or selected_id != ids[index]):
                    continue
                if strip.is_offline and strip.cached_device:
                    continue
                name = names[index]
                display = name[:15] + "…" if len(name) > 16 else name
                strip.dev_btn.setText(f"{display} ▾")
                strip.dev_btn.setToolTip(name)

    def _check_and_reconnect_devices(self, devs: Dict[str, Any]):
        captures, renders = devs.get("captures", []), devs.get("renders", [])
        groups = [(getattr(self, "physical_strips", []), "input", captures, renders),
                  (getattr(self, "virtual_strips", []), "input", captures, renders),
                  (getattr(self, "master_buses", []), "output", renders, [])]
        for strips, kind, preferred, secondary in groups:
            names = devs.get("attached_inputs" if kind == "input" else "attached_outputs", [])
            ids = devs.get("attached_input_ids" if kind == "input" else "attached_output_ids", [])
            for strip in strips:
                if not strip.processing_enabled:
                    continue
                selected = getattr(strip, "cached_device", None)
                if not selected or selected in ("Select Device", "None"):
                    continue
                index = strip.strip_id if kind == "input" else strip.bus_id
                device = resolve_selection(selected, getattr(strip, "cached_device_id", None), preferred, secondary)
                attached_id = ids[index] if index < len(ids) else ""
                if device:
                    endpoint_id = device.get("id")
                    strip.cached_device_id = endpoint_id
                    strip.dev_btn.selected_device_id = endpoint_id
                    attached = bool(endpoint_id and attached_id == endpoint_id)
                    if attached:
                        strip.set_device_connected(True)
                    else:
                        import time
                        if time.monotonic() < getattr(strip, "_attach_retry_at", 0):
                            continue
                        query = "id:" + endpoint_id if endpoint_id else device["name"]
                        attach = self.client.attach_input if kind == "input" else self.client.attach_output
                        attach(index, query)
                else:
                    if attached_id or (index < len(names) and names[index] != "None"):
                        self.client.send_command(f"detach_{kind} {index}")
                    strip.set_device_connected(False)

    def _get_devices(self) -> Optional[Dict[str, Any]]:
        return self._cached_devices

    def _init_ui(self):
        central = QWidget(self)
        central.setObjectName("CentralWidget")
        central.setStyleSheet(f"""
            #CentralWidget {{
                background-color: {BG_BLACK};
                border: 1px solid {BORDER_OUTLINE};
            }}
        """)
        self.setCentralWidget(central)

        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(1, 1, 1, 4)
        main_layout.setSpacing(0)

        # 0. Custom Stylized Cyber Title Bar
        self.title_bar = CyberTitleBar("MUXELLA", self)
        main_layout.addWidget(self.title_bar)

        # Main content container
        content_box = QWidget(central)
        content_box.setObjectName("ContentBox")
        content_layout = QVBoxLayout(content_box)
        content_layout.setContentsMargins(10, 6, 10, 4)
        content_layout.setSpacing(6)
        main_layout.addWidget(content_box, stretch=1)

        # 1. Top bar and title
        top_bar = QHBoxLayout()
        top_bar.setSpacing(12)

        hdr_vbox = QVBoxLayout()
        hdr_vbox.setSpacing(1)
        title_lbl = QLabel("MUXELLA")
        title_lbl.setStyleSheet(f"font-family: '{FONT_PIXEL}', sans-serif; color: {ACCENT_PURPLE}; font-size: 15pt; font-weight: bold; letter-spacing: 2px;")
        sub_lbl = QLabel("8x8 REAL-TIME MATRIX ROUTING // MULTI-VAIO AUDIO PIPELINE // MMCSS PRO AUDIO")
        sub_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 7.5pt; letter-spacing: 0.5px;")
        hdr_vbox.addWidget(title_lbl)
        hdr_vbox.addWidget(sub_lbl)
        top_bar.addLayout(hdr_vbox)

        top_bar.addStretch()

        self.status_pill = QPushButton(f"● PRO AUDIO MMCSS ACTIVE ({self.sample_rate} Hz | {self.buffer_frames} FRAMES)")
        self.status_pill.setFixedHeight(24)
        self.status_pill.setCursor(Qt.CursorShape.PointingHandCursor)
        self.status_pill.setToolTip("Click to configure Sample Rate (kHz) & Buffer Size")
        self.status_pill.setStyleSheet(f"""
            QPushButton {{
                background-color: #0b2413;
                color: {ACCENT_GREEN};
                border: 1px solid {ACCENT_GREEN};
                padding: 2px 12px;
                font-weight: bold;
                font-size: 7.5pt;
                font-family: Consolas, monospace;
                letter-spacing: 0.5px;
            }}
            QPushButton:hover {{
                background-color: #124d26;
                color: #FFFFFF;
                border: 1px solid #00FF88;
            }}
            QPushButton:pressed {{
                background-color: #00FF88;
                color: #000000;
            }}
        """)
        self.status_pill.clicked.connect(self._open_engine_config_dialog)
        top_bar.addWidget(self.status_pill)

        self.preset_btn = QPushButton("PRESETS ▾")
        self.preset_btn.setFixedHeight(24)
        self.preset_btn.setStyleSheet(f"""
            QPushButton {{
                background-color: #120421;
                color: {ACCENT_CYAN};
                border: 1px solid {ACCENT_CYAN};
                font-family: Consolas, monospace;
                font-size: 7.5pt;
                font-weight: bold;
                padding: 2px 10px;
                letter-spacing: 0.5px;
            }}
            QPushButton:hover {{
                background-color: #072a33;
                color: #FFFFFF;
                border: 1px solid #00FFFF;
            }}
            QPushButton:pressed {{
                background-color: #00E5FF;
                color: #000000;
            }}
        """)
        self.preset_btn.clicked.connect(self._open_presets_menu)
        top_bar.addWidget(self.preset_btn)
        self.apps_button = QPushButton("APPS")
        self.apps_button.setFixedHeight(24)
        self.apps_button.setToolTip("Adjust Windows app volumes")
        self.apps_button.clicked.connect(self._open_apps_panel)
        top_bar.addWidget(self.apps_button)
        self.record_button = QPushButton("RECORD")
        self.record_button.setFixedHeight(24)
        self.record_button.setToolTip("Record selected strips and buses to WAV")
        self.record_button.clicked.connect(self._open_recording_panel)
        top_bar.addWidget(self.record_button)

        save_btn = QPushButton("SAVE")
        save_btn.setToolTip("Save Current Configuration (Ctrl+S) -> %APPDATA%\\Muxella\\settings.json")
        save_btn.setFixedHeight(24)
        save_btn.setStyleSheet(f"""
            QPushButton {{
                background-color: #0b2413;
                color: {ACCENT_GREEN};
                border: 1px solid {ACCENT_GREEN};
                font-family: Consolas, monospace;
                font-size: 7.5pt;
                font-weight: bold;
                padding: 2px 10px;
            }}
            QPushButton:hover {{
                background-color: #124d26;
                color: #FFFFFF;
            }}
        """)
        save_btn.clicked.connect(lambda: self.save_settings())
        top_bar.addWidget(save_btn)

        self.autostart_btn = QPushButton()
        self.autostart_btn.setFixedHeight(24)
        self.autostart_btn.clicked.connect(self._toggle_autostart_clicked)
        self._sync_autostart_ui()
        top_bar.addWidget(self.autostart_btn)

        content_layout.addLayout(top_bar)

        # 2. Main Mixing Desk Horizontal Scroll Area
        scroll_area = QScrollArea(self)
        scroll_area.setWidgetResizable(True)
        scroll_area.setStyleSheet(f"QScrollArea {{ border: none; background-color: {BG_BLACK}; }}")

        desk_widget = QWidget()
        desk_widget.setStyleSheet(f"background-color: {BG_BLACK};")
        desk_layout = QHBoxLayout(desk_widget)
        desk_layout.setContentsMargins(2, 4, 2, 4)
        desk_layout.setSpacing(8)

        # A. 5 PHYSICAL INPUT STRIPS
        phys_box = QVBoxLayout()
        phys_box.setSpacing(4)
        phys_hdr = QLabel("PHYSICAL INPUTS (1..5)")
        phys_hdr.setFixedHeight(18)
        phys_hdr.setAlignment(Qt.AlignmentFlag.AlignCenter)
        phys_hdr.setStyleSheet(f"""
            QLabel {{
                color: {ACCENT_PURPLE};
                font-size: 7pt;
                font-weight: bold;
                letter-spacing: 1px;
            }}
        """)
        phys_box.addWidget(phys_hdr, stretch=0)

        phys_strips_row = QHBoxLayout()
        phys_strips_row.setSpacing(6)
        self.physical_strips: List[PhysicalInputStrip] = []

        phys_configs = [
            ("Main Mic", "Mic (Realtek)"),
            ("Laptop Mic", "Headset (Aux)"),
            ("Hardware In 3", "Line In 3/4"),
            ("Hardware In 4", "Digital In"),
            ("Hardware In 5", "Instrument In"),
        ]

        for s_idx, (t, dev) in enumerate(phys_configs):
            strip = PhysicalInputStrip(s_idx, t, dev, self.client, self._get_devices, self.log_message, self)
            self.physical_strips.append(strip)
            phys_strips_row.addWidget(strip)

        phys_box.addLayout(phys_strips_row, stretch=1)
        desk_layout.addLayout(phys_box)

        # Soft vertical divider line
        sep1 = QFrame()
        sep1.setFrameShape(QFrame.Shape.VLine)
        sep1.setStyleSheet(f"color: {BORDER_SUBTLE}; background-color: {BORDER_SUBTLE}; width: 1px;")
        desk_layout.addWidget(sep1)

        # B. 3 VIRTUAL INPUT STRIPS (VAIO 1..3)
        virt_box = QVBoxLayout()
        virt_box.setSpacing(4)
        virt_hdr = QLabel("VIRTUAL INPUTS (VAIO 1..3)")
        virt_hdr.setFixedHeight(18)
        virt_hdr.setAlignment(Qt.AlignmentFlag.AlignCenter)
        virt_hdr.setStyleSheet(f"""
            QLabel {{
                color: {ACCENT_GREEN};
                font-size: 7pt;
                font-weight: bold;
                letter-spacing: 1px;
            }}
        """)
        virt_box.addWidget(virt_hdr, stretch=0)

        virt_strips_row = QHBoxLayout()
        virt_strips_row.setSpacing(6)
        self.virtual_strips: List[VirtualInputStrip] = []

        virt_configs = [
            (5, "Main VAIO", "Muxella Main"),
            (6, "Spotify VAIO", "Muxella Spotify"),
            (7, "Discord VAIO", "Muxella Discord"),
        ]

        for s_idx, t, dev in virt_configs:
            strip = VirtualInputStrip(s_idx, t, dev, self.client, self._get_devices, self.log_message, self)
            self.virtual_strips.append(strip)
            virt_strips_row.addWidget(strip)

        virt_box.addLayout(virt_strips_row, stretch=1)
        desk_layout.addLayout(virt_box)

        # Soft vertical divider line
        sep2 = QFrame()
        sep2.setFrameShape(QFrame.Shape.VLine)
        sep2.setStyleSheet(f"color: {BORDER_SUBTLE}; background-color: {BORDER_SUBTLE}; width: 1px;")
        desk_layout.addWidget(sep2)

        # C. 8 MASTER OUTPUT BUSES (A1..A5 Physical + B1..B3 Virtual)
        master_box = QVBoxLayout()
        master_box.setSpacing(4)
        master_hdr = QLabel("MASTER BUSES (A1..A5 / B1..B3)")
        master_hdr.setFixedHeight(18)
        master_hdr.setAlignment(Qt.AlignmentFlag.AlignCenter)
        master_hdr.setStyleSheet(f"""
            QLabel {{
                color: {TEXT_MUTED};
                font-size: 7pt;
                font-weight: bold;
                letter-spacing: 1px;
            }}
        """)
        master_box.addWidget(master_hdr, stretch=0)

        master_strips_row = QHBoxLayout()
        master_strips_row.setSpacing(6)
        self.master_buses: List[MasterBusStrip] = []

        bus_configs = [
            (0, "A1", "Speakers", False),
            (1, "A2", "Headphones", False),
            (2, "A3", "Monitors", False),
            (3, "A4", "Aux Out", False),
            (4, "A5", "Line Out", False),
            (5, "B1", "Muxella Out B1", True),
            (6, "B2", "Muxella Out B2", True),
            (7, "B3", "Muxella Out B3", True),
        ]

        for b_idx, code, name, is_v in bus_configs:
            bus_strip = MasterBusStrip(b_idx, code, name, is_v, self.client, self._get_devices, self.log_message, self)
            self.master_buses.append(bus_strip)
            master_strips_row.addWidget(bus_strip)

        master_box.addLayout(master_strips_row, stretch=1)
        desk_layout.addLayout(master_box)

        scroll_area.setWidget(desk_widget)
        content_layout.addWidget(scroll_area, stretch=6)

        # 3. Collapsible Diagnostic Terminal Drawer
        self.term_drawer = QWidget()
        term_box = QVBoxLayout(self.term_drawer)
        term_box.setContentsMargins(0, 2, 0, 0)
        term_box.setSpacing(3)

        term_hdr = QLabel("DIAGNOSTIC TELEMETRY & ROUTING LOGS")
        term_hdr.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 7.5pt; font-weight: bold;")
        term_box.addWidget(term_hdr)

        self.terminal = QPlainTextEdit(self)
        self.terminal.setMaximumBlockCount(2000)
        self.terminal.setUndoRedoEnabled(False)
        self.terminal.setReadOnly(True)
        self.terminal.setFixedHeight(100)
        term_box.addWidget(self.terminal)

        # Terminal starts collapsed by default to give maximum vertical space to faders!
        self.term_drawer.setVisible(False)
        content_layout.addWidget(self.term_drawer, stretch=0)

        self.log_message("MUXELLA initialized.")
        self.log_message("Device selector dropdowns active. In-place editable titles active.")

        # 4. Bottom Toolbar & Status
        bottom_bar = QFrame(self)
        bottom_bar.setFixedHeight(34)
        bottom_bar.setStyleSheet(f"""
            QFrame {{
                background-color: #06020A;
                border-top: 1px solid {BORDER_OUTLINE};
            }}
        """)
        bot_layout = QHBoxLayout(bottom_bar)
        bot_layout.setContentsMargins(10, 0, 10, 0)
        bot_layout.setSpacing(10)

        bot_left = QLabel("TONY v1.0.dev")
        bot_left.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold; font-size: 8.5pt;")
        bot_layout.addWidget(bot_left)
        self.performance_label = QLabel("CPU — | RAM —")
        self.performance_label.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 8pt;")
        bot_layout.addWidget(self.performance_label)

        bot_layout.addStretch()

        # Collapsible Drawer Toggle Button
        self.toggle_log_btn = QPushButton("TERMINAL ▲")
        self.toggle_log_btn.setFixedHeight(22)
        self.toggle_log_btn.setToolTip("Toggle Diagnostic Terminal Drawer")
        self.toggle_log_btn.clicked.connect(self._toggle_terminal)
        bot_layout.addWidget(self.toggle_log_btn)

        reset_btn = QPushButton("RESET METERS")
        reset_btn.setFixedHeight(22)
        reset_btn.clicked.connect(self._reset_meters)
        bot_layout.addWidget(reset_btn)

        clear_btn = QPushButton("CLEAR LOGS")
        clear_btn.setFixedHeight(22)
        clear_btn.clicked.connect(self.terminal.clear)
        bot_layout.addWidget(clear_btn)

        quit_btn = QPushButton("QUIT")
        quit_btn.setFixedHeight(22)
        quit_btn.clicked.connect(self.close)
        bot_layout.addWidget(quit_btn)

        content_layout.addWidget(bottom_bar)

        # CRT Scanlines overlay
        self.scanlines = CrtScanlineOverlay(self)
        self.scanlines.setGeometry(self.rect())
        self.scanlines.lower()

    def _toggle_terminal(self):
        is_vis = self.term_drawer.isVisible()
        self.term_drawer.setVisible(not is_vis)
        self.toggle_log_btn.setText("TERMINAL ▼" if not is_vis else "TERMINAL ▲")

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, "scanlines"):
            self.scanlines.setGeometry(self.rect())

    def log_message(self, text: str):
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S,%f")[:-3]
        self.terminal.appendPlainText(f"{now_str} - muxella.ui - {text}")
        self.terminal.verticalScrollBar().setValue(self.terminal.verticalScrollBar().maximum())

    def _poll_telemetry(self):
        data = self.client.get_meters()
        if not data:
            self._telemetry_misses = getattr(self, "_telemetry_misses", 0) + 1
            if self._telemetry_misses >= 12:  # ~400ms of sustained pipe silence
                self._set_pill_offline()
            return

        self._telemetry_misses = 0
        self._set_pill_online()
        if not self.isVisible() or self.isMinimized():
            return  # Background health polling does not update hidden meters.

        strips_data = data.get("strips", [])
        for i in range(min(5, len(strips_data))):
            s_info = strips_data[i]
            pl = s_info.get("peak_l", -60.0)
            pr = s_info.get("peak_r", -60.0)
            self.physical_strips[i].meter.set_levels(pl, pr)

        for idx, s_idx in enumerate([5, 6, 7]):
            if s_idx < len(strips_data):
                s_info = strips_data[s_idx]
                pl = s_info.get("peak_l", -60.0)
                pr = s_info.get("peak_r", -60.0)
                self.virtual_strips[idx].meter.set_levels(pl, pr)

        buses_data = data.get("buses", [])
        for j in range(min(len(self.master_buses), len(buses_data))):
            b_info = buses_data[j]
            pl = b_info.get("peak_l", -60.0)
            pr = b_info.get("peak_r", -60.0)
            self.master_buses[j].meter.set_levels(pl, pr)

    def _reset_meters(self):
        for s in self.physical_strips:
            s.meter.hold_l = -60.0
            s.meter.hold_r = -60.0
        for s in self.virtual_strips:
            s.meter.hold_l = -60.0
            s.meter.hold_r = -60.0
        for b in self.master_buses:
            b.meter.hold_l = -60.0
            b.meter.hold_r = -60.0

    def _open_apps_panel(self):
        if self.apps_panel is None:
            from gui.apps_panel import AppsPanel
            self.apps_panel = AppsPanel(self)
        self.apps_panel.show()
        self.apps_panel.raise_()
        self.apps_panel.activateWindow()

    def _open_recording_panel(self):
        if self.recording_panel is None:
            from gui.recording_panel import RecordingPanel
            self.recording_panel = RecordingPanel(self)
        self.recording_panel.show()
        self.recording_panel.raise_()
        self.recording_panel.activateWindow()

    def _on_feature_reply(self, command, data):
        if command not in ("record_status", "record_start", "record_stop"):
            return
        active = data.get("active", False)
        if hasattr(self, "record_button"):
            self.record_button.setText("● REC" if active else "RECORD")
            self.record_button.setStyleSheet("color: #FF3377; font-weight: bold;" if active else "")
            self.record_button.setToolTip(data.get("error") or data.get("message") or
                (f"Recording: {data.get('seconds', 0):.1f} seconds — click to stop/save" if active else "Record selected strips and buses to WAV"))
        if command != "record_status":
            self.log_message(data.get("message") or data.get("error") or
                ("WAV recording started." if active else "WAV recording stopped and saved."))

    def _open_health_panel(self):
        if self.health_panel is None:
            from gui.health_panel import HealthPanel
            self.health_panel = HealthPanel(self)
        self.health_panel.show()
        self.health_panel.raise_()
        self.health_panel.activateWindow()

    def _open_presets_menu(self):
        menu = QMenu(self)
        menu.setStyleSheet(f"""
            QMenu {{
                background-color: #080110;
                border: 1.5px solid {ACCENT_PURPLE};
                color: {TEXT_PRIMARY};
                padding: 4px;
                font-size: 8pt;
                font-family: Consolas, monospace;
            }}
            QMenu::item {{
                padding: 5px 14px;
                border-radius: 0px;
            }}
            QMenu::item:selected {{
                background-color: #24083b;
                color: {ACCENT_GREEN};
            }}
            QMenu::separator {{
                height: 1px;
                background: #3b1166;
                margin: 4px 0px;
            }}
        """)

        menu.addAction("Audio health…", self._open_health_panel)
        menu.addAction("App volumes…", self._open_apps_panel)
        menu.addAction("WAV recording…", self._open_recording_panel)
        menu.addAction("Global hotkeys…", self.hotkeys.open_dialog)
        menu.addSeparator()
        act_save = menu.addAction("💾 Save Settings (%APPDATA%\\Muxella)")
        act_save.triggered.connect(lambda: self.save_settings())

        act_save_as = menu.addAction("📁 Save Preset As...")
        act_save_as.triggered.connect(self._save_preset_as_dialog)

        menu.addSeparator()

        act_load = menu.addAction("🔄 Reload Saved Settings")
        act_load.triggered.connect(lambda: self.load_settings())

        act_load_from = menu.addAction("📂 Load Preset From File...")
        act_load_from.triggered.connect(self._load_preset_from_dialog)

        menu.addSeparator()
        act_cable_driver = menu.addAction("Install virtual audio transport...")
        act_cable_driver.triggered.connect(self.cable_process.install_driver)
        act_cable_sync = menu.addAction("Reconnect virtual cables")
        act_cable_sync.triggered.connect(self.cable_process.sync)
        depth_menu = menu.addMenu("Virtual cable bit depth")
        for bits in (16, 24, 32):
            action = depth_menu.addAction(f"{bits}-bit PCM / 48 kHz")
            action.setCheckable(True)
            action.setChecked(bits == self.cable_process.bits)
            action.triggered.connect(lambda _, depth=bits: self._set_cable_depth(depth))

        menu.addSeparator()

        act_reset = menu.addAction("⚠️ Reset All (Default Levels)")
        act_reset.triggered.connect(self._reset_all_to_defaults)

        menu.addSeparator()

        self.act_menu_autostart = menu.addAction("🚀 Start with Windows")
        self.act_menu_autostart.setCheckable(True)
        self.act_menu_autostart.setChecked(is_start_with_windows_enabled())
        self.act_menu_autostart.toggled.connect(self._toggle_autostart)

        menu.addSeparator()

        act_min = menu.addAction("🔽 Minimize to System Tray")
        act_min.triggered.connect(self.hide)

        act_exit = menu.addAction("❌ Exit & Stop Engine")
        act_exit.triggered.connect(self.quit_application)

        menu.exec(self.preset_btn.mapToGlobal(QPoint(0, self.preset_btn.height())))

    def _save_preset_as_dialog(self):
        default_dir = str(get_settings_path().parent)
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Muxella Preset",
            default_dir,
            "Muxella Preset (*.json);;All Files (*.*)"
        )
        if path:
            self.save_settings(Path(path))

    def _load_preset_from_dialog(self):
        default_dir = str(get_settings_path().parent)
        path, _ = QFileDialog.getOpenFileName(
            self, "Load Muxella Preset",
            default_dir,
            "Muxella Preset (*.json);;All Files (*.*)"
        )
        if path:
            self.load_settings(Path(path))

    def _reset_all_to_defaults(self):
        for s in self.physical_strips:
            s.fader.setValue(0)
            s._on_fader_changed(0)
            s.mute_btn.setChecked(False)
            s._on_mute_clicked(False)
            s.solo_btn.setChecked(False)
            s._on_solo_clicked(False)
            s.mono_btn.setChecked(False)
            s._on_mono_clicked(False)
            s.comp_knob.set_value(0.0)
            s._on_comp_changed(0.0)
            s.gate_knob.set_value(0.0)
            s._on_gate_changed(0.0)
            s.denoise_knob.set_value(0.0)
            s._on_denoise_changed(0.0)
            s.rev_knob.set_value(0.0)
            s._on_reverb_changed(0.0)

        for s in self.virtual_strips:
            s.fader.setValue(0)
            s._on_fader_changed(0)
            s.mute_btn.setChecked(False)
            s._on_mute_clicked(False)
            s.solo_btn.setChecked(False)
            s._on_solo_clicked(False)
            s.mc_btn.setChecked(False)
            s._on_mc_clicked(False)
            s.pan_slider.setValue(0)
            s.client.set_pan(s.strip_id, 0.0)
            s.bass_knob.set_value(0.0)
            s.mid_knob.set_value(0.0)
            s.treble_knob.set_value(0.0)
            s._on_eq_changed()

        for b in self.master_buses:
            b.limiter_btn.setChecked(False)
            b._on_limiter_clicked(False)
            b.fader.setValue(0)
            b._on_fader_changed(0)
            b.mute_btn.setChecked(False)
            b._on_mute_clicked(False)
            b.mono_btn.setChecked(False)
            b._on_mono_clicked(False)
            b.eq_btn.setChecked(False)
            b._on_eq_clicked(False)
            b.client.set_bus_eq_params(b.bus_id, 0.0, 0.0, 0.0, 100.0, 1000.0, 8000.0, 1.0)

        self.log_message("All channels and controls reset to factory defaults.")

    def save_settings(self, file_path: Optional[Path] = None) -> bool:
        target = file_path if file_path else get_settings_path()
        try:
            state = {
                "hotkeys": self.hotkeys.bindings,
                "hotkey_feedback": self.hotkeys.feedback_state(),
                "recording_preferences": self.recording_preferences,
                "voice_combo": self.hotkeys.combo_state(),
                "version": "1.0",
                "timestamp": datetime.now().isoformat(),
                "window": {
                    "width": self.width(),
                    "height": self.height(),
                },
                "audio_engine": {
                    "sample_rate": self.sample_rate,
                    "buffer_frames": self.buffer_frames
                },
                "physical_strips": [s.get_state() for s in self.physical_strips],
                "virtual_strips": [s.get_state() for s in self.virtual_strips],
                "master_buses": [b.get_state() for b in self.master_buses]
            }
            target.parent.mkdir(parents=True, exist_ok=True)
            with open(target, "w", encoding="utf-8") as f:
                json.dump(state, f, indent=2)
            self.log_message(f"Saved configuration to: {target}")
            return True
        except Exception as e:
            self.log_message(f"Failed to save settings: {e}")
            return False

    def load_settings(self, file_path: Optional[Path] = None) -> bool:
        target = file_path if file_path else get_settings_path()
        if not target.exists():
            return False
        try:
            with open(target, "r", encoding="utf-8") as f:
                state = json.load(f)

            self.hotkeys.deafened = False
            if isinstance(state.get("recording_preferences"), dict):
                self.recording_preferences = state["recording_preferences"]
            self.hotkeys.restore_feedback(state.get("hotkey_feedback", {}))
            if "hotkeys" in state and isinstance(state["hotkeys"], dict):
                for error in self.hotkeys.apply(state["hotkeys"]):
                    self.log_message("Hotkey: " + error)

            from gui.virtual_audio import migrate_device_name
            for group in ("physical_strips", "virtual_strips", "master_buses"):
                for item in state.get(group, []):
                    if isinstance(item.get("device"), str):
                        item["device"] = migrate_device_name(item["device"])

            if "audio_engine" in state:
                ae = state["audio_engine"]
                if "sample_rate" in ae:
                    self.sample_rate = int(ae["sample_rate"])
                if "buffer_frames" in ae:
                    self.buffer_frames = int(ae["buffer_frames"])
                self._update_status_pill()

            if "physical_strips" in state:
                for idx, s_data in enumerate(state["physical_strips"]):
                    if idx < len(self.physical_strips):
                        self.physical_strips[idx].set_state(s_data)

            if "virtual_strips" in state:
                for idx, s_data in enumerate(state["virtual_strips"]):
                    if idx < len(self.virtual_strips):
                        self.virtual_strips[idx].set_state(s_data)

            if "master_buses" in state:
                for idx, b_data in enumerate(state["master_buses"]):
                    if idx < len(self.master_buses):
                        self.master_buses[idx].set_state(b_data)

            self.hotkeys.restore_combo(state.get("voice_combo", {}))
            self.log_message(f"Loaded configuration from: {target}")
            return True
        except Exception as e:
            self.log_message(f"Failed to load settings: {e}")
            return False

    def _init_tray_icon(self):
        icon_path = get_resource_dir() / "icons" / "app.ico"
        icon = QIcon(str(icon_path)) if icon_path.exists() else self.windowIcon()

        self.tray_icon = QSystemTrayIcon(icon, self)
        self.tray_icon.setToolTip("MUXELLA")

        tray_menu = QMenu()
        tray_menu.setStyleSheet(f"""
            QMenu {{
                background-color: #080110;
                border: 1.5px solid {ACCENT_PURPLE};
                color: {TEXT_PRIMARY};
                padding: 4px;
                font-size: 8.5pt;
                font-family: Consolas, monospace;
            }}
            QMenu::item {{
                padding: 5px 16px;
                border-radius: 0px;
            }}
            QMenu::item:selected {{
                background-color: #24083b;
                color: {ACCENT_GREEN};
            }}
            QMenu::separator {{
                height: 1px;
                background: #3b1166;
                margin: 4px 0px;
            }}
        """)

        act_show_hide = tray_menu.addAction("Show / Hide MUXELLA")
        act_show_hide.triggered.connect(self._toggle_window_visibility)

        act_save = tray_menu.addAction("💾 Save Settings")
        act_save.triggered.connect(lambda: self.save_settings())

        tray_menu.addAction("Audio health…", self._open_health_panel)
        tray_menu.addAction("App volumes…", self._open_apps_panel)
        tray_menu.addAction("WAV recording…", self._open_recording_panel)
        tray_menu.addAction("Global hotkeys…", self.hotkeys.open_dialog)
        tray_menu.addSeparator()

        self.act_tray_autostart = tray_menu.addAction("🚀 Start with Windows")
        self.act_tray_autostart.setCheckable(True)
        self.act_tray_autostart.setChecked(is_start_with_windows_enabled())
        self.act_tray_autostart.toggled.connect(self._toggle_autostart)

        tray_menu.addSeparator()

        act_restart_engine = tray_menu.addAction("🔄 Restart Audio Engine")
        act_restart_engine.triggered.connect(self._restart_audio_engine)

        tray_menu.addSeparator()

        act_quit = tray_menu.addAction("❌ Exit & Stop Audio")
        act_quit.triggered.connect(self.quit_application)

        self.tray_icon.setContextMenu(tray_menu)
        self.tray_icon.activated.connect(self._on_tray_activated)
        self.tray_icon.show()

    def _toggle_autostart_clicked(self):
        new_state = not is_start_with_windows_enabled()
        self._toggle_autostart(new_state)

    def _toggle_autostart(self, enabled: bool):
        success = set_start_with_windows(enabled)
        if success:
            state_str = "ENABLED (Starts in System Tray on Windows Boot)" if enabled else "DISABLED"
            self.log_message(f"Windows Autostart: {state_str}")
        else:
            self.log_message("Failed to update Windows Autostart registry key.")
        self._sync_autostart_ui()

    def _sync_autostart_ui(self):
        enabled = is_start_with_windows_enabled()
        if hasattr(self, "autostart_btn"):
            if enabled:
                self.autostart_btn.setText("🚀 AUTOSTART: ON")
                self.autostart_btn.setToolTip("Start with Windows: ENABLED (Starts silently in System Tray). Click to disable.")
                self.autostart_btn.setStyleSheet(f"""
                    QPushButton {{
                        background-color: #0b2413;
                        color: {ACCENT_GREEN};
                        border: 1px solid {ACCENT_GREEN};
                        font-family: Consolas, monospace;
                        font-size: 7.5pt;
                        font-weight: bold;
                        padding: 2px 10px;
                        letter-spacing: 0.5px;
                    }}
                    QPushButton:hover {{
                        background-color: #124d26;
                        color: #FFFFFF;
                        border: 1px solid #00FF88;
                    }}
                """)
            else:
                self.autostart_btn.setText("🚀 AUTOSTART: OFF")
                self.autostart_btn.setToolTip("Start with Windows: DISABLED. Click to enable.")
                self.autostart_btn.setStyleSheet(f"""
                    QPushButton {{
                        background-color: #120421;
                        color: {TEXT_MUTED};
                        border: 1px solid #3b1166;
                        font-family: Consolas, monospace;
                        font-size: 7.5pt;
                        font-weight: bold;
                        padding: 2px 10px;
                        letter-spacing: 0.5px;
                    }}
                    QPushButton:hover {{
                        background-color: #24083b;
                        color: {TEXT_PRIMARY};
                        border: 1px solid {ACCENT_PURPLE};
                    }}
                """)
        if hasattr(self, "act_tray_autostart"):
            self.act_tray_autostart.blockSignals(True)
            self.act_tray_autostart.setChecked(enabled)
            self.act_tray_autostart.blockSignals(False)
        if hasattr(self, "act_menu_autostart"):
            self.act_menu_autostart.blockSignals(True)
            self.act_menu_autostart.setChecked(enabled)
            self.act_menu_autostart.blockSignals(False)

    def _toggle_window_visibility(self):
        if self.isVisible() and not self.isMinimized():
            self.hide()
        else:
            self.showNormal()
            self.raise_()
            self.activateWindow()

    def _on_tray_activated(self, reason):
        if reason in (QSystemTrayIcon.ActivationReason.Trigger, QSystemTrayIcon.ActivationReason.DoubleClick):
            self._toggle_window_visibility()

    def _open_engine_config_dialog(self):
        dlg = AudioEngineConfigDialog(self.sample_rate, self.buffer_frames, self)
        dlg.config_applied.connect(self._apply_audio_engine_config)
        dlg.exec()

    def _apply_audio_engine_config(self, sample_rate: int, buffer_frames: int):
        if self.sample_rate == sample_rate and self.buffer_frames == buffer_frames:
            return
        self.sample_rate = sample_rate
        self.buffer_frames = buffer_frames
        self._update_status_pill()
        self.save_settings()
        self._restart_audio_engine()

    def _update_status_pill(self):
        if hasattr(self, "status_pill"):
            self._pill_is_online = None
            self._set_pill_online()

    def _set_pill_online(self):
        if not self._pill_is_online:
            self._pill_is_online = True
            self.status_pill.setText(f"● PRO AUDIO MMCSS ACTIVE ({self.sample_rate} Hz | {self.buffer_frames} FRAMES)")
            self.status_pill.setStyleSheet(f"""
                QPushButton {{
                    background-color: #0b2413;
                    color: {ACCENT_GREEN};
                    border: 1px solid {ACCENT_GREEN};
                    padding: 2px 12px;
                    font-weight: bold;
                    font-size: 7.5pt;
                    font-family: Consolas, monospace;
                    letter-spacing: 0.5px;
                }}
                QPushButton:hover {{
                    background-color: #124d26;
                    color: #FFFFFF;
                    border: 1px solid #00FF88;
                }}
                QPushButton:pressed {{
                    background-color: #00FF88;
                    color: #000000;
                }}
            """)

    def _set_pill_offline(self):
        if self._pill_is_online or self._pill_is_online is None:
            self._pill_is_online = False
            self.status_pill.setText("● ENGINE OFFLINE / CONNECTING")
            self.status_pill.setStyleSheet(f"""
                QPushButton {{
                    background-color: #330810;
                    color: #FF1A53;
                    border: 1px solid #FF1A53;
                    padding: 2px 12px;
                    font-weight: bold;
                    font-size: 7.5pt;
                    font-family: Consolas, monospace;
                    letter-spacing: 0.5px;
                }}
                QPushButton:hover {{
                    background-color: #4d0c18;
                    color: #FFFFFF;
                    border: 1px solid #FF4D79;
                }}
            """)

    def _restart_audio_engine(self):
        if self._restart_pending:
            return
        self._restart_pending = True
        self.log_message("Restarting audio engine; any recording will be finalized first...")
        def restart():
            self.client.raw.send_command("record_stop", timeout=20.0)
            self.client.raw.send_command("shutdown")
            if self.engine_process:
                try:
                    self.engine_process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.engine_process.terminate()
                    self.engine_process.wait(timeout=2)
                self.engine_process = None
            self._force_relaunch = True
            self._ensure_engine_running(client=self.client.raw)
            return bool(self.client.raw.send_command("status"))
        self.client.submit_task("restart-engine", restart)

    def _on_background_task(self, name, result):
        if self._is_quitting:
            return
        if name == "performance":
            self.performance_snapshot = result if isinstance(result, dict) else {"error": str(result)}
            data = self.performance_snapshot
            cpu, ram = data.get("cpu"), data.get("ram_bytes")
            cpu_text = f"{cpu:.1f}%" if cpu is not None else "—"
            ram_text = f"{ram / 1048576:.0f} MiB" if ram is not None else "—"
            self.performance_label.setText(f"CPU {cpu_text} | RAM {ram_text}" + (" *" if data.get("partial") else ""))
            self.performance_label.setToolTip(data.get("error") or
                f"GUI, audio engine, cable host/broker and their children. CPU: {data.get('cpu_readable', 0)}/{data.get('processes', 0)} processes; RAM: {data.get('ram_readable', 0)}/{data.get('processes', 0)}.\nCPU is normalized to total logical processor capacity. RAM sums working sets; shared pages may be counted twice.\nWindows audio service/driver CPU outside these processes is not included.\n* means some process counters were inaccessible.")
            return
        if name == "start-engine":
            self._restart_pending = False
            self.log_message("Audio engine ready." if result is True else f"Audio engine startup failed: {result}")
        elif name == "restart-engine":
            self._restart_pending = False
            if result is True:
                self.load_settings()
                self.log_message("Audio engine restarted and settings restored.")
            else:
                self.log_message(f"Audio engine restart failed: {result}")
        elif name == "cable-depth":
            self.log_message(f"Cable format change completed: {self.cable_process.bits}-bit PCM")
        if isinstance(result, dict) and "error" in result:
            self.log_message(f"Background operation failed: {result['error']}")
        self.client.get_devices()

    def _set_cable_depth(self, bits):
        if bits == self.cable_process.bits:
            return
        for index in range(5, 8):
            self.client.send_command(f"detach_input {index}")
            self.client.send_command(f"detach_output {index}")
        self.client.submit_task("cable-depth", lambda: self.cable_process.set_bits(bits, should_stop=lambda: self.client._stopping))
        self.log_message(f"Changing virtual cables to {bits}-bit PCM in the background...")

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, "scanlines"):
            self.scanlines.setGeometry(self.rect())

    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() == QEvent.Type.WindowStateChange:
            self._update_polling_rate()
            if hasattr(self, "title_bar") and hasattr(self.title_bar, "max_btn"):
                if self.isMaximized():
                    self.title_bar.max_btn.setText("🗗")
                    self.title_bar.max_btn.setToolTip("Restore")
                else:
                    self.title_bar.max_btn.setText("🗖")
                    self.title_bar.max_btn.setToolTip("Maximize")

    def _update_polling_rate(self):
        visible = self.isVisible() and not self.isMinimized()
        if hasattr(self, "poll_timer"):
            self.poll_timer.setInterval(33 if visible else 500)
        if hasattr(self, "perf_timer"):
            self.perf_timer.setInterval(2000 if visible else 5000)

    def showEvent(self, event):
        super().showEvent(event)
        self._update_polling_rate()

    def hideEvent(self, event):
        super().hideEvent(event)
        self._update_polling_rate()

    def quit_application(self):
        self._is_quitting = True
        self.close()
        # EQ/settings dialogs may still be hidden or open. Closing the main
        # window alone does not guarantee that Qt ends its event loop.
        QApplication.instance().quit()

    def closeEvent(self, event):
        if not self._is_quitting:
            event.ignore()
            self.hide()
            if hasattr(self, "tray_icon") and self.tray_icon.isVisible() and not self._notified_tray:
                self._notified_tray = True
                self.tray_icon.showMessage(
                    "Muxella Audio Active",
                    "Muxella is minimized to the system tray. Audio routing continues uninterrupted.\nRight-click tray icon to Exit.",
                    QSystemTrayIcon.MessageIcon.Information,
                    2500
                )
            return

        self.poll_timer.stop()
        self.dev_timer.stop()

        # 1. Auto-save current settings to recommended location (%APPDATA%\Muxella\settings.json)
        try:
            self.save_settings()
        except Exception as e:
            print(f"Failed to auto-save settings on exit: {e}")

        self.hotkeys.clear()
        self.record_timer.stop()
        self.perf_timer.stop()
        for panel in (self.apps_panel, self.recording_panel):
            if panel is not None:
                panel.close()
        QApplication.instance().removeNativeEventFilter(self.hotkeys)
        if self.health_panel is not None:
            self.health_panel.close()

        # 2. Instruct engine to shutdown cleanly over IPC
        self.client.close()
        try:
            self.client.raw.send_command("record_stop", timeout=20.0)
            self.client.raw.send_command("shutdown")
        except Exception:
            pass

        # 3. Wait or terminate engine process if managed
        if self.engine_process:
            try:
                self.engine_process.wait(timeout=3.0)
            except Exception:
                try:
                    self.engine_process.terminate()
                    self.engine_process.wait(timeout=2.0)
                except Exception:
                    try:
                        self.engine_process.kill()
                        self.engine_process.wait(timeout=2.0)
                    except Exception:
                        pass
            self.engine_process = None

        self.cable_process.stop()
        if hasattr(self, "tray_icon"):
            self.tray_icon.hide()
        event.accept()

# Backward compatibility alias
MetVoiceMainWindow = MuxellaMainWindow

