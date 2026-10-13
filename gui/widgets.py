import math
from PyQt6.QtCore import Qt, QRectF, QTimer
from PyQt6.QtGui import QPainter, QColor, QPen, QBrush
from PyQt6.QtWidgets import QWidget

class BlockyVuMeter(QWidget):
    """
    Segmented LED Block VU Meter.
    Renders discrete cyber-style LED blocks with peak-hold ballistics.
    """
    def __init__(self, parent=None, min_db=-60.0, max_db=0.0):
        super().__init__(parent)
        self.min_db = min_db
        self.max_db = max_db

        self.peak_l = -60.0
        self.peak_r = -60.0
        self.hold_l = -60.0
        self.hold_r = -60.0

        self.setMinimumWidth(32)
        self.setMinimumHeight(120)

        # Decay timer
        self.decay_timer = QTimer(self)
        self.decay_timer.timeout.connect(self._decay)
        self.decay_timer.start(25) # 40 FPS

    def set_levels(self, peak_l_db: float, peak_r_db: float):
        self.peak_l = max(self.min_db, min(peak_l_db, self.max_db))
        self.peak_r = max(self.min_db, min(peak_r_db, self.max_db))
        if self.peak_l > self.hold_l:
            self.hold_l = self.peak_l
        if self.peak_r > self.hold_r:
            self.hold_r = self.peak_r
        self.update()

    def _decay(self):
        # -1.5 dB per frame decay for hold
        self.hold_l = max(self.min_db, self.hold_l - 0.8)
        self.hold_r = max(self.min_db, self.hold_r - 0.8)
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False) # Sharp crisp pixels!

        w = self.width()
        h = self.height()

        # Background
        painter.fillRect(0, 0, w, h, QColor("#08020e"))
        painter.setPen(QPen(QColor("#4a1469"), 1))
        painter.drawRect(0, 0, w - 1, h - 1)

        num_segments = 24
        segment_gap = 2
        total_gaps = (num_segments - 1) * segment_gap
        seg_h = max(2, (h - 8 - total_gaps) // num_segments)
        ch_w = (w - 10) // 2

        def db_to_norm(db):
            return max(0.0, min(1.0, (db - self.min_db) / (self.max_db - self.min_db)))

        norm_l = db_to_norm(self.peak_l)
        norm_r = db_to_norm(self.peak_r)
        norm_hold_l = db_to_norm(self.hold_l)
        norm_hold_r = db_to_norm(self.hold_r)

        active_segs_l = int(norm_l * num_segments)
        active_segs_r = int(norm_r * num_segments)
        hold_idx_l = int(norm_hold_l * num_segments)
        hold_idx_r = int(norm_hold_r * num_segments)

        for i in range(num_segments):
            # i = 0 is bottom, num_segments - 1 is top
            y = h - 5 - (i + 1) * seg_h - i * segment_gap

            # Color gradient: bottom (green/purple), mid (yellow), top (red)
            ratio = i / float(num_segments)
            if ratio >= 0.85:
                color_lit = QColor("#ff1a53")   # Overload Red
                color_dim = QColor("#330510")
            elif ratio >= 0.65:
                color_lit = QColor("#ffcc00")   # Warning Yellow
                color_dim = QColor("#332800")
            else:
                color_lit = QColor("#00ff66")   # Cyber Green
                color_dim = QColor("#00290e")

            # Left channel
            if i < active_segs_l or i == hold_idx_l:
                painter.fillRect(3, y, ch_w, seg_h, color_lit)
            else:
                painter.fillRect(3, y, ch_w, seg_h, color_dim)

            # Right channel
            if i < active_segs_r or i == hold_idx_r:
                painter.fillRect(w - 3 - ch_w, y, ch_w, seg_h, color_lit)
            else:
                painter.fillRect(w - 3 - ch_w, y, ch_w, seg_h, color_dim)


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
        scanline_color = QColor(0, 0, 0, 32)
        h = self.height()
        w = self.width()

        for y in range(0, h, 3):
            painter.fillRect(0, y, w, 1, scanline_color)
