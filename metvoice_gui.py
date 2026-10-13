#!/usr/bin/env python3
import sys
from pathlib import Path
from PyQt6.QtWidgets import QApplication

# Ensure project root is in sys.path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from gui.theme import apply_accela_theme
from gui.main_window import MetVoiceMainWindow

def main():
    app = QApplication(sys.argv)
    app.setApplicationName("MetVoice Cyber Audio Matrix")

    # Apply the blocky Cyberpunk / Accela theme
    apply_accela_theme(app)

    window = MetVoiceMainWindow()
    window.show()

    sys.exit(app.exec())

if __name__ == "__main__":
    main()
