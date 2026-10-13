import os
import sys
import subprocess
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Any

from PyQt6.QtCore import Qt, QTimer, QSize
from PyQt6.QtGui import QIcon, QPixmap, QMovie, QColor, QFont
from PyQt6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QLabel, QPushButton, QSlider, QFrame, QGroupBox, QPlainTextEdit,
    QCheckBox, QScrollArea, QSplitter, QApplication
)

from .widgets import BlockyVuMeter, BlockyKnob, CrtScanlineOverlay
from .pipe_client import MetVoicePipeClient
from .theme import ACCENT_PURPLE, ACCENT_GREEN, BG_BLACK, BORDER_OUTLINE, SURFACE_CARD


class PhysicalInputStrip(QFrame):
    """
    Physical Input Strip (Strips 1..5) in ACCELA blocky style with VoiceMeeter Potato features:
    - Device label
    - Comp, Gate, Denoiser knobs
    - Reverb & Delay FX sends
    - Full 8-bus matrix routing buttons [A1..A5, B1..B3]
    - [MONO], [SOLO], [MUTE]
    - Gain Fader & Blocky LED VU Meter
    """
    def __init__(self, strip_id: int, title: str, default_device: str, client: MetVoicePipeClient, log_callback, parent=None):
        super().__init__(parent)
        self.strip_id = strip_id
        self.title = title
        self.client = client
        self.log_callback = log_callback

        self.setFixedWidth(135)
        self.setFrameShape(QFrame.Shape.Box)
        self.setStyleSheet(f"""
            PhysicalInputStrip {{
                background-color: #08020e;
                border: 1.5px solid {BORDER_OUTLINE};
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 6, 4, 6)
        layout.setSpacing(4)

        # 1. Header & Device
        hdr_lbl = QLabel(title.upper())
        hdr_lbl.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold; font-size: 8.5pt;")
        hdr_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(hdr_lbl)

        dev_lbl = QLabel(default_device)
        dev_lbl.setStyleSheet("color: #7b29a8; font-size: 7pt;")
        dev_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(dev_lbl)

        # 2. FX Knobs Row (Comp, Gate, Denoiser)
        knobs_box = QHBoxLayout()
        knobs_box.setSpacing(2)

        self.comp_knob = BlockyKnob("COMP", 0.0, 10.0, 0.0, step=0.1, parent=self)
        self.comp_knob.valueChanged.connect(lambda v: self.client.set_comp(self.strip_id, v))
        knobs_box.addWidget(self.comp_knob)

        self.gate_knob = BlockyKnob("GATE", 0.0, 10.0, 0.0, step=0.1, parent=self)
        knobs_box.addWidget(self.gate_knob)

        self.denoise_knob = BlockyKnob("DENOISE", 0.0, 10.0, 0.0, step=0.1, parent=self)
        knobs_box.addWidget(self.denoise_knob)
        layout.addLayout(knobs_box)

        # FX Sends (Reverb & Delay)
        fx_box = QHBoxLayout()
        fx_box.setSpacing(2)
        self.rev_knob = BlockyKnob("REVERB", 0.0, 10.0, 0.0, step=0.1, parent=self)
        self.delay_knob = BlockyKnob("DELAY", 0.0, 10.0, 0.0, step=0.1, parent=self)
        fx_box.addWidget(self.rev_knob)
        fx_box.addWidget(self.delay_knob)
        layout.addLayout(fx_box)

        # 3. Routing Grid [A1..A5] and [B1..B3]
        route_group = QGroupBox("BUS ROUTING")
        route_group.setStyleSheet(f"QGroupBox {{ font-size: 7.5pt; border: 1px solid #4a1469; margin-top: 4px; }}")
        route_grid = QGridLayout(route_group)
        route_grid.setContentsMargins(2, 6, 2, 2)
        route_grid.setSpacing(2)

        self.route_buttons: List[QPushButton] = []
        bus_names = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]

        for b_idx, b_name in enumerate(bus_names):
            btn = QPushButton(b_name)
            btn.setCheckable(True)
            btn.setFixedSize(26, 20)
            btn.setStyleSheet(f"""
                QPushButton {{
                    font-size: 7.5pt; padding: 0px;
                    border: 1px solid #6b1f94;
                    background-color: #0f041a;
                    color: #b829ea;
                }}
                QPushButton:checked {{
                    border: 1.5px solid {ACCENT_GREEN};
                    background-color: #0b2915;
                    color: {ACCENT_GREEN};
                    font-weight: bold;
                }}
            """)
            # Default routing: Mic 0 to B1 & B2
            if strip_id == 0 and b_name in ["B1", "B2"]:
                btn.setChecked(True)
            elif strip_id > 0 and b_name == "B1":
                btn.setChecked(True)

            btn.clicked.connect(lambda chk, b=b_idx: self._on_route_clicked(b, chk))
            self.route_buttons.append(btn)

            row = 0 if b_idx < 4 else (1 if b_idx < 8 else 2)
            col = b_idx % 4
            route_grid.addWidget(btn, row, col)

        layout.addWidget(route_group)

        # 4. Fader & Meter Section
        fader_row = QHBoxLayout()
        fader_row.setSpacing(6)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(2)

        self.fader = QSlider(Qt.Orientation.Vertical)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet("color: #e074ff; font-size: 8pt; font-weight: bold;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row)

        # 5. Quick Toggles: MONO / SOLO / MUTE
        toggles_layout = QVBoxLayout()
        toggles_layout.setSpacing(2)

        sub_row = QHBoxLayout()
        sub_row.setSpacing(2)

        self.mono_btn = QPushButton("MONO")
        self.mono_btn.setCheckable(True)
        self.mono_btn.setFixedHeight(22)
        sub_row.addWidget(self.mono_btn)

        self.solo_btn = QPushButton("SOLO")
        self.solo_btn.setCheckable(True)
        self.solo_btn.setFixedHeight(22)
        self.solo_btn.clicked.connect(self._on_solo_clicked)
        sub_row.addWidget(self.solo_btn)

        toggles_layout.addLayout(sub_row)

        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setFixedHeight(24)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        toggles_layout.addWidget(self.mute_btn)

        layout.addLayout(toggles_layout)

    def _on_route_clicked(self, bus_idx: int, enabled: bool):
        self.client.set_route(self.strip_id, bus_idx, enabled, 0.0)
        bus_name = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"][bus_idx]
        self.log_callback(f"Strip {self.strip_id + 1} -> Bus {bus_name}: {'ON' if enabled else 'OFF'}")

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_gain(self.strip_id, db)

    def _on_mute_clicked(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #ff1a53; color: #ffffff; border: 1.5px solid #ff1a53; font-weight: bold;")
        else:
            self.mute_btn.setStyleSheet("")
        self.client.set_mute(self.strip_id, checked)
        self.log_callback(f"{self.title} Mute: {checked}")

    def _on_solo_clicked(self, checked: bool):
        if checked:
            self.solo_btn.setStyleSheet("background-color: #ffcc00; color: #000000; border: 1.5px solid #ffcc00; font-weight: bold;")
        else:
            self.solo_btn.setStyleSheet("")
        self.client.set_solo(self.strip_id, checked)
        self.log_callback(f"{self.title} Solo: {checked}")


class VirtualInputStrip(QFrame):
    """
    Virtual Input Strip (Strips 6..8) in ACCELA blocky style with VoiceMeeter Potato features:
    - Virtual format label (48.0 kHz)
    - 3-band Equalizer knobs (Bass, Mid, Treble)
    - Surround/Pan balance
    - Full 8-bus matrix routing buttons [A1..A5, B1..B3]
    - [M.C], [SOLO], [MUTE]
    - Gain Fader & Multi-channel Blocky LED VU Meter
    """
    def __init__(self, strip_id: int, title: str, subtitle: str, client: MetVoicePipeClient, log_callback, parent=None):
        super().__init__(parent)
        self.strip_id = strip_id
        self.title = title
        self.client = client
        self.log_callback = log_callback

        self.setFixedWidth(145)
        self.setFrameShape(QFrame.Shape.Box)
        self.setStyleSheet(f"""
            VirtualInputStrip {{
                background-color: #06020c;
                border: 1.5px solid #a855f7;
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 6, 4, 6)
        layout.setSpacing(4)

        # 1. Header
        hdr_lbl = QLabel(title.upper())
        hdr_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-weight: bold; font-size: 8.5pt;")
        hdr_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(hdr_lbl)

        sub_lbl = QLabel(subtitle)
        sub_lbl.setStyleSheet("color: #7b29a8; font-size: 7pt;")
        sub_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(sub_lbl)

        # 2. 3-Band Parametric EQ Knobs (Treble, Mid, Bass)
        eq_box = QHBoxLayout()
        eq_box.setSpacing(2)

        self.bass_knob = BlockyKnob("BASS", -12.0, 12.0, 0.0, step=0.5, unit="dB", parent=self)
        self.bass_knob.valueChanged.connect(self._on_eq_changed)
        eq_box.addWidget(self.bass_knob)

        self.mid_knob = BlockyKnob("MID", -12.0, 12.0, 0.0, step=0.5, unit="dB", parent=self)
        self.mid_knob.valueChanged.connect(self._on_eq_changed)
        eq_box.addWidget(self.mid_knob)

        self.treble_knob = BlockyKnob("TREBLE", -12.0, 12.0, 0.0, step=0.5, unit="dB", parent=self)
        self.treble_knob.valueChanged.connect(self._on_eq_changed)
        eq_box.addWidget(self.treble_knob)

        layout.addLayout(eq_box)

        # Balance / Pan Slider
        pan_row = QHBoxLayout()
        pan_lbl_l = QLabel("L")
        pan_lbl_l.setStyleSheet("color: #7b29a8; font-size: 7.5pt;")
        self.pan_slider = QSlider(Qt.Orientation.Horizontal)
        self.pan_slider.setRange(-100, 100)
        self.pan_slider.setValue(0)
        self.pan_slider.valueChanged.connect(lambda v: self.client.set_pan(self.strip_id, v / 100.0))
        pan_lbl_r = QLabel("R")
        pan_lbl_r.setStyleSheet("color: #7b29a8; font-size: 7.5pt;")

        pan_row.addWidget(pan_lbl_l)
        pan_row.addWidget(self.pan_slider)
        pan_row.addWidget(pan_lbl_r)
        layout.addLayout(pan_row)

        # 3. Routing Grid [A1..A5] and [B1..B3]
        route_group = QGroupBox("BUS ROUTING")
        route_group.setStyleSheet(f"QGroupBox {{ font-size: 7.5pt; border: 1px solid #4a1469; margin-top: 4px; }}")
        route_grid = QGridLayout(route_group)
        route_grid.setContentsMargins(2, 6, 2, 2)
        route_grid.setSpacing(2)

        self.route_buttons: List[QPushButton] = []
        bus_names = ["A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3"]

        for b_idx, b_name in enumerate(bus_names):
            btn = QPushButton(b_name)
            btn.setCheckable(True)
            btn.setFixedSize(28, 20)
            btn.setStyleSheet(f"""
                QPushButton {{
                    font-size: 7.5pt; padding: 0px;
                    border: 1px solid #6b1f94;
                    background-color: #0f041a;
                    color: #b829ea;
                }}
                QPushButton:checked {{
                    border: 1.5px solid {ACCENT_GREEN};
                    background-color: #0b2915;
                    color: {ACCENT_GREEN};
                    font-weight: bold;
                }}
            """)
            # Virtual inputs default to A1 (Speakers)
            if b_name == "A1":
                btn.setChecked(True)
            # Spotify to Stream B1 as well
            if strip_id == 6 and b_name == "B1":
                btn.setChecked(True)

            btn.clicked.connect(lambda chk, b=b_idx: self._on_route_clicked(b, chk))
            self.route_buttons.append(btn)

            row = 0 if b_idx < 4 else 1
            col = b_idx % 4
            route_grid.addWidget(btn, row, col)

        layout.addWidget(route_group)

        # 4. Fader & Meter Section
        fader_row = QHBoxLayout()
        fader_row.setSpacing(6)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(2)

        self.fader = QSlider(Qt.Orientation.Vertical)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 8pt; font-weight: bold;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row)

        # 5. Quick Toggles: M.C / SOLO / MUTE
        toggles_layout = QVBoxLayout()
        toggles_layout.setSpacing(2)

        sub_row = QHBoxLayout()
        sub_row.setSpacing(2)

        self.mc_btn = QPushButton("M.C")
        self.mc_btn.setCheckable(True)
        self.mc_btn.setFixedHeight(22)
        sub_row.addWidget(self.mc_btn)

        self.solo_btn = QPushButton("SOLO")
        self.solo_btn.setCheckable(True)
        self.solo_btn.setFixedHeight(22)
        self.solo_btn.clicked.connect(self._on_solo_clicked)
        sub_row.addWidget(self.solo_btn)

        toggles_layout.addLayout(sub_row)

        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setFixedHeight(24)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        toggles_layout.addWidget(self.mute_btn)

        layout.addLayout(toggles_layout)

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

    def _on_mute_clicked(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #ff1a53; color: #ffffff; border: 1.5px solid #ff1a53; font-weight: bold;")
        else:
            self.mute_btn.setStyleSheet("")
        self.client.set_mute(self.strip_id, checked)
        self.log_callback(f"{self.title} Mute: {checked}")

    def _on_solo_clicked(self, checked: bool):
        if checked:
            self.solo_btn.setStyleSheet("background-color: #ffcc00; color: #000000; border: 1.5px solid #ffcc00; font-weight: bold;")
        else:
            self.solo_btn.setStyleSheet("")
        self.client.set_solo(self.strip_id, checked)
        self.log_callback(f"{self.title} Solo: {checked}")


class MasterBusStrip(QFrame):
    """
    Master Output Bus Strip (A1..A5 Physical, B1..B3 Virtual) in ACCELA blocky style:
    - Bus ID badge (e.g. [A1] SPEAKERS)
    - [SEL], [MONO], [EQ] buttons
    - Master Fader & Master Blocky LED VU Meter
    - Master [MUTE]
    - Soft Limiter safety status indicator
    """
    def __init__(self, bus_id: int, bus_code: str, name: str, is_virtual: bool, client: MetVoicePipeClient, log_callback, parent=None):
        super().__init__(parent)
        self.bus_id = bus_id
        self.bus_code = bus_code
        self.name = name
        self.client = client
        self.log_callback = log_callback

        self.setFixedWidth(105)
        self.setFrameShape(QFrame.Shape.Box)
        border_col = "#00ff66" if is_virtual else "#a855f7"
        self.setStyleSheet(f"""
            MasterBusStrip {{
                background-color: #06020c;
                border: 1.5px solid {border_col};
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 6, 4, 6)
        layout.setSpacing(4)

        # Bus Tag
        code_lbl = QLabel(f"[{bus_code}]")
        code_lbl.setStyleSheet(f"color: {border_col}; font-weight: bold; font-size: 9.5pt;")
        code_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(code_lbl)

        name_lbl = QLabel(name)
        name_lbl.setStyleSheet("color: #7b29a8; font-size: 7pt;")
        name_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(name_lbl)

        # Mode Toggles
        mode_box = QHBoxLayout()
        mode_box.setSpacing(2)

        mono_btn = QPushButton("MONO")
        mono_btn.setCheckable(True)
        mono_btn.setFixedHeight(20)
        mode_box.addWidget(mono_btn)

        eq_btn = QPushButton("EQ")
        eq_btn.setCheckable(True)
        eq_btn.setFixedHeight(20)
        mode_box.addWidget(eq_btn)
        layout.addLayout(mode_box)

        # Meter + Fader
        fader_row = QHBoxLayout()
        fader_row.setSpacing(4)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(2)

        self.fader = QSlider(Qt.Orientation.Vertical)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet(f"color: {border_col}; font-size: 7.5pt; font-weight: bold;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row)

        # Master MUTE
        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.setFixedHeight(24)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        layout.addWidget(self.mute_btn)

        # Limiter indicator
        limiter_badge = QLabel("● LIMITER")
        limiter_badge.setStyleSheet(f"color: {border_col}; font-size: 7pt; border: 1px solid {border_col}; padding: 1px;")
        limiter_badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(limiter_badge)

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_bus_gain(self.bus_id, db)

    def _on_mute_clicked(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #ff1a53; color: #ffffff; border: 1.5px solid #ff1a53; font-weight: bold;")
        else:
            self.mute_btn.setStyleSheet("")
        self.client.set_bus_mute(self.bus_id, checked)
        self.log_callback(f"Master Bus {self.bus_code} Mute: {checked}")


class MetVoiceMainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("METVOICE // CYBER AUDIO MATRIX ROUTER [POTATO CONSOLE]")
        self.resize(1440, 840)

        self.client = MetVoicePipeClient()
        self.engine_process = None

        self._ensure_engine_running()
        self._init_ui()

        # Telemetry Polling Timer (30 FPS)
        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll_telemetry)
        self.poll_timer.start(33)

    def _ensure_engine_running(self):
        if self.client.send_command("status"):
            return

        server_exe = Path(__file__).resolve().parent.parent / "build" / "metvoice_server.exe"
        if server_exe.exists():
            try:
                self.engine_process = subprocess.Popen(
                    [str(server_exe)],
                    creationflags=subprocess.CREATE_NEW_CONSOLE
                )
            except Exception as e:
                print(f"Failed to start audio engine: {e}")

    def _init_ui(self):
        central = QWidget(self)
        central.setObjectName("CentralWidget")
        self.setCentralWidget(central)

        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(8, 8, 8, 4)
        main_layout.setSpacing(6)

        # 1. Top Cyber Bar & Tony Logo
        top_bar = QHBoxLayout()
        top_bar.setSpacing(10)

        res_dir = Path(__file__).resolve().parent.parent / "res"
        tony_gif = res_dir / "tony.gif"
        accela_logo = res_dir / "accela.png"

        logo_lbl = QLabel()
        if tony_gif.exists():
            movie = QMovie(str(tony_gif))
            logo_lbl.setMovie(movie)
            movie.start()
        elif accela_logo.exists():
            pix = QPixmap(str(accela_logo)).scaledToHeight(40, Qt.TransformationMode.SmoothTransformation)
            logo_lbl.setPixmap(pix)
        top_bar.addWidget(logo_lbl)

        hdr_vbox = QVBoxLayout()
        hdr_vbox.setSpacing(0)
        title_lbl = QLabel("METVOICE // CYBER AUDIO MATRIX")
        title_lbl.setStyleSheet(f"color: {ACCENT_PURPLE}; font-size: 13pt; font-weight: bold; letter-spacing: 2px;")
        sub_lbl = QLabel("8-CHANNEL HARDWARE & VIRTUAL CONSOLE // WASAPI MMCSS PRO AUDIO")
        sub_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 7.5pt;")
        hdr_vbox.addWidget(title_lbl)
        hdr_vbox.addWidget(sub_lbl)
        top_bar.addLayout(hdr_vbox)

        top_bar.addStretch()

        # Engine Telemetry Pill
        self.status_pill = QLabel("● PRO AUDIO MMCSS ACTIVE (48000 Hz | 480 FRAMES)")
        self.status_pill.setStyleSheet(f"""
            QLabel {{
                background-color: #0b2413;
                color: {ACCENT_GREEN};
                border: 1.5px solid {ACCENT_GREEN};
                padding: 4px 12px;
                font-weight: bold;
                font-size: 8pt;
            }}
        """)
        top_bar.addWidget(self.status_pill)
        main_layout.addLayout(top_bar)

        # 2. Main Mixing Desk Horizontal Scroll Area
        scroll_area = QScrollArea(self)
        scroll_area.setWidgetResizable(True)
        scroll_area.setStyleSheet(f"QScrollArea {{ border: 1.5px solid {BORDER_OUTLINE}; background-color: #040108; }}")

        desk_widget = QWidget()
        desk_layout = QHBoxLayout(desk_widget)
        desk_layout.setContentsMargins(6, 6, 6, 6)
        desk_layout.setSpacing(6)

        # A. 5 PHYSICAL INPUT STRIPS
        phys_box = QVBoxLayout()
        phys_hdr = QLabel("PHYSICAL INPUTS (1..5)")
        phys_hdr.setStyleSheet(f"color: {ACCENT_PURPLE}; font-size: 8pt; font-weight: bold;")
        phys_box.addWidget(phys_hdr)

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
            strip = PhysicalInputStrip(s_idx, t, dev, self.client, self.log_message, self)
            self.physical_strips.append(strip)
            phys_strips_row.addWidget(strip)

        phys_box.addLayout(phys_strips_row)
        desk_layout.addLayout(phys_box)

        # Separator VLine
        sep1 = QFrame()
        sep1.setFrameShape(QFrame.Shape.VLine)
        sep1.setStyleSheet("color: #4a1469;")
        desk_layout.addWidget(sep1)

        # B. 3 VIRTUAL INPUT STRIPS (VAIO 1..3)
        virt_box = QVBoxLayout()
        virt_hdr = QLabel("VIRTUAL INPUTS (VAIO 1..3)")
        virt_hdr.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 8pt; font-weight: bold;")
        virt_box.addWidget(virt_hdr)

        virt_strips_row = QHBoxLayout()
        virt_strips_row.setSpacing(6)
        self.virtual_strips: List[VirtualInputStrip] = []

        virt_configs = [
            (5, "Main VAIO", "Desktop / System"),
            (6, "Spotify VAIO", "Music / Player"),
            (7, "Discord VAIO", "Comms / Voice Chat"),
        ]

        for s_idx, t, sub in virt_configs:
            strip = VirtualInputStrip(s_idx, t, sub, self.client, self.log_message, self)
            self.virtual_strips.append(strip)
            virt_strips_row.addWidget(strip)

        virt_box.addLayout(virt_strips_row)
        desk_layout.addLayout(virt_box)

        # Separator VLine
        sep2 = QFrame()
        sep2.setFrameShape(QFrame.Shape.VLine)
        sep2.setStyleSheet("color: #4a1469;")
        desk_layout.addWidget(sep2)

        # C. 8 MASTER OUTPUT BUSES (A1..A5 Physical + B1..B3 Virtual)
        master_box = QVBoxLayout()
        master_hdr = QLabel("MASTER OUTPUT BUSES (A1..A5 PHYSICAL / B1..B3 VIRTUAL)")
        master_hdr.setStyleSheet("color: #a855f7; font-size: 8pt; font-weight: bold;")
        master_box.addWidget(master_hdr)

        master_strips_row = QHBoxLayout()
        master_strips_row.setSpacing(6)
        self.master_buses: List[MasterBusStrip] = []

        bus_configs = [
            (0, "A1", "Speakers", False),
            (1, "A2", "Headphones", False),
            (2, "A3", "Monitors", False),
            (3, "A4", "Aux Out", False),
            (4, "A5", "Line Out", False),
            (5, "B1", "OBS Stream", True),
            (6, "B2", "Discord Mic", True),
            (7, "B3", "Recording", True),
        ]

        for b_idx, code, name, is_v in bus_configs:
            bus_strip = MasterBusStrip(b_idx, code, name, is_v, self.client, self.log_message, self)
            self.master_buses.append(bus_strip)
            master_strips_row.addWidget(bus_strip)

        master_box.addLayout(master_strips_row)
        desk_layout.addLayout(master_box)

        scroll_area.setWidget(desk_widget)
        main_layout.addWidget(scroll_area, stretch=5)

        # 3. Diagnostic Cyber Terminal (Matching ASSella screenshot)
        term_box = QVBoxLayout()
        term_box.setSpacing(2)

        term_hdr = QLabel("DIAGNOSTIC TELEMETRY & ROUTING LOGS")
        term_hdr.setStyleSheet("color: #7b29a8; font-size: 7.5pt; font-weight: bold;")
        term_box.addWidget(term_hdr)

        self.terminal = QPlainTextEdit(self)
        self.terminal.setReadOnly(True)
        self.terminal.setMaximumHeight(105)
        term_box.addWidget(self.terminal)
        main_layout.addLayout(term_box, stretch=1)

        self.log_message("MetVoice 8x8 Cyber Audio Matrix initialized.")
        self.log_message("All 5 Physical Strips, 3 Virtual Strips, and 8 Master Buses loaded.")

        # 4. Bottom Titlebar (Matching ACCELA screenshot)
        bottom_bar = QFrame(self)
        bottom_bar.setFixedHeight(34)
        bottom_bar.setStyleSheet(f"""
            QFrame {{
                background-color: #06020a;
                border-top: 1.5px solid {BORDER_OUTLINE};
            }}
        """)
        bot_layout = QHBoxLayout(bottom_bar)
        bot_layout.setContentsMargins(10, 0, 10, 0)
        bot_layout.setSpacing(12)

        bot_left = QLabel("TONY v1.0.dev")
        bot_left.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold; font-size: 8.5pt;")
        bot_layout.addWidget(bot_left)

        bot_mid = QLabel("ACCELA")
        bot_mid.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold; letter-spacing: 3px; font-size: 10.5pt;")
        bot_layout.addWidget(bot_mid, alignment=Qt.AlignmentFlag.AlignCenter)

        bot_layout.addStretch()

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

        main_layout.addWidget(bottom_bar)

        # Subtle CRT scanline overlay
        self.scanlines = CrtScanlineOverlay(self)
        self.scanlines.setGeometry(self.rect())
        self.scanlines.lower()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, "scanlines"):
            self.scanlines.setGeometry(self.rect())

    def log_message(self, text: str):
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S,%f")[:-3]
        self.terminal.appendPlainText(f"{now_str} - metvoice.ui - {text}")
        self.terminal.verticalScrollBar().setValue(self.terminal.verticalScrollBar().maximum())

    def _poll_telemetry(self):
        data = self.client.get_meters()
        if not data:
            self.status_pill.setText("● ENGINE OFFLINE / CONNECTING")
            self.status_pill.setStyleSheet("background-color: #330810; color: #ff1a53; border: 1.5px solid #ff1a53; padding: 4px 12px; font-weight: bold; font-size: 8pt;")
            return

        self.status_pill.setText("● PRO AUDIO MMCSS ACTIVE (48000 Hz | 480 FRAMES)")
        self.status_pill.setStyleSheet(f"background-color: #0b2413; color: {ACCENT_GREEN}; border: 1.5px solid {ACCENT_GREEN}; padding: 4px 12px; font-weight: bold; font-size: 8pt;")

        strips_data = data.get("strips", [])
        # Strips 0..4 are physical
        for i in range(min(5, len(strips_data))):
            s_info = strips_data[i]
            pl = s_info.get("peak_l", -60.0)
            pr = s_info.get("peak_r", -60.0)
            self.physical_strips[i].meter.set_levels(pl, pr)

        # Strips 5..7 are virtual
        for idx, s_idx in enumerate([5, 6, 7]):
            if s_idx < len(strips_data):
                s_info = strips_data[s_idx]
                pl = s_info.get("peak_l", -60.0)
                pr = s_info.get("peak_r", -60.0)
                self.virtual_strips[idx].meter.set_levels(pl, pr)

        # Master Buses (0..7)
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

    def closeEvent(self, event):
        if self.engine_process:
            self.engine_process.terminate()
        event.accept()
