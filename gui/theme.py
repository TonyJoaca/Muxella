import sys
from pathlib import Path
from PyQt6.QtGui import QColor, QFont, QFontDatabase, QPalette
from PyQt6.QtWidgets import QApplication

FONT_PIXEL = "Sonic 1 HUD (Font)"
FONT_BODY = "'JetBrains Mono', 'Fira Code', 'Share Tech Mono', 'Chakra Petch', 'Consolas', monospace"

def get_resource_dir() -> Path:
    if getattr(sys, 'frozen', False):
        meipass = getattr(sys, '_MEIPASS', None)
        if meipass and (Path(meipass) / "res").exists():
            return Path(meipass) / "res"
        return Path(sys.executable).parent / "res"
    return Path(__file__).resolve().parent.parent / "res"

# Cyberpunk Synthwave Color Palette (Optimized for High Legibility & Contrast)
ACCENT_PURPLE = "#C084FC"       # Primary vibrant lavender (readable on dark)
ACCENT_GREEN = "#00FF66"        # Cyber neon green (active routes / levels)
ACCENT_CYAN = "#00E5FF"         # High-tech cyan
ACCENT_MAGENTA = "#E879F9"      # Neon magenta
ACCENT_RED = "#FF1A53"          # Mute / Peak overload red

TEXT_PRIMARY = "#F3E8FF"        # Crisp white-lavender for primary text
TEXT_MUTED = "#B388FF"          # Desaturated neon lavender for secondary labels (no more dark violet!)
TEXT_DIM = "#7E58A8"            # Subtle scale marks and inactive indicators

BG_BLACK = "#08020F"            # Deep canvas background
SURFACE_CARD = "#0E051C"        # Physical channel strip card
SURFACE_VIRTUAL = "#05130A"     # Virtual channel strip card
SURFACE_MASTER = "#0D041A"      # Master bus card

BORDER_OUTLINE = "#3B1166"      # Main panel dividers
BORDER_STRIP = "#1E0836"        # Subtle card boundary
BORDER_STRIP_HOVER = "#581C87"  # Focus / hover boundary
BORDER_SUBTLE = "#160526"       # Gutter / divider lines

def load_fonts() -> str:
    font_path = get_resource_dir() / "fonts" / "sonic-1-hud-font.otf"
    if font_path.exists():
        font_id = QFontDatabase.addApplicationFont(str(font_path))
        if font_id != -1:
            families = QFontDatabase.applicationFontFamilies(font_id)
            if families:
                return families[0]
    return "Consolas"

def apply_accela_theme(app: QApplication) -> str:
    pixel_font = load_fonts()

    app.setStyle("Fusion")
    palette = QPalette()

    bg = QColor(BG_BLACK)
    accent = QColor(ACCENT_PURPLE)

    palette.setColor(QPalette.ColorRole.Window, bg)
    palette.setColor(QPalette.ColorRole.WindowText, accent)
    palette.setColor(QPalette.ColorRole.Base, QColor(SURFACE_CARD))
    palette.setColor(QPalette.ColorRole.AlternateBase, bg)
    palette.setColor(QPalette.ColorRole.Text, accent)
    palette.setColor(QPalette.ColorRole.Button, QColor(SURFACE_CARD))
    palette.setColor(QPalette.ColorRole.ButtonText, accent)
    palette.setColor(QPalette.ColorRole.Highlight, accent)
    palette.setColor(QPalette.ColorRole.HighlightedText, bg)

    app.setPalette(palette)

    stylesheet = f"""
    * {{
        font-family: {FONT_BODY};
        font-size: 8.5pt;
    }}

    QMainWindow, QDialog, QWidget#CentralWidget {{
        background-color: {BG_BLACK};
        color: {TEXT_PRIMARY};
    }}

    QLabel {{
        background: transparent;
        border: none;
        margin: 0px;
        padding: 0px;
        color: {TEXT_MUTED};
    }}

    QGroupBox {{
        background-color: transparent;
        border: none;
        margin-top: 4px;
    }}

    QGroupBox::title {{
        subcontrol-origin: margin;
        subcontrol-position: top left;
        padding: 0 4px;
        color: {TEXT_MUTED};
        font-weight: bold;
        font-size: 7pt;
        letter-spacing: 0.5px;
    }}

    QPushButton {{
        background-color: #0d0317;
        color: {TEXT_MUTED};
        border: 1px solid #2d0b4d;
        border-radius: 0px;
        padding: 4px 8px;
        font-weight: bold;
    }}

    QPushButton:hover {{
        background-color: #1f0736;
        color: {TEXT_PRIMARY};
        border: 1px solid {ACCENT_PURPLE};
    }}

    QPushButton:pressed {{
        background-color: #3b0f66;
        color: #ffffff;
    }}

    QPushButton:checked {{
        background-color: #0b2915;
        border: 1.5px solid {ACCENT_GREEN};
        color: {ACCENT_GREEN};
    }}

    /* Vertical Faders */
    QSlider::groove:vertical {{
        background: #0d0417;
        width: 8px;
        border: 1px solid #220838;
    }}

    QSlider::sub-page:vertical {{
        background: #2a0b47;
    }}

    QSlider::add-page:vertical {{
        background: #090212;
    }}

    QSlider::handle:vertical {{
        background: {ACCENT_PURPLE};
        border: 1px solid #ffffff;
        height: 18px;
        margin: 0 -5px;
        border-radius: 0px;
    }}

    QSlider::handle:vertical:hover {{
        background: {ACCENT_CYAN};
        border: 1px solid {ACCENT_GREEN};
    }}

    /* Horizontal Pan Sliders */
    QSlider::groove:horizontal {{
        background: #0d0417;
        height: 6px;
        border: 1px solid #220838;
    }}

    QSlider::handle:horizontal {{
        background: {ACCENT_PURPLE};
        border: 1px solid #ffffff;
        width: 14px;
        margin: -4px 0;
        border-radius: 0px;
    }}

    QSlider::handle:horizontal:hover {{
        background: {ACCENT_GREEN};
        border: 1px solid #ffffff;
    }}

    /* Terminal Output / Log Area */
    QTextEdit, QPlainTextEdit {{
        background-color: #040108;
        color: {ACCENT_PURPLE};
        border: 1px solid {BORDER_OUTLINE};
        border-radius: 0px;
        font-family: {FONT_BODY};
        font-size: 8pt;
        selection-background-color: {ACCENT_PURPLE};
        selection-color: #000000;
    }}

    QScrollBar:vertical {{
        background: #06010d;
        width: 8px;
        border: none;
    }}

    QScrollBar::handle:vertical {{
        background: #2d0b4d;
        min-height: 20px;
    }}

    QScrollBar::handle:vertical:hover {{
        background: {ACCENT_PURPLE};
    }}

    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
        height: 0px;
    }}
    """

    app.setStyleSheet(stylesheet)
    return pixel_font
