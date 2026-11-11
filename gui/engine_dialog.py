"""
Audio Engine Configuration Dialog for MetVoice:
Allows configuring real-time Sample Rate (kHz) and Buffer Size (Frames)
with dynamic latency readouts and instant engine restart.
"""
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QComboBox, QFrame, QWidget
)

from .theme import (
    ACCENT_GREEN, ACCENT_PURPLE, ACCENT_CYAN, ACCENT_RED,
    TEXT_PRIMARY, TEXT_MUTED, BG_BLACK, FONT_PIXEL, FONT_BODY
)

class AudioEngineConfigDialog(QDialog):
    """
    Cyberpunk-styled Hardware Engine Configuration Dialog for kHz & Buffer.
    """
    config_applied = pyqtSignal(int, int) # (sample_rate, buffer_frames)

    SAMPLE_RATES = [
        (44100, "44100 Hz  (44.1 kHz - CD Standard)"),
        (48000, "48000 Hz  (48.0 kHz - Broadcast/Studio Default)"),
        (88200, "88200 Hz  (88.2 kHz - High Definition)"),
        (96000, "96000 Hz  (96.0 kHz - Hi-Res Studio)"),
        (192000, "192000 Hz (192.0 kHz - Ultra Hi-Res)"),
    ]

    BUFFER_SIZES = [
        (128, "128 Frames  (~2.7 ms) - Ultra-Low Latency"),
        (256, "256 Frames  (~5.3 ms) - Low Latency"),
        (480, "480 Frames  (10.0 ms) - Windows Native Quantum [Default]"),
        (512, "512 Frames  (~10.7 ms) - Pro Audio Standard"),
        (1024, "1024 Frames (~21.3 ms) - Heavy Load"),
        (2048, "2048 Frames (~42.7 ms) - Maximum Cushion"),
    ]

    def __init__(self, current_sample_rate: int = 48000, current_buffer: int = 480, parent=None):
        super().__init__(parent)
        self.setWindowTitle("AUDIO ENGINE HARDWARE CONFIGURATION")
        self.setFixedSize(480, 360)
        self.setWindowFlags(self.windowFlags() & ~Qt.WindowType.WindowContextHelpButtonHint)

        self.setStyleSheet(f"""
            QDialog {{
                background-color: #070112;
                border: 2px solid {ACCENT_GREEN};
            }}
            QLabel {{
                color: {TEXT_PRIMARY};
                font-family: 'Segoe UI', Consolas, sans-serif;
            }}
            QComboBox {{
                background-color: #0d021c;
                border: 1.5px solid #3b1166;
                color: {ACCENT_CYAN};
                font-family: Consolas, monospace;
                font-size: 8.5pt;
                font-weight: bold;
                padding: 6px 12px;
                border-radius: 2px;
            }}
            QComboBox:hover {{
                border: 1.5px solid {ACCENT_CYAN};
                background-color: #120326;
            }}
            QComboBox::drop-down {{
                border: none;
                width: 24px;
            }}
            QComboBox QAbstractItemView {{
                background-color: #080110;
                border: 1.5px solid {ACCENT_PURPLE};
                color: {TEXT_PRIMARY};
                selection-background-color: #24083b;
                selection-color: {ACCENT_GREEN};
                font-family: Consolas, monospace;
                font-size: 8.5pt;
                padding: 4px;
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(20, 18, 20, 18)
        layout.setSpacing(14)

        # 1. Header Banner
        hdr_box = QVBoxLayout()
        hdr_box.setSpacing(2)

        title = QLabel("● PRO AUDIO MMCSS HARDWARE ENGINE", self)
        title.setStyleSheet(f"""
            font-family: '{FONT_PIXEL}', Consolas, monospace;
            font-size: 11pt;
            font-weight: bold;
            color: {ACCENT_GREEN};
            letter-spacing: 1.5px;
        """)
        hdr_box.addWidget(title)

        subtitle = QLabel("CONFIGURE SAMPLE RATE & DMA BUFFER FRAMES", self)
        subtitle.setStyleSheet(f"font-size: 7.5pt; color: {TEXT_MUTED}; letter-spacing: 0.5px;")
        hdr_box.addWidget(subtitle)
        layout.addLayout(hdr_box)

        sep = QFrame(self)
        sep.setFrameShape(QFrame.Shape.HLine)
        sep.setStyleSheet(f"background-color: #260840; max-height: 1px;")
        layout.addWidget(sep)

        # 2. Sample Rate Selector
        rate_layout = QVBoxLayout()
        rate_layout.setSpacing(4)
        rate_lbl = QLabel("SAMPLE RATE (kHz):", self)
        rate_lbl.setStyleSheet(f"font-family: Consolas, monospace; font-size: 8pt; font-weight: bold; color: {ACCENT_PURPLE};")
        rate_layout.addWidget(rate_lbl)

        self.rate_combo = QComboBox(self)
        for val, desc in self.SAMPLE_RATES:
            self.rate_combo.addItem(desc, val)
        # Select current
        idx = self.rate_combo.findData(current_sample_rate)
        if idx != -1:
            self.rate_combo.setCurrentIndex(idx)
        else:
            self.rate_combo.addItem(f"{current_sample_rate} Hz (Custom)", current_sample_rate)
            self.rate_combo.setCurrentIndex(self.rate_combo.count() - 1)
        self.rate_combo.currentIndexChanged.connect(self._update_readout)
        rate_layout.addWidget(self.rate_combo)
        layout.addLayout(rate_layout)

        # 3. Buffer Size Selector
        buf_layout = QVBoxLayout()
        buf_layout.setSpacing(4)
        buf_lbl = QLabel("BUFFER SIZE (FRAMES):", self)
        buf_lbl.setStyleSheet(f"font-family: Consolas, monospace; font-size: 8pt; font-weight: bold; color: {ACCENT_PURPLE};")
        buf_layout.addWidget(buf_lbl)

        self.buf_combo = QComboBox(self)
        for val, desc in self.BUFFER_SIZES:
            self.buf_combo.addItem(desc, val)
        idx = self.buf_combo.findData(current_buffer)
        if idx != -1:
            self.buf_combo.setCurrentIndex(idx)
        else:
            self.buf_combo.addItem(f"{current_buffer} Frames (Custom)", current_buffer)
            self.buf_combo.setCurrentIndex(self.buf_combo.count() - 1)
        self.buf_combo.currentIndexChanged.connect(self._update_readout)
        buf_layout.addWidget(self.buf_combo)
        layout.addLayout(buf_layout)

        # 4. Computed Latency HUD Box
        self.hud_box = QFrame(self)
        self.hud_box.setStyleSheet(f"""
            QFrame {{
                background-color: #0b1c11;
                border: 1px solid {ACCENT_GREEN};
                padding: 6px;
            }}
        """)
        hud_l = QVBoxLayout(self.hud_box)
        hud_l.setContentsMargins(8, 4, 8, 4)
        hud_l.setSpacing(2)

        self.latency_lbl = QLabel(self)
        self.latency_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-family: Consolas, monospace; font-size: 8.5pt; font-weight: bold;")
        hud_l.addWidget(self.latency_lbl)

        self.desc_lbl = QLabel(self)
        self.desc_lbl.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 7.5pt; font-family: Consolas, monospace;")
        hud_l.addWidget(self.desc_lbl)

        layout.addWidget(self.hud_box)

        # 5. Buttons
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(10)

        cancel_btn = QPushButton("CANCEL", self)
        cancel_btn.setFixedHeight(30)
        cancel_btn.setStyleSheet(f"""
            QPushButton {{
                background-color: #120421;
                color: {TEXT_MUTED};
                border: 1px solid #3b1166;
                font-family: Consolas, monospace;
                font-size: 8pt;
                font-weight: bold;
                padding: 4px 14px;
            }}
            QPushButton:hover {{
                background-color: #24083b;
                color: {TEXT_PRIMARY};
            }}
        """)
        cancel_btn.clicked.connect(self.reject)
        btn_layout.addWidget(cancel_btn)

        apply_btn = QPushButton("APPLY & RESTART ENGINE", self)
        apply_btn.setFixedHeight(30)
        apply_btn.setStyleSheet(f"""
            QPushButton {{
                background-color: #0b2413;
                color: {ACCENT_GREEN};
                border: 1.5px solid {ACCENT_GREEN};
                font-family: Consolas, monospace;
                font-size: 8pt;
                font-weight: bold;
                padding: 4px 18px;
            }}
            QPushButton:hover {{
                background-color: #124d26;
                color: #FFFFFF;
                border: 1.5px solid #00FF88;
            }}
            QPushButton:pressed {{
                background-color: #00FF88;
                color: #000000;
            }}
        """)
        apply_btn.clicked.connect(self._on_apply)
        btn_layout.addWidget(apply_btn)

        layout.addLayout(btn_layout)

        self._update_readout()

    def _update_readout(self):
        sr = self.selected_sample_rate
        bf = self.selected_buffer
        if sr > 0:
            lat_ms = (bf / sr) * 1000.0
            self.latency_lbl.setText(f"ESTIMATED HARDWARE BUFFER LATENCY: {lat_ms:.1f} ms")
            if bf == 480 and sr == 48000:
                self.desc_lbl.setText("● Windows Native 10.0 ms Quantum — Zero driver resampling or slicing.")
            elif bf <= 256:
                self.desc_lbl.setText("● Ultra-Low Latency Mode — Requires fast CPU or dedicated interface.")
            elif bf >= 1024:
                self.desc_lbl.setText("● Safe Mode Buffer — Maximum cushion against high CPU spikes / heavy load.")
            else:
                self.desc_lbl.setText("● Standard Low-Latency Audio Streaming.")
        else:
            self.latency_lbl.setText("LATENCY: --")
            self.desc_lbl.setText("")

    @property
    def selected_sample_rate(self) -> int:
        return self.rate_combo.currentData() or 48000

    @property
    def selected_buffer(self) -> int:
        return self.buf_combo.currentData() or 480

    def _on_apply(self):
        self.config_applied.emit(self.selected_sample_rate, self.selected_buffer)
        self.accept()
