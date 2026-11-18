import math
from typing import Optional, Callable
from PyQt6.QtCore import Qt, QRectF, pyqtSignal, QPointF
from PyQt6.QtGui import QPainter, QColor, QPen, QBrush, QFont, QLinearGradient, QPainterPath
from PyQt6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLabel, QPushButton, QFrame, QWidget
)

from gui.widgets import CyberSlider, BlockyKnob
from gui.pipe_client import MuxellaPipeClient
from gui.theme import (
    BG_BLACK, SURFACE_CARD, BORDER_OUTLINE, ACCENT_CYAN, ACCENT_GREEN,
    ACCENT_PURPLE, TEXT_PRIMARY, TEXT_MUTED, TEXT_DIM
)


def calc_rbj_coeffs(filter_type: str, f0: float, q: float, gain_db: float, sr: float = 48000.0):
    """Calculate RBJ Audio EQ Cookbook normalized coefficients."""
    f0 = max(10.0, min(f0, sr * 0.49))
    q = max(0.05, q)
    w0 = 2.0 * math.pi * (f0 / sr)
    cos_w0 = math.cos(w0)
    sin_w0 = math.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    a_gain = 10.0 ** (gain_db / 40.0)

    if filter_type == "peaking":
        b0 = 1.0 + alpha * a_gain
        b1 = -2.0 * cos_w0
        b2 = 1.0 - alpha * a_gain
        a0 = 1.0 + alpha / a_gain
        a1 = -2.0 * cos_w0
        a2 = 1.0 - alpha / a_gain
    elif filter_type == "low_shelf":
        sqrt_a_2 = 2.0 * math.sqrt(a_gain) * alpha
        b0 = a_gain * ((a_gain + 1.0) - (a_gain - 1.0) * cos_w0 + sqrt_a_2)
        b1 = 2.0 * a_gain * ((a_gain - 1.0) - (a_gain + 1.0) * cos_w0)
        b2 = a_gain * ((a_gain + 1.0) - (a_gain - 1.0) * cos_w0 - sqrt_a_2)
        a0 = (a_gain + 1.0) + (a_gain - 1.0) * cos_w0 + sqrt_a_2
        a1 = -2.0 * ((a_gain - 1.0) + (a_gain + 1.0) * cos_w0)
        a2 = (a_gain + 1.0) + (a_gain - 1.0) * cos_w0 - sqrt_a_2
    elif filter_type == "high_shelf":
        sqrt_a_2 = 2.0 * math.sqrt(a_gain) * alpha
        b0 = a_gain * ((a_gain + 1.0) + (a_gain - 1.0) * cos_w0 + sqrt_a_2)
        b1 = -2.0 * a_gain * ((a_gain - 1.0) + (a_gain + 1.0) * cos_w0)
        b2 = a_gain * ((a_gain + 1.0) + (a_gain - 1.0) * cos_w0 - sqrt_a_2)
        a0 = (a_gain + 1.0) - (a_gain - 1.0) * cos_w0 + sqrt_a_2
        a1 = 2.0 * ((a_gain - 1.0) - (a_gain + 1.0) * cos_w0)
        a2 = (a_gain + 1.0) - (a_gain - 1.0) * cos_w0 - sqrt_a_2
    else:
        return 1.0, 0.0, 0.0, 0.0, 0.0

    inv_a0 = 1.0 / a0
    return b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0


def eval_biquad_db(coeffs, f: float, sr: float = 48000.0) -> float:
    """Evaluate magnitude response in dB of a biquad filter at frequency f."""
    b0, b1, b2, a1, a2 = coeffs
    w = 2.0 * math.pi * (f / sr)
    cos1 = math.cos(w)
    sin1 = math.sin(w)
    cos2 = math.cos(2.0 * w)
    sin2 = math.sin(2.0 * w)

    nr = b0 + b1 * cos1 + b2 * cos2
    ni = -b1 * sin1 - b2 * sin2
    dr = 1.0 + a1 * cos1 + a2 * cos2
    di = -a1 * sin1 - a2 * sin2

    num_sq = nr * nr + ni * ni
    den_sq = dr * dr + di * di
    if den_sq < 1e-12:
        return 0.0
    mag_sq = num_sq / den_sq
    return 10.0 * math.log10(max(1e-12, mag_sq))


class EqCurveCanvas(QWidget):
    """
    Real-time interactive frequency response curve display (20 Hz - 20 kHz).
    Supports dragging node handles to set frequency and gain visually.
    """
    nodeChanged = pyqtSignal(int, float, float)  # (band_idx, gain_db, freq_hz)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(170)
        self.setMinimumWidth(460)

        self.low_gain = 0.0
        self.low_freq = 100.0
        self.mid_gain = 0.0
        self.mid_freq = 1000.0
        self.mid_q = 1.0
        self.high_gain = 0.0
        self.high_freq = 8000.0
        self.is_enabled = True

        self._active_node: Optional[int] = None
        self.setMouseTracking(True)

    def set_parameters(self, low_gain: float, low_freq: float,
                       mid_gain: float, mid_freq: float, mid_q: float,
                       high_gain: float, high_freq: float, enabled: bool = True):
        self.low_gain = low_gain
        self.low_freq = low_freq
        self.mid_gain = mid_gain
        self.mid_freq = mid_freq
        self.mid_q = mid_q
        self.high_gain = high_gain
        self.high_freq = high_freq
        self.is_enabled = enabled
        self.update()

    def _freq_to_x(self, freq: float) -> float:
        w = float(self.width())
        # Logarithmic scale 20 Hz .. 20000 Hz (3 decades)
        freq = max(20.0, min(freq, 20000.0))
        norm = (math.log10(freq) - math.log10(20.0)) / 3.0
        return norm * w

    def _x_to_freq(self, x: float) -> float:
        w = max(1.0, float(self.width()))
        norm = max(0.0, min(1.0, x / w))
        log_f = math.log10(20.0) + norm * 3.0
        return 10.0 ** log_f

    def _db_to_y(self, db: float) -> float:
        h = float(self.height())
        # Display range: -18 dB .. +18 dB
        norm = (db - (-18.0)) / 36.0
        return h - (norm * h)

    def _y_to_db(self, y: float) -> float:
        h = max(1.0, float(self.height()))
        norm = max(0.0, min(1.0, 1.0 - (y / h)))
        return -18.0 + norm * 36.0

    def _get_node_pos(self, band_idx: int) -> QPointF:
        if band_idx == 0:
            return QPointF(self._freq_to_x(self.low_freq), self._db_to_y(self.low_gain))
        elif band_idx == 1:
            return QPointF(self._freq_to_x(self.mid_freq), self._db_to_y(self.mid_gain))
        else:
            return QPointF(self._freq_to_x(self.high_freq), self._db_to_y(self.high_gain))

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            pos = event.position()
            # Hit-test the 3 nodes (radius 12 px)
            for idx in (0, 1, 2):
                node_pt = self._get_node_pos(idx)
                dx = pos.x() - node_pt.x()
                dy = pos.y() - node_pt.y()
                if math.hypot(dx, dy) <= 12.0:
                    self._active_node = idx
                    event.accept()
                    return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self._active_node is not None:
            pos = event.position()
            new_f = self._x_to_freq(pos.x())
            new_db = max(-12.0, min(12.0, self._y_to_db(pos.y())))

            if self._active_node == 0:
                new_f = max(30.0, min(500.0, new_f))
                self.low_freq = new_f
                self.low_gain = new_db
            elif self._active_node == 1:
                new_f = max(200.0, min(5000.0, new_f))
                self.mid_freq = new_f
                self.mid_gain = new_db
            elif self._active_node == 2:
                new_f = max(1000.0, min(18000.0, new_f))
                self.high_freq = new_f
                self.high_gain = new_db

            self.nodeChanged.emit(self._active_node, new_db, new_f)
            self.update()
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self._active_node is not None:
            self._active_node = None
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        w = self.width()
        h = self.height()

        # 1. Dark synthwave background
        painter.fillRect(0, 0, w, h, QColor("#08020E"))

        # 2. Outer border
        painter.setPen(QPen(QColor("#2B0C47"), 1))
        painter.drawRect(0, 0, w - 1, h - 1)

        # 3. Frequency Grid (Vertical lines & labels)
        freq_grid = [
            (50, "50Hz"), (100, "100Hz"), (250, "250"), (500, "500"),
            (1000, "1kHz"), (2000, "2kHz"), (4000, "4kHz"), (8000, "8kHz"), (16000, "16k")
        ]
        painter.setFont(QFont("Consolas", 6))
        for f, lbl in freq_grid:
            gx = self._freq_to_x(f)
            painter.setPen(QPen(QColor("#1A072B"), 1, Qt.PenStyle.DashLine))
            painter.drawLine(QPointF(gx, 0), QPointF(gx, h))
            painter.setPen(QColor("#5C2D91"))
            painter.drawText(QRectF(gx - 18, h - 14, 36, 12), Qt.AlignmentFlag.AlignCenter, lbl)

        # 4. Gain Grid (Horizontal lines & labels)
        gain_grid = [(12.0, "+12dB"), (6.0, "+6dB"), (0.0, "0dB"), (-6.0, "-6dB"), (-12.0, "-12dB")]
        for g_db, lbl in gain_grid:
            gy = self._db_to_y(g_db)
            if g_db == 0.0:
                painter.setPen(QPen(QColor("#7B2CBF"), 1, Qt.PenStyle.SolidLine))
            else:
                painter.setPen(QPen(QColor("#1A072B"), 1, Qt.PenStyle.DashLine))
            painter.drawLine(QPointF(0, gy), QPointF(w, gy))
            painter.setPen(QColor("#00FF66" if g_db == 0.0 else "#5C2D91"))
            painter.drawText(QRectF(3, gy - 6, 36, 12), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, lbl)

        # 5. Calculate Composite EQ Response Curve
        c_low = calc_rbj_coeffs("low_shelf", self.low_freq, 0.7071, self.low_gain)
        c_mid = calc_rbj_coeffs("peaking", self.mid_freq, self.mid_q, self.mid_gain)
        c_high = calc_rbj_coeffs("high_shelf", self.high_freq, 0.7071, self.high_gain)

        step_px = 3
        num_pts = (w // step_px) + 2
        path = QPainterPath()
        zero_y = self._db_to_y(0.0)

        curve_points = []
        for i in range(num_pts):
            px = min(float(w), float(i * step_px))
            f = self._x_to_freq(px)
            if self.is_enabled:
                resp_db = eval_biquad_db(c_low, f) + eval_biquad_db(c_mid, f) + eval_biquad_db(c_high, f)
            else:
                resp_db = 0.0
            py = self._db_to_y(resp_db)
            curve_points.append(QPointF(px, py))

        if curve_points:
            path.moveTo(curve_points[0])
            for pt in curve_points[1:]:
                path.lineTo(pt)

            # Translucent neon cyan filled area between curve and 0 dB line
            fill_path = QPainterPath(path)
            fill_path.lineTo(w, zero_y)
            fill_path.lineTo(0, zero_y)
            fill_path.closeSubpath()

            grad = QLinearGradient(0, 0, 0, h)
            grad.setColorAt(0.0, QColor(0, 229, 255, 35))
            grad.setColorAt(0.5, QColor(0, 229, 255, 15))
            grad.setColorAt(1.0, QColor(0, 229, 255, 5))
            painter.fillPath(fill_path, QBrush(grad))

            # Main Neon Stroke
            stroke_color = QColor("#00E5FF" if self.is_enabled else "#6B21A8")
            painter.setPen(QPen(stroke_color, 2.0))
            painter.drawPath(path)

        # 6. Interactive Draggable Nodes
        nodes = [
            (0, self.low_freq, self.low_gain, "#00E5FF", "LOW"),
            (1, self.mid_freq, self.mid_gain, "#00FF66", "MID"),
            (2, self.high_freq, self.high_gain, "#C084FC", "HIGH"),
        ]

        for idx, freq, gain, col_hex, tag in nodes:
            pt = self._get_node_pos(idx)
            is_active = (self._active_node == idx)

            # Outer glow
            painter.setBrush(QColor(col_hex))
            painter.setPen(QPen(QColor("#FFFFFF" if is_active else col_hex), 2))
            r = 7.0 if is_active else 5.5
            painter.drawEllipse(pt, r, r)

            # Center dark dot
            painter.setBrush(QColor("#08020E"))
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawEllipse(pt, 2.5, 2.5)

            # Tag label
            painter.setFont(QFont("Consolas", 6, QFont.Weight.Bold))
            painter.setPen(QColor(col_hex))
            y_offset = -14 if gain >= 0 else 12
            painter.drawText(QRectF(pt.x() - 25, pt.y() + y_offset, 50, 12),
                             Qt.AlignmentFlag.AlignCenter, f"{tag}")


class CyberEqualizerDialog(QDialog):
    """
    Parametric Equalizer floating window for Master Bus strips (A1..A5, B1..B3):
    - Real-time 3-band filtering (Low Shelf, Peaking Mid, High Shelf)
    - Interactive 20Hz-20kHz curve display with draggable handles
    - Keyboard precision input on right-click for all sliders and knobs
    - Live synchronization with C++ audio engine
    """
    def __init__(self, bus_id: int, bus_code: str, bus_name: str,
                 client: MuxellaPipeClient, on_state_changed: Optional[Callable] = None, parent=None):
        super().__init__(parent)
        self.bus_id = bus_id
        self.bus_code = bus_code
        self.bus_name = bus_name
        self.client = client
        self.on_state_changed = on_state_changed

        self.setWindowTitle(f"Muxella // EQUALIZER - {bus_code} ({bus_name})")
        self.resize(540, 480)
        self.setMinimumSize(500, 440)

        # Style dialog with synthwave frame
        self.setStyleSheet(f"""
            QDialog {{
                background-color: {BG_BLACK};
                border: 1.5px solid {BORDER_OUTLINE};
            }}
            QLabel {{
                font-family: Consolas, monospace;
            }}
        """)

        # Initial default values
        self.low_gain = 0.0
        self.low_freq = 100.0
        self.mid_gain = 0.0
        self.mid_freq = 1000.0
        self.mid_q = 1.0
        self.high_gain = 0.0
        self.high_freq = 8000.0
        self.is_enabled = True

        # Query existing EQ state from C++ engine if available
        self._load_current_state()

        # Build UI layout
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(14, 12, 14, 12)
        main_layout.setSpacing(10)

        # 1. Header Bar: Bus Tag, Name, and Bypass Button
        hdr_row = QHBoxLayout()
        hdr_tag = QLabel(f"// PARAMETRIC EQ [{bus_code}]")
        hdr_tag.setStyleSheet(f"color: {ACCENT_CYAN}; font-size: 9pt; font-weight: bold;")
        hdr_row.addWidget(hdr_tag)

        self.name_lbl = QLabel(bus_name)
        self.name_lbl.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 8.5pt;")
        hdr_row.addWidget(self.name_lbl)
        hdr_row.addStretch()

        self.bypass_btn = QPushButton("EQ ACTIVE" if self.is_enabled else "EQ BYPASSED")
        self.bypass_btn.setCheckable(True)
        self.bypass_btn.setChecked(self.is_enabled)
        self.bypass_btn.setFixedHeight(24)
        self._update_bypass_btn_style()
        self.bypass_btn.clicked.connect(self._on_bypass_toggle)
        hdr_row.addWidget(self.bypass_btn)
        main_layout.addLayout(hdr_row)

        # 2. Interactive Real-Time EQ Frequency Curve Canvas
        self.canvas = EqCurveCanvas(self)
        self.canvas.set_parameters(
            self.low_gain, self.low_freq,
            self.mid_gain, self.mid_freq, self.mid_q,
            self.high_gain, self.high_freq, self.is_enabled
        )
        self.canvas.nodeChanged.connect(self._on_node_dragged)
        main_layout.addWidget(self.canvas)

        # 3. 3-Band Control Strips (LOW SHELF | MID PEAK | HIGH SHELF)
        bands_frame = QFrame()
        bands_frame.setStyleSheet(f"""
            QFrame {{
                background-color: {SURFACE_CARD};
                border: 1px solid #2B0C47;
                border-radius: 2px;
            }}
        """)
        bands_layout = QHBoxLayout(bands_frame)
        bands_layout.setContentsMargins(10, 8, 10, 8)
        bands_layout.setSpacing(14)

        # --- Band 1: LOW SHELF ---
        low_col = QVBoxLayout()
        low_col.setSpacing(4)
        low_lbl = QLabel("LOW SHELF")
        low_lbl.setStyleSheet(f"color: {ACCENT_CYAN}; font-size: 7.5pt; font-weight: bold;")
        low_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        low_col.addWidget(low_lbl)

        low_slider_row = QHBoxLayout()
        self.low_gain_slider = CyberSlider(
            Qt.Orientation.Vertical, default_value=0,
            title=f"[{bus_code}] LOW GAIN", min_display=-12.0, max_display=12.0,
            display_scale=0.1, unit="dB", parent=self
        )
        self.low_gain_slider.setRange(-120, 120)
        self.low_gain_slider.setValue(int(round(self.low_gain * 10)))
        self.low_gain_slider.valueChanged.connect(self._on_low_slider_changed)
        low_slider_row.addWidget(self.low_gain_slider, alignment=Qt.AlignmentFlag.AlignCenter)
        low_col.addLayout(low_slider_row)

        self.low_gain_readout = QLabel(f"{self.low_gain:+.1f} dB")
        self.low_gain_readout.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 7.5pt;")
        self.low_gain_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        low_col.addWidget(self.low_gain_readout)

        self.low_freq_knob = BlockyKnob("FREQ", 30.0, 500.0, default_val=100.0, step=5.0, unit="Hz", parent=self)
        self.low_freq_knob.set_value(self.low_freq)
        self.low_freq_knob.valueChanged.connect(self._on_low_freq_changed)
        low_col.addWidget(self.low_freq_knob, alignment=Qt.AlignmentFlag.AlignCenter)

        bands_layout.addLayout(low_col)

        # Separator 1
        sep1 = QFrame()
        sep1.setFrameShape(QFrame.Shape.VLine)
        sep1.setStyleSheet("color: #24073A;")
        bands_layout.addWidget(sep1)

        # --- Band 2: MID PEAK ---
        mid_col = QVBoxLayout()
        mid_col.setSpacing(4)
        mid_lbl = QLabel("MID PEAK")
        mid_lbl.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 7.5pt; font-weight: bold;")
        mid_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        mid_col.addWidget(mid_lbl)

        mid_slider_row = QHBoxLayout()
        self.mid_gain_slider = CyberSlider(
            Qt.Orientation.Vertical, default_value=0,
            title=f"[{bus_code}] MID GAIN", min_display=-12.0, max_display=12.0,
            display_scale=0.1, unit="dB", parent=self
        )
        self.mid_gain_slider.setRange(-120, 120)
        self.mid_gain_slider.setValue(int(round(self.mid_gain * 10)))
        self.mid_gain_slider.valueChanged.connect(self._on_mid_slider_changed)
        mid_slider_row.addWidget(self.mid_gain_slider, alignment=Qt.AlignmentFlag.AlignCenter)
        mid_col.addLayout(mid_slider_row)

        self.mid_gain_readout = QLabel(f"{self.mid_gain:+.1f} dB")
        self.mid_gain_readout.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 7.5pt;")
        self.mid_gain_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        mid_col.addWidget(self.mid_gain_readout)

        mid_knobs_row = QHBoxLayout()
        mid_knobs_row.setSpacing(6)
        self.mid_freq_knob = BlockyKnob("FREQ", 200.0, 5000.0, default_val=1000.0, step=10.0, unit="Hz", parent=self)
        self.mid_freq_knob.set_value(self.mid_freq)
        self.mid_freq_knob.valueChanged.connect(self._on_mid_freq_changed)
        mid_knobs_row.addWidget(self.mid_freq_knob)

        self.mid_q_knob = BlockyKnob("Q", 0.3, 5.0, default_val=1.0, step=0.1, unit="", parent=self)
        self.mid_q_knob.set_value(self.mid_q)
        self.mid_q_knob.valueChanged.connect(self._on_mid_q_changed)
        mid_knobs_row.addWidget(self.mid_q_knob)
        mid_col.addLayout(mid_knobs_row)

        bands_layout.addLayout(mid_col)

        # Separator 2
        sep2 = QFrame()
        sep2.setFrameShape(QFrame.Shape.VLine)
        sep2.setStyleSheet("color: #24073A;")
        bands_layout.addWidget(sep2)

        # --- Band 3: HIGH SHELF ---
        high_col = QVBoxLayout()
        high_col.setSpacing(4)
        high_lbl = QLabel("HIGH SHELF")
        high_lbl.setStyleSheet(f"color: {ACCENT_PURPLE}; font-size: 7.5pt; font-weight: bold;")
        high_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        high_col.addWidget(high_lbl)

        high_slider_row = QHBoxLayout()
        self.high_gain_slider = CyberSlider(
            Qt.Orientation.Vertical, default_value=0,
            title=f"[{bus_code}] HIGH GAIN", min_display=-12.0, max_display=12.0,
            display_scale=0.1, unit="dB", parent=self
        )
        self.high_gain_slider.setRange(-120, 120)
        self.high_gain_slider.setValue(int(round(self.high_gain * 10)))
        self.high_gain_slider.valueChanged.connect(self._on_high_slider_changed)
        high_slider_row.addWidget(self.high_gain_slider, alignment=Qt.AlignmentFlag.AlignCenter)
        high_col.addLayout(high_slider_row)

        self.high_gain_readout = QLabel(f"{self.high_gain:+.1f} dB")
        self.high_gain_readout.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 7.5pt;")
        self.high_gain_readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        high_col.addWidget(self.high_gain_readout)

        self.high_freq_knob = BlockyKnob("FREQ", 1000.0, 18000.0, default_val=8000.0, step=50.0, unit="Hz", parent=self)
        self.high_freq_knob.set_value(self.high_freq)
        self.high_freq_knob.valueChanged.connect(self._on_high_freq_changed)
        high_col.addWidget(self.high_freq_knob, alignment=Qt.AlignmentFlag.AlignCenter)

        bands_layout.addLayout(high_col)

        main_layout.addWidget(bands_frame)

        # 4. Action Bar (FLAT / RESET FREQS / CLOSE)
        act_row = QHBoxLayout()
        act_row.setSpacing(8)

        flat_btn = QPushButton("FLAT (0 dB)")
        flat_btn.setFixedHeight(24)
        flat_btn.setStyleSheet("""
            QPushButton {
                background-color: #0b2915;
                color: #00ff66;
                border: 1px solid #00ff66;
                font-family: Consolas, monospace;
                font-size: 7.5pt;
                font-weight: bold;
                padding: 2px 10px;
            }
            QPushButton:hover {
                background-color: #124d26;
                color: #ffffff;
            }
        """)
        flat_btn.clicked.connect(self._reset_flat)
        act_row.addWidget(flat_btn)

        reset_f_btn = QPushButton("RESET FREQS")
        reset_f_btn.setFixedHeight(24)
        reset_f_btn.setStyleSheet("""
            QPushButton {
                background-color: #1a0526;
                color: #c084fc;
                border: 1px solid #7b2cbf;
                font-family: Consolas, monospace;
                font-size: 7.5pt;
                padding: 2px 10px;
            }
            QPushButton:hover {
                background-color: #330b4d;
                color: #ffffff;
            }
        """)
        reset_f_btn.clicked.connect(self._reset_frequencies)
        act_row.addWidget(reset_f_btn)

        act_row.addStretch()

        close_btn = QPushButton("CLOSE")
        close_btn.setFixedHeight(24)
        close_btn.setStyleSheet("""
            QPushButton {
                background-color: #100318;
                color: #b388ff;
                border: 1px solid #3d145c;
                font-family: Consolas, monospace;
                font-size: 7.5pt;
                padding: 2px 14px;
            }
            QPushButton:hover {
                background-color: #2b0c47;
                color: #ffffff;
            }
        """)
        close_btn.clicked.connect(self.close)
        act_row.addWidget(close_btn)

        main_layout.addLayout(act_row)

    def _load_current_state(self):
        eq_info = self.client.get_bus_eq_params(self.bus_id)
        if eq_info and eq_info.get("status") == "ok":
            self.is_enabled = bool(eq_info.get("enabled", True))
            self.low_gain = float(eq_info.get("low_gain", 0.0))
            self.low_freq = float(eq_info.get("low_freq", 100.0))
            self.mid_gain = float(eq_info.get("mid_gain", 0.0))
            self.mid_freq = float(eq_info.get("mid_freq", 1000.0))
            self.mid_q = float(eq_info.get("mid_q", 1.0))
            self.high_gain = float(eq_info.get("high_gain", 0.0))
            self.high_freq = float(eq_info.get("high_freq", 8000.0))

    def _update_bypass_btn_style(self):
        if self.is_enabled:
            self.bypass_btn.setText("EQ ACTIVE")
            self.bypass_btn.setStyleSheet("""
                QPushButton {
                    background-color: #082630;
                    color: #00e5ff;
                    border: 1.5px solid #00e5ff;
                    font-family: Consolas, monospace;
                    font-size: 7.5pt;
                    font-weight: bold;
                    padding: 2px 10px;
                }
                QPushButton:hover {
                    background-color: #0d3847;
                    color: #ffffff;
                }
            """)
        else:
            self.bypass_btn.setText("EQ BYPASSED")
            self.bypass_btn.setStyleSheet("""
                QPushButton {
                    background-color: #1a0526;
                    color: #7b2cbf;
                    border: 1px solid #3d145c;
                    font-family: Consolas, monospace;
                    font-size: 7.5pt;
                    padding: 2px 10px;
                }
                QPushButton:hover {
                    background-color: #2b0c47;
                    color: #ffffff;
                }
            """)

    def _on_bypass_toggle(self, checked: bool):
        self.is_enabled = checked
        self._update_bypass_btn_style()
        self.client.set_bus_eq(self.bus_id, self.is_enabled)
        self.canvas.set_parameters(
            self.low_gain, self.low_freq,
            self.mid_gain, self.mid_freq, self.mid_q,
            self.high_gain, self.high_freq, self.is_enabled
        )
        if self.on_state_changed:
            self.on_state_changed(self.is_enabled)

    def _sync_to_engine(self):
        self.client.set_bus_eq_params(
            self.bus_id,
            self.low_gain, self.mid_gain, self.high_gain,
            self.low_freq, self.mid_freq, self.high_freq, self.mid_q
        )
        self.canvas.set_parameters(
            self.low_gain, self.low_freq,
            self.mid_gain, self.mid_freq, self.mid_q,
            self.high_gain, self.high_freq, self.is_enabled
        )

    def _on_low_slider_changed(self, val: int):
        self.low_gain = val / 10.0
        self.low_gain_readout.setText(f"{self.low_gain:+.1f} dB")
        self._sync_to_engine()

    def _on_low_freq_changed(self, val: float):
        self.low_freq = val
        self._sync_to_engine()

    def _on_mid_slider_changed(self, val: int):
        self.mid_gain = val / 10.0
        self.mid_gain_readout.setText(f"{self.mid_gain:+.1f} dB")
        self._sync_to_engine()

    def _on_mid_freq_changed(self, val: float):
        self.mid_freq = val
        self._sync_to_engine()

    def _on_mid_q_changed(self, val: float):
        self.mid_q = val
        self._sync_to_engine()

    def _on_high_slider_changed(self, val: int):
        self.high_gain = val / 10.0
        self.high_gain_readout.setText(f"{self.high_gain:+.1f} dB")
        self._sync_to_engine()

    def _on_high_freq_changed(self, val: float):
        self.high_freq = val
        self._sync_to_engine()

    def _on_node_dragged(self, band_idx: int, gain_db: float, freq_hz: float):
        if band_idx == 0:
            self.low_gain = gain_db
            self.low_freq = freq_hz
            self.low_gain_slider.blockSignals(True)
            self.low_gain_slider.setValue(int(round(gain_db * 10)))
            self.low_gain_slider.blockSignals(False)
            self.low_gain_readout.setText(f"{gain_db:+.1f} dB")
            self.low_freq_knob.set_value(freq_hz)
        elif band_idx == 1:
            self.mid_gain = gain_db
            self.mid_freq = freq_hz
            self.mid_gain_slider.blockSignals(True)
            self.mid_gain_slider.setValue(int(round(gain_db * 10)))
            self.mid_gain_slider.blockSignals(False)
            self.mid_gain_readout.setText(f"{gain_db:+.1f} dB")
            self.mid_freq_knob.set_value(freq_hz)
        elif band_idx == 2:
            self.high_gain = gain_db
            self.high_freq = freq_hz
            self.high_gain_slider.blockSignals(True)
            self.high_gain_slider.setValue(int(round(gain_db * 10)))
            self.high_gain_slider.blockSignals(False)
            self.high_gain_readout.setText(f"{gain_db:+.1f} dB")
            self.high_freq_knob.set_value(freq_hz)

        self._sync_to_engine()

    def _reset_flat(self):
        self.low_gain = 0.0
        self.mid_gain = 0.0
        self.high_gain = 0.0
        self.low_gain_slider.setValue(0)
        self.mid_gain_slider.setValue(0)
        self.high_gain_slider.setValue(0)
        self._sync_to_engine()

    def _reset_frequencies(self):
        self.low_freq = 100.0
        self.mid_freq = 1000.0
        self.mid_q = 1.0
        self.high_freq = 8000.0
        self.low_freq_knob.set_value(100.0)
        self.mid_freq_knob.set_value(1000.0)
        self.mid_q_knob.set_value(1.0)
        self.high_freq_knob.set_value(8000.0)
        self._sync_to_engine()
