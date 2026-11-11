import math
from PyQt6.QtCore import Qt, QRectF, QTimer, pyqtSignal, QPointF
from PyQt6.QtGui import QPainter, QColor, QPen, QBrush, QFont
from PyQt6.QtWidgets import (
    QWidget, QSlider, QDialog, QVBoxLayout, QHBoxLayout, QLabel, QLineEdit, QPushButton
)


class CyberValuePromptDialog(QDialog):
    """
    Sleek Cyberpunk floating dialog for numeric keyboard input on right-click.
    Auto-focuses with the current value pre-selected.
    """
    def __init__(self, title: str, current_val: float, min_val: float, max_val: float, unit: str = "", parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"SET {title}")
        self.setWindowFlags(Qt.WindowType.Dialog | Qt.WindowType.FramelessWindowHint)
        self.min_val = min_val
        self.max_val = max_val
        self.unit = unit
        self.result_value = None

        self.setStyleSheet("""
            QDialog {
                background-color: #0c0317;
                border: 1.5px solid #00ff66;
            }
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(8)

        unit_str = f" {unit}" if unit else ""
        hdr = QLabel(f"// SET {title} [{min_val:.1f} .. {max_val:.1f}{unit_str}]")
        hdr.setStyleSheet("color: #b388ff; font-size: 7.5pt; font-weight: bold; font-family: Consolas, monospace;")
        layout.addWidget(hdr)

        self.edit = QLineEdit(f"{current_val:.1f}")
        self.edit.setStyleSheet("""
            QLineEdit {
                background-color: #05010a;
                color: #00ff66;
                border: 1px solid #c084fc;
                font-family: Consolas, monospace;
                font-size: 11pt;
                font-weight: bold;
                padding: 4px 6px;
                text-align: center;
            }
            QLineEdit:focus {
                border: 1.5px solid #00ff66;
            }
        """)
        self.edit.selectAll()
        self.edit.returnPressed.connect(self._accept_value)
        layout.addWidget(self.edit)

        btn_row = QHBoxLayout()
        btn_row.setSpacing(6)

        ok_btn = QPushButton("OK (Enter)")
        ok_btn.setFixedHeight(22)
        ok_btn.setStyleSheet("""
            QPushButton {
                background-color: #0b2915;
                color: #00ff66;
                border: 1px solid #00ff66;
                font-weight: bold;
                font-size: 7.5pt;
                padding: 2px 8px;
            }
            QPushButton:hover {
                background-color: #124d26;
                color: #ffffff;
            }
        """)
        ok_btn.clicked.connect(self._accept_value)
        btn_row.addWidget(ok_btn)

        cancel_btn = QPushButton("CANCEL (Esc)")
        cancel_btn.setFixedHeight(22)
        cancel_btn.setStyleSheet("""
            QPushButton {
                background-color: #1a0526;
                color: #b388ff;
                border: 1px solid #4a1469;
                font-size: 7.5pt;
                padding: 2px 8px;
            }
            QPushButton:hover {
                background-color: #330b4d;
                color: #ffffff;
            }
        """)
        cancel_btn.clicked.connect(self.reject)
        btn_row.addWidget(cancel_btn)

        layout.addLayout(btn_row)

    def _accept_value(self):
        try:
            txt = self.edit.text().replace(self.unit, "").replace("dB", "").replace("%", "").strip()
            val = float(txt)
            self.result_value = max(self.min_val, min(val, self.max_val))
            self.accept()
        except ValueError:
            self.edit.setStyleSheet("""
                QLineEdit {
                    background-color: #22030a;
                    color: #ff1a53;
                    border: 1.5px solid #ff1a53;
                    font-family: Consolas, monospace;
                    font-size: 11pt;
                    font-weight: bold;
                    padding: 4px 6px;
                }
            """)

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.reject()
        else:
            super().keyPressEvent(event)


class CyberSlider(QSlider):
    """
    Vertical or Horizontal Slider with Cyberpunk styling:
    - Double click: Resets to default value
    - Right click: Opens CyberValuePromptDialog for precision keyboard entry
    """
    def __init__(self, orientation=Qt.Orientation.Vertical, default_value: int = 0,
                 title: str = "VAL", min_display: float = -60.0, max_display: float = 12.0,
                 display_scale: float = 0.1, unit: str = "dB", parent=None):
        super().__init__(orientation, parent)
        self.default_value = default_value
        self.title = title
        self.min_display = min_display
        self.max_display = max_display
        self.display_scale = display_scale
        self.unit = unit

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setValue(self.default_value)
            event.accept()
        else:
            super().mouseDoubleClickEvent(event)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            cur_display = self.value() * self.display_scale
            dlg = CyberValuePromptDialog(self.title, cur_display, self.min_display, self.max_display, self.unit, self)
            dlg.move(event.globalPosition().toPoint())
            if dlg.exec():
                if dlg.result_value is not None:
                    raw_val = int(round(dlg.result_value / self.display_scale))
                    self.setValue(raw_val)
            event.accept()
        else:
            super().mousePressEvent(event)


class DbScaleWidget(QWidget):
    """
    Subtle technical dB markings (+12, 0, -10, -20, -40, -inf)
    alongside vertical faders and peak meters.
    """
    def __init__(self, parent=None, min_db: float = -60.0, max_db: float = 12.0):
        super().__init__(parent)
        self.min_db = min_db
        self.max_db = max_db
        self.setFixedWidth(20)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        h = self.height()
        w = self.width()

        markings = [
            (12.0, "+12"),
            (0.0, " 0"),
            (-10.0, "-10"),
            (-20.0, "-20"),
            (-40.0, "-40"),
            (-60.0, "-inf")
        ]

        painter.setFont(QFont("Consolas", 6))

        for db, text in markings:
            norm = (db - self.min_db) / (self.max_db - self.min_db)
            y = int(h - 10 - norm * (h - 20))

            painter.setPen(QPen(QColor("#4A1469"), 1))
            painter.drawLine(0, y, 4, y)

            painter.setPen(QColor("#00ff66" if db == 0.0 else "#8f6ba3"))
            painter.drawText(QRectF(5, y - 5, w - 5, 10), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, text)


class BlockyKnob(QWidget):
    """
    Cyberpunk Rotary Knob with circular track ring, bright cyan/green indicator:
    - Drag Up/Down: Smooth fine adjustment
    - Double click: Resets to default value
    - Right click: Opens CyberValuePromptDialog for precision keyboard entry
    """
    valueChanged = pyqtSignal(float)

    def __init__(self, title: str = "VAL", min_val: float = 0.0, max_val: float = 10.0,
                 default_val: float = 0.0, step: float = 0.1, unit: str = "", parent=None):
        super().__init__(parent)
        self.title = title
        self.min_val = min_val
        self.max_val = max_val
        self.default_val = default_val
        self.value = default_val
        self.step = step
        self.unit = unit

        self.setFixedSize(46, 52)
        self.setToolTip(f"{title}: Double-click to reset, Right-click to enter value")
        self._dragging = False
        self._last_y = 0

    def set_value(self, val: float):
        self.value = max(self.min_val, min(val, self.max_val))
        self.update()

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            dlg = CyberValuePromptDialog(self.title, self.value, self.min_val, self.max_val, self.unit, self)
            dlg.move(event.globalPosition().toPoint())
            if dlg.exec():
                if dlg.result_value is not None:
                    self.set_value(dlg.result_value)
                    self.valueChanged.emit(self.value)
            event.accept()
        elif event.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._last_y = event.pos().y()

    def mouseMoveEvent(self, event):
        if self._dragging:
            dy = self._last_y - event.pos().y()
            self._last_y = event.pos().y()
            delta = dy * ((self.max_val - self.min_val) / 90.0)
            self.value = max(self.min_val, min(self.value + delta, self.max_val))
            self.valueChanged.emit(self.value)
            self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = False

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.value = self.default_val
            self.valueChanged.emit(self.value)
            self.update()
            event.accept()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        w = self.width()
        h = self.height()

        painter.setPen(QColor("#B388FF"))
        painter.setFont(QFont("Consolas", 6, QFont.Weight.Bold))
        painter.drawText(QRectF(0, 0, w, 11), Qt.AlignmentFlag.AlignCenter, self.title)

        center_x = w / 2.0
        center_y = 25.0
        radius = 12.0

        painter.setPen(QPen(QColor("#27083E"), 1.5))
        painter.setBrush(QBrush(QColor("#0A0314")))
        painter.drawEllipse(QPointF(center_x, center_y), radius, radius)

        norm = (self.value - self.min_val) / (self.max_val - self.min_val) if self.max_val > self.min_val else 0.0
        start_angle = 210 * 16
        span_angle = -int(norm * 240 * 16)

        painter.setPen(QPen(QColor("#2E0D4A"), 2.0))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        arc_rect = QRectF(center_x - radius, center_y - radius, radius * 2, radius * 2)
        painter.drawArc(arc_rect, 210 * 16, -240 * 16)

        active_color = QColor("#00FF66" if norm > 0 else "#C084FC")
        painter.setPen(QPen(active_color, 2.2))
        painter.drawArc(arc_rect, start_angle, span_angle)

        angle_rad = math.radians(210 - norm * 240)
        nx = center_x + math.cos(angle_rad) * (radius - 3.0)
        ny = center_y - math.sin(angle_rad) * (radius - 3.0)
        painter.setPen(QPen(QColor("#FFFFFF"), 1.5))
        painter.drawLine(QPointF(center_x, center_y), QPointF(nx, ny))

        painter.setBrush(QBrush(QColor("#1A072B")))
        painter.setPen(QPen(QColor("#00E5FF" if norm > 0 else "#6B21A8"), 1))
        painter.drawEllipse(QPointF(center_x, center_y), 3.0, 3.0)

        val_str = f"{self.value:.1f}{self.unit}"
        painter.setPen(QColor("#F3E8FF" if norm > 0 else "#8F6BA3"))
        painter.setFont(QFont("Consolas", 6))
        painter.drawText(QRectF(0, h - 12, w, 12), Qt.AlignmentFlag.AlignCenter, val_str)


class BlockyVuMeter(QWidget):
    """
    Elongated Segmented LED Block VU Meter with discrete pixel blocks.
    Extended height (160-220px) for professional DAW mixing console feel.
    """
    def __init__(self, parent=None, min_db=-60.0, max_db=0.0):
        super().__init__(parent)
        self.min_db = min_db
        self.max_db = max_db

        self.peak_l = -60.0
        self.peak_r = -60.0
        self.hold_l = -60.0
        self.hold_r = -60.0

        self.setMinimumWidth(16)
        self.setMaximumWidth(20)
        self.setMinimumHeight(160)

        self.decay_timer = QTimer(self)
        self.decay_timer.timeout.connect(self._decay)
        self.decay_timer.start(25)

    def set_levels(self, peak_l_db: float, peak_r_db: float):
        self.peak_l = max(self.min_db, min(peak_l_db, self.max_db))
        self.peak_r = max(self.min_db, min(peak_r_db, self.max_db))
        if self.peak_l > self.hold_l:
            self.hold_l = self.peak_l
        if self.peak_r > self.hold_r:
            self.hold_r = self.peak_r
        self.update()

    def _decay(self):
        self.hold_l = max(self.min_db, self.hold_l - 0.7)
        self.hold_r = max(self.min_db, self.hold_r - 0.7)
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)

        w = self.width()
        h = self.height()

        painter.fillRect(0, 0, w, h, QColor("#08020E"))
        painter.setPen(QPen(QColor("#24073A"), 1))
        painter.drawRect(0, 0, w - 1, h - 1)

        num_segments = 28
        segment_gap = 2
        total_gaps = (num_segments - 1) * segment_gap
        seg_h = max(2, (h - 6 - total_gaps) // num_segments)
        ch_w = (w - 7) // 2

        def db_to_norm(db):
            return max(0.0, min(1.0, (db - self.min_db) / (self.max_db - self.min_db)))

        norm_l = db_to_norm(self.peak_l)
        norm_r = db_to_norm(self.peak_r)
        active_segs_l = int(norm_l * num_segments)
        active_segs_r = int(norm_r * num_segments)
        hold_idx_l = int(db_to_norm(self.hold_l) * num_segments)
        hold_idx_r = int(db_to_norm(self.hold_r) * num_segments)

        for i in range(num_segments):
            y = h - 4 - (i + 1) * seg_h - i * segment_gap
            ratio = i / float(num_segments)

            if ratio >= 0.88:
                color_lit = QColor("#FF1A53")
                color_dim = QColor("#22030A")
            elif ratio >= 0.68:
                color_lit = QColor("#FFCC00")
                color_dim = QColor("#221B00")
            else:
                color_lit = QColor("#00FF66")
                color_dim = QColor("#001F0A")

            if i < active_segs_l or i == hold_idx_l:
                painter.fillRect(2, y, ch_w, seg_h, color_lit)
            else:
                painter.fillRect(2, y, ch_w, seg_h, color_dim)

            if i < active_segs_r or i == hold_idx_r:
                painter.fillRect(w - 2 - ch_w, y, ch_w, seg_h, color_lit)
            else:
                painter.fillRect(w - 2 - ch_w, y, ch_w, seg_h, color_dim)


class CrtScanlineOverlay(QWidget):
    """
    Subtle CRT Scanline overlay widget for retro terminal authenticity.
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)

    def paintEvent(self, event):
        painter = QPainter(self)
        scanline_color = QColor(0, 0, 0, 18)
        h = self.height()
        w = self.width()

        for y in range(0, h, 3):
            painter.fillRect(0, y, w, 1, scanline_color)


class CyberEqButton(QPushButton):
    """
    Dedicated EQ toggle button:
    - Left-click: Toggles EQ enabled state
    - Right-click: Emits rightClicked to open Parametric Equalizer window
    """
    rightClicked = pyqtSignal()

    def __init__(self, text="EQ", parent=None):
        super().__init__(text, parent)
        self.setToolTip("Click: Toggle EQ on/off | Right-Click: Open Equalizer Window")

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            self.rightClicked.emit()
            event.accept()
        else:
            super().mousePressEvent(event)
