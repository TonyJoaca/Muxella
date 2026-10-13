import math
from PyQt6.QtCore import Qt, QRectF, QTimer, pyqtSignal, QPointF
from PyQt6.QtGui import QPainter, QColor, QPen, QBrush, QFont
from PyQt6.QtWidgets import QWidget

class BlockyKnob(QWidget):
    """
    Cyberpunk Blocky Rotary Knob.
    Allows click-and-drag up/down to adjust values with arc feedback.
    """
    valueChanged = pyqtSignal(float)

    def __init__(self, title: str = "VAL", min_val: float = 0.0, max_val: float = 10.0,
                 default_val: float = 0.0, step: float = 0.1, unit: str = "", parent=None):
        super().__init__(parent)
        self.title = title
        self.min_val = min_val
        self.max_val = max_val
        self.value = default_val
        self.step = step
        self.unit = unit

        self.setFixedSize(54, 62)
        self._dragging = False
        self._last_y = 0

    def set_value(self, val: float):
        self.value = max(self.min_val, min(val, self.max_val))
        self.update()

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._last_y = event.pos().y()

    def mouseMoveEvent(self, event):
        if self._dragging:
            dy = self._last_y - event.pos().y()
            self._last_y = event.pos().y()
            delta = dy * ((self.max_val - self.min_val) / 80.0)
            self.value = max(self.min_val, min(self.value + delta, self.max_val))
            self.valueChanged.emit(self.value)
            self.update()

    def mouseReleaseEvent(self, event):
        self._dragging = False

    def mouseDoubleClickEvent(self, event):
        # Reset to min or 0
        self.value = 0.0 if (self.min_val <= 0.0 <= self.max_val) else self.min_val
        self.valueChanged.emit(self.value)
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        w = self.width()
        h = self.height()

        # Title at the top
        painter.setPen(QColor("#a855f7"))
        painter.setFont(QFont("Consolas", 7, QFont.Weight.Bold))
        painter.drawText(QRectF(0, 0, w, 12), Qt.AlignmentFlag.AlignCenter, self.title)

        center_x = w / 2.0
        center_y = 28.0
        radius = 14.0

        # Draw outer dial background
        painter.setPen(QPen(QColor("#3b0764"), 2))
        painter.setBrush(QBrush(QColor("#0d0217")))
        painter.drawEllipse(QPointF(center_x, center_y), radius, radius)

        # Arc calculation (240 degree range: from 150 deg down to -30 deg)
        norm = (self.value - self.min_val) / (self.max_val - self.min_val)
        start_angle = 210 * 16
        span_angle = -int(norm * 240 * 16)

        # Arc track
        painter.setPen(QPen(QColor("#4a0e78"), 2.5))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        arc_rect = QRectF(center_x - radius, center_y - radius, radius * 2, radius * 2)
        painter.drawArc(arc_rect, 210 * 16, -240 * 16)

        # Active arc
        painter.setPen(QPen(QColor("#00ff66" if norm > 0 else "#a855f7"), 2.5))
        painter.drawArc(arc_rect, start_angle, span_angle)

        # Needle pointer
        angle_rad = math.radians(210 - norm * 240)
        nx = center_x + math.cos(angle_rad) * (radius - 4)
        ny = center_y - math.sin(angle_rad) * (radius - 4)
        painter.setPen(QPen(QColor("#ffffff"), 1.5))
        painter.drawLine(QPointF(center_x, center_y), QPointF(nx, ny))

        # Value text at the bottom
        val_str = f"{self.value:.1f}{self.unit}"
        painter.setPen(QColor("#e074ff" if norm > 0 else "#8b5cf6"))
        painter.setFont(QFont("Consolas", 7))
        painter.drawText(QRectF(0, h - 14, w, 14), Qt.AlignmentFlag.AlignCenter, val_str)


class BlockyVuMeter(QWidget):
    """
    Segmented LED Block VU Meter with discrete pixel blocks.
    """
    def __init__(self, parent=None, min_db=-60.0, max_db=0.0):
        super().__init__(parent)
        self.min_db = min_db
        self.max_db = max_db

        self.peak_l = -60.0
        self.peak_r = -60.0
        self.hold_l = -60.0
        self.hold_r = -60.0

        self.setMinimumWidth(22)
        self.setMaximumWidth(28)
        self.setMinimumHeight(130)

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

        painter.fillRect(0, 0, w, h, QColor("#08020e"))
        painter.setPen(QPen(QColor("#3b0764"), 1))
        painter.drawRect(0, 0, w - 1, h - 1)

        num_segments = 22
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
                color_lit = QColor("#ff1a53")   # Overload Red
                color_dim = QColor("#29040c")
            elif ratio >= 0.68:
                color_lit = QColor("#ffcc00")   # Warning Yellow
                color_dim = QColor("#2b2200")
            else:
                color_lit = QColor("#00ff66")   # Cyber Green
                color_dim = QColor("#00240d")

            # Left
            if i < active_segs_l or i == hold_idx_l:
                painter.fillRect(2, y, ch_w, seg_h, color_lit)
            else:
                painter.fillRect(2, y, ch_w, seg_h, color_dim)

            # Right
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
        scanline_color = QColor(0, 0, 0, 24)
        h = self.height()
        w = self.width()

        for y in range(0, h, 3):
            painter.fillRect(0, y, w, 1, scanline_color)
