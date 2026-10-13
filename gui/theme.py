from pathlib import Path
from PyQt6.QtGui import QColor, QFont, QFontDatabase, QPalette
from PyQt6.QtWidgets import QApplication

FONT_FAMILY = "Sonic 1 HUD (Font)"
ACCENT_PURPLE = "#b829ea"
ACCENT_GREEN = "#00ff66"
BG_BLACK = "#050208"
SURFACE_CARD = "#0d0514"
BORDER_OUTLINE = "#8a1ec4"
BORDER_SUBTLE = "#43125e"

def load_fonts() -> str:
    font_path = Path(__file__).resolve().parent.parent / "res" / "fonts" / "sonic-1-hud-font.otf"
    if font_path.exists():
        font_id = QFontDatabase.addApplicationFont(str(font_path))
        if font_id != -1:
            families = QFontDatabase.applicationFontFamilies(font_id)
            if families:
                return families[0]
    return "Consolas"

def apply_accela_theme(app: QApplication) -> str:
    font_family = load_fonts()

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
        font-family: '{font_family}', 'Consolas', monospace;
        font-size: 10pt;
    }}

    QMainWindow, QDialog, QWidget#CentralWidget {{
        background-color: {BG_BLACK};
        color: {ACCENT_PURPLE};
    }}

    QFrame, QGroupBox {{
        background-color: {SURFACE_CARD};
        border: 1.5px solid {BORDER_OUTLINE};
        border-radius: 0px;
        margin-top: 6px;
    }}

    QGroupBox::title {{
        subcontrol-origin: margin;
        subcontrol-position: top left;
        padding: 0 4px;
        color: {ACCENT_PURPLE};
        font-weight: bold;
    }}

    QPushButton {{
        background-color: {SURFACE_CARD};
        color: {ACCENT_PURPLE};
        border: 1.5px solid {BORDER_OUTLINE};
        border-radius: 0px;
        padding: 5px 12px;
        font-weight: bold;
    }}

    QPushButton:hover {{
        background-color: #230836;
        color: #e074ff;
        border: 1.5px solid #d45eff;
    }}

    QPushButton:pressed {{
        background-color: #4a136e;
        color: #ffffff;
    }}

    QPushButton:checked {{
        background-color: #3f105e;
        border: 1.5px solid {ACCENT_GREEN};
        color: {ACCENT_GREEN};
    }}

    /* Custom Cyber Sliders */
    QSlider::groove:vertical {{
        background: #140721;
        width: 10px;
        border: 1px solid {BORDER_SUBTLE};
    }}

    QSlider::sub-page:vertical {{
        background: {BORDER_OUTLINE};
    }}

    QSlider::add-page:vertical {{
        background: #1a0a2a;
    }}

    QSlider::handle:vertical {{
        background: {ACCENT_PURPLE};
        border: 1px solid #ffffff;
        height: 16px;
        margin: 0 -4px;
        border-radius: 0px;
    }}

    QSlider::handle:vertical:hover {{
        background: #e074ff;
        border: 1px solid {ACCENT_GREEN};
    }}

    QSlider::groove:horizontal {{
        background: #140721;
        height: 8px;
        border: 1px solid {BORDER_SUBTLE};
    }}

    QSlider::handle:horizontal {{
        background: {ACCENT_PURPLE};
        border: 1px solid #ffffff;
        width: 14px;
        margin: -4px 0;
        border-radius: 0px;
    }}

    QSlider::handle:horizontal:hover {{
        background: #e074ff;
        border: 1px solid {ACCENT_GREEN};
    }}

    /* Terminal Output / Log Area */
    QTextEdit, QPlainTextEdit {{
        background-color: #030105;
        color: #d15fff;
        border: 1.5px solid {BORDER_OUTLINE};
        border-radius: 0px;
        font-family: 'Consolas', monospace;
        font-size: 9.5pt;
        selection-background-color: {ACCENT_PURPLE};
        selection-color: #000000;
    }}

    QScrollBar:vertical {{
        background: #08020d;
        width: 10px;
        border: 1px solid {BORDER_SUBTLE};
    }}

    QScrollBar::handle:vertical {{
        background: {BORDER_OUTLINE};
        min-height: 20px;
    }}

    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
        height: 0px;
    }}

    QLabel {{
        color: {ACCENT_PURPLE};
    }}

    QCheckBox {{
        color: {ACCENT_PURPLE};
        spacing: 6px;
        font-weight: bold;
    }}

    QCheckBox::indicator {{
        width: 14px;
        height: 14px;
        background: #0d0514;
        border: 1.5px solid {BORDER_OUTLINE};
        border-radius: 0px;
    }}

    QCheckBox::indicator:checked {{
        background: {ACCENT_GREEN};
        border: 1.5px solid {ACCENT_GREEN};
    }}
    """

    app.setStyleSheet(stylesheet)
    return font_family
