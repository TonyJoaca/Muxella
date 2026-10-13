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
    QCheckBox, QSplitter, QApplication
)

from .widgets import BlockyVuMeter, CrtScanlineOverlay
from .pipe_client import MetVoicePipeClient
from .theme import ACCENT_PURPLE, ACCENT_GREEN, BG_BLACK, BORDER_OUTLINE


class ChannelStripWidget(QFrame):
    def __init__(self, strip_id: int, name: str, client: MetVoicePipeClient, log_callback, parent=None):
        super().__init__(parent)
        self.strip_id = strip_id
        self.name = name
        self.client = client
        self.log_callback = log_callback

        self.setFrameShape(QFrame.Shape.Box)
        self.setStyleSheet(f"""
            ChannelStripWidget {{
                background-color: #0c0414;
                border: 1.5px solid {BORDER_OUTLINE};
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        # Header Title
        self.title_lbl = QLabel(f"[{strip_id + 1}] {name.upper()}")
        self.title_lbl.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold; font-size: 9.5pt;")
        self.title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.title_lbl)

        # Center Section: Meter + Fader
        fader_row = QHBoxLayout()
        fader_row.setSpacing(8)

        # Vertical Meter
        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        # Vertical Gain Fader (-60 dB to +12 dB)
        fader_col = QVBoxLayout()
        fader_col.setSpacing(4)

        self.fader = QSlider(Qt.Orientation.Vertical)
        self.fader.setRange(-600, 120) # -60.0 to +12.0 dB (scaled by 10)
        self.fader.setValue(0)
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet("color: #e074ff; font-size: 8.5pt;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row)

        # Panning Slider
        pan_box = QHBoxLayout()
        pan_lbl_l = QLabel("L")
        pan_lbl_l.setStyleSheet("color: #7b29a8; font-size: 8pt;")
        self.pan_slider = QSlider(Qt.Orientation.Horizontal)
        self.pan_slider.setRange(-100, 100)
        self.pan_slider.setValue(0)
        self.pan_slider.valueChanged.connect(self._on_pan_changed)
        pan_lbl_r = QLabel("R")
        pan_lbl_r.setStyleSheet("color: #7b29a8; font-size: 8pt;")

        pan_box.addWidget(pan_lbl_l)
        pan_box.addWidget(self.pan_slider)
        pan_box.addWidget(pan_lbl_r)
        layout.addLayout(pan_box)

        # Buttons Row: MUTE / SOLO
        btn_row = QHBoxLayout()
        btn_row.setSpacing(4)

        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        btn_row.addWidget(self.mute_btn)

        self.solo_btn = QPushButton("SOLO")
        self.solo_btn.setCheckable(True)
        self.solo_btn.clicked.connect(self._on_solo_clicked)
        btn_row.addWidget(self.solo_btn)

        layout.addLayout(btn_row)

        # DSP Row: EQ / COMP
        dsp_row = QHBoxLayout()
        dsp_row.setSpacing(4)

        self.eq_btn = QPushButton("EQ")
        self.eq_btn.setCheckable(True)
        self.eq_btn.setChecked(strip_id == 0) # Vocals EQ enabled by default
        self.eq_btn.clicked.connect(lambda: self.log_callback(f"Strip {self.strip_id + 1} EQ toggled"))
        dsp_row.addWidget(self.eq_btn)

        self.comp_btn = QPushButton("COMP")
        self.comp_btn.setCheckable(True)
        self.comp_btn.setChecked(strip_id == 0) # Vocals Comp enabled by default
        self.comp_btn.clicked.connect(lambda: self.log_callback(f"Strip {self.strip_id + 1} Compressor toggled"))
        dsp_row.addWidget(self.comp_btn)

        layout.addLayout(dsp_row)

        # Matrix Routing Toggles
        routes_group = QGroupBox("BUS ASSIGN")
        routes_group.setStyleSheet(f"QGroupBox {{ font-size: 8pt; border: 1px solid {BORDER_OUTLINE}; }}")
        routes_layout = QVBoxLayout(routes_group)
        routes_layout.setContentsMargins(4, 8, 4, 4)
        routes_layout.setSpacing(2)

        self.route_a1 = QCheckBox("A1 MONITORS")
        self.route_a1.setChecked(strip_id != 0) # Aux/Apps to monitors by default
        self.route_a1.toggled.connect(lambda chk: self._on_route_toggled(0, chk))
        routes_layout.addWidget(self.route_a1)

        self.route_b1 = QCheckBox("B1 BROADCAST")
        self.route_b1.setChecked(True) # All to stream by default
        self.route_b1.toggled.connect(lambda chk: self._on_route_toggled(1, chk))
        routes_layout.addWidget(self.route_b1)

        layout.addWidget(routes_group)

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_gain(self.strip_id, db)

    def _on_pan_changed(self, val: int):
        pan = val / 100.0
        self.client.set_pan(self.strip_id, pan)

    def _on_mute_clicked(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #ff1a53; color: #ffffff; border: 1.5px solid #ff1a53;")
        else:
            self.mute_btn.setStyleSheet("")
        self.client.set_mute(self.strip_id, checked)
        self.log_callback(f"Strip {self.strip_id + 1} Mute: {checked}")

    def _on_solo_clicked(self, checked: bool):
        if checked:
            self.solo_btn.setStyleSheet("background-color: #ffcc00; color: #000000; border: 1.5px solid #ffcc00;")
        else:
            self.solo_btn.setStyleSheet("")
        self.client.set_solo(self.strip_id, checked)
        self.log_callback(f"Strip {self.strip_id + 1} Solo: {checked}")

    def _on_route_toggled(self, bus_id: int, enabled: bool):
        self.client.set_route(self.strip_id, bus_id, enabled, 0.0)
        bus_name = "A1" if bus_id == 0 else "B1"
        self.log_callback(f"Routing Crosspoint [{self.name} -> Bus {bus_name}]: {enabled}")


class MasterBusWidget(QFrame):
    def __init__(self, bus_id: int, name: str, client: MetVoicePipeClient, log_callback, parent=None):
        super().__init__(parent)
        self.bus_id = bus_id
        self.name = name
        self.client = client
        self.log_callback = log_callback

        self.setFrameShape(QFrame.Shape.Box)
        self.setStyleSheet(f"""
            MasterBusWidget {{
                background-color: #08020e;
                border: 1.5px solid #9c27b0;
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        self.title_lbl = QLabel(f"[BUS {name}]")
        self.title_lbl.setStyleSheet("color: #00ff66; font-weight: bold; font-size: 9.5pt;")
        self.title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.title_lbl)

        fader_row = QHBoxLayout()
        fader_row.setSpacing(8)

        self.meter = BlockyVuMeter(self)
        fader_row.addWidget(self.meter)

        fader_col = QVBoxLayout()
        fader_col.setSpacing(4)

        self.fader = QSlider(Qt.Orientation.Vertical)
        self.fader.setRange(-600, 120)
        self.fader.setValue(0)
        self.fader.valueChanged.connect(self._on_fader_changed)
        fader_col.addWidget(self.fader, alignment=Qt.AlignmentFlag.AlignCenter)

        self.gain_lbl = QLabel("0.0 dB")
        self.gain_lbl.setStyleSheet("color: #00ff66; font-size: 8.5pt;")
        self.gain_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        fader_col.addWidget(self.gain_lbl)

        fader_row.addLayout(fader_col)
        layout.addLayout(fader_row)

        self.mute_btn = QPushButton("MUTE")
        self.mute_btn.setCheckable(True)
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        layout.addWidget(self.mute_btn)

        self.limiter_badge = QLabel("● LIMITER ARMED")
        self.limiter_badge.setStyleSheet("color: #00ff66; font-size: 8pt; border: 1px solid #00ff66; padding: 2px;")
        self.limiter_badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.limiter_badge)

    def _on_fader_changed(self, val: int):
        db = val / 10.0
        self.gain_lbl.setText(f"{db:+.1f} dB" if db > -60.0 else "-inf dB")
        self.client.set_bus_gain(self.bus_id, db)

    def _on_mute_clicked(self, checked: bool):
        if checked:
            self.mute_btn.setStyleSheet("background-color: #ff1a53; color: #ffffff;")
        else:
            self.mute_btn.setStyleSheet("")
        self.log_callback(f"Master Bus {self.name} Mute: {checked}")


class MetVoiceMainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("METVOICE // CYBER AUDIO MATRIX ROUTER")
        self.resize(1020, 720)

        self.client = MetVoicePipeClient()
        self.engine_process = None

        self._ensure_engine_running()
        self._init_ui()

        # Telemetry Polling Timer (30 FPS)
        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll_telemetry)
        self.poll_timer.start(33)

    def _ensure_engine_running(self):
        # Test if server is already answering
        if self.client.send_command("status"):
            return

        # Attempt to launch metvoice_server.exe in background
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
        main_layout.setContentsMargins(12, 12, 12, 6)
        main_layout.setSpacing(10)

        # 1. Top Cyber Banner
        top_banner = QHBoxLayout()
        top_banner.setSpacing(12)

        # Tony / Lain Gif or Logo
        res_dir = Path(__file__).resolve().parent.parent / "res"
        tony_gif = res_dir / "tony.gif"
        accela_logo = res_dir / "accela.png"

        logo_lbl = QLabel()
        if tony_gif.exists():
            movie = QMovie(str(tony_gif))
            logo_lbl.setMovie(movie)
            movie.start()
        elif accela_logo.exists():
            pix = QPixmap(str(accela_logo)).scaledToHeight(48, Qt.TransformationMode.SmoothTransformation)
            logo_lbl.setPixmap(pix)
        top_banner.addWidget(logo_lbl)

        # Title Block
        header_vbox = QVBoxLayout()
        header_vbox.setSpacing(2)
        title_lbl = QLabel("METVOICE // CYBER AUDIO MATRIX")
        title_lbl.setStyleSheet(f"color: {ACCENT_PURPLE}; font-size: 15pt; font-weight: bold; letter-spacing: 2px;")
        subtitle_lbl = QLabel("OPEN-SOURCE LOW-LATENCY WASAPI ROUTER & PRO AUDIO MIXER")
        subtitle_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 8.5pt;")
        header_vbox.addWidget(title_lbl)
        header_vbox.addWidget(subtitle_lbl)
        top_banner.addLayout(header_vbox)

        top_banner.addStretch()

        # Engine Status Pill
        self.status_pill = QLabel("● PRO AUDIO MMCSS ACTIVE")
        self.status_pill.setStyleSheet(f"""
            QLabel {{
                background-color: #0b2413;
                color: {ACCENT_GREEN};
                border: 1.5px solid {ACCENT_GREEN};
                padding: 6px 14px;
                font-weight: bold;
                font-size: 9pt;
            }}
        """)
        top_banner.addWidget(self.status_pill)
        main_layout.addLayout(top_banner)

        # 2. Main Middle Splitter: Strips on Left, Buses in Mid, Logs on Bottom
        middle_layout = QHBoxLayout()
        middle_layout.setSpacing(10)

        # Input Strips
        self.strip_widgets: List[ChannelStripWidget] = []
        strip_names = ["Physical Mic (In 1)", "Virtual Aux (In 2)", "Virtual Apps (In 3)"]
        for idx, name in enumerate(strip_names):
            sw = ChannelStripWidget(idx, name, self.client, self.log_message, self)
            self.strip_widgets.append(sw)
            middle_layout.addWidget(sw, stretch=2)

        # Separator Line
        sep = QFrame()
        sep.setFrameShape(QFrame.Shape.VLine)
        sep.setStyleSheet(f"color: {BORDER_OUTLINE};")
        middle_layout.addWidget(sep)

        # Output Master Buses
        self.bus_widgets: List[MasterBusWidget] = []
        bus_names = ["A1 (Monitors)", "B1 (Broadcast)"]
        for idx, name in enumerate(bus_names):
            bw = MasterBusWidget(idx, name, self.client, self.log_message, self)
            self.bus_widgets.append(bw)
            middle_layout.addWidget(bw, stretch=2)

        main_layout.addLayout(middle_layout, stretch=4)

        # 3. Diagnostic Cyber Terminal / Log Area (Inspired by ASSella screenshot)
        term_box = QVBoxLayout()
        term_box.setSpacing(2)
        term_hdr = QLabel("DIAGNOSTIC TELEMETRY & SYSTEM LOGS")
        term_hdr.setStyleSheet("color: #7b29a8; font-size: 8pt; font-weight: bold;")
        term_box.addWidget(term_hdr)

        self.terminal = QPlainTextEdit(self)
        self.terminal.setReadOnly(True)
        self.terminal.setMaximumHeight(140)
        term_box.addWidget(self.terminal)
        main_layout.addLayout(term_box, stretch=1)

        # Initial boot logs
        self.log_message("MetVoice Cyber Audio Matrix GUI initialized.")
        self.log_message("Connecting to Named Pipe IPC \\\\.\\pipe\\MetVoice_Control...")

        # 4. Bottom Titlebar (Matching ACCELA screenshot)
        bottom_bar = QFrame(self)
        bottom_bar.setFixedHeight(36)
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
        bot_left.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold;")
        bot_layout.addWidget(bot_left)

        bot_mid = QLabel("ACCELA")
        bot_mid.setStyleSheet(f"color: {ACCENT_PURPLE}; font-weight: bold; letter-spacing: 3px; font-size: 11pt;")
        bot_layout.addWidget(bot_mid, alignment=Qt.AlignmentFlag.AlignCenter)

        bot_layout.addStretch()

        rescan_btn = QPushButton("RESCAN DEVICES")
        rescan_btn.setFixedHeight(24)
        rescan_btn.clicked.connect(self._rescan_devices)
        bot_layout.addWidget(rescan_btn)

        clear_btn = QPushButton("CLEAR LOGS")
        clear_btn.setFixedHeight(24)
        clear_btn.clicked.connect(self.terminal.clear)
        bot_layout.addWidget(clear_btn)

        quit_btn = QPushButton("QUIT")
        quit_btn.setFixedHeight(24)
        quit_btn.clicked.connect(self.close)
        bot_layout.addWidget(quit_btn)

        main_layout.addWidget(bottom_bar)

        # Add subtle CRT scanlines overlay
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
            self.status_pill.setStyleSheet("background-color: #330810; color: #ff1a53; border: 1.5px solid #ff1a53;")
            return

        self.status_pill.setText("● PRO AUDIO MMCSS ACTIVE")
        self.status_pill.setStyleSheet(f"background-color: #0b2413; color: {ACCENT_GREEN}; border: 1.5px solid {ACCENT_GREEN};")

        # Update Strips
        strips_data = data.get("strips", [])
        for i, s_info in enumerate(strips_data):
            if i < len(self.strip_widgets):
                pl = s_info.get("peak_l", -60.0)
                pr = s_info.get("peak_r", -60.0)
                self.strip_widgets[i].meter.set_levels(pl, pr)

        # Update Buses
        buses_data = data.get("buses", [])
        for j, b_info in enumerate(buses_data):
            if j < len(self.bus_widgets):
                pl = b_info.get("peak_l", -60.0)
                pr = b_info.get("peak_r", -60.0)
                self.bus_widgets[j].meter.set_levels(pl, pr)

    def _rescan_devices(self):
        self.log_message("Requesting hardware device rescan from audio server...")

    def closeEvent(self, event):
        if self.engine_process:
            self.engine_process.terminate()
        event.accept()
