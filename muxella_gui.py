#!/usr/bin/env python3
import os
import sys
from pathlib import Path

# In frozen GUI apps on Windows, stdout/stderr might be None
if sys.stdout is None:
    sys.stdout = open(os.devnull, 'w')
if sys.stderr is None:
    sys.stderr = open(os.devnull, 'w')

# Prevent multiple conflicting GUI instances from fighting over the audio engine pipe
import ctypes
from ctypes import wintypes
ERROR_ALREADY_EXISTS = 183
kernel32 = ctypes.windll.kernel32
CreateMutexW = kernel32.CreateMutexW
CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
CreateMutexW.restype = wintypes.HANDLE
_instance_mutex = CreateMutexW(None, False, "Local\\Muxella_SingleInstance_Mutex")
if kernel32.GetLastError() == ERROR_ALREADY_EXISTS:
    sys.exit(0)

# Ensure project root is in sys.path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from PyQt6.QtWidgets import QApplication
from PyQt6.QtGui import QIcon

# Exercise the frozen Qt imports and platform plugin without starting audio.
if "--smoke-test-qt" in sys.argv:
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    app = QApplication([])
    from PyQt6.QtWidgets import QWidget
    widget = QWidget()
    widget.show()
    app.processEvents()
    result_path = sys.argv[sys.argv.index("--smoke-test-qt") + 1]
    Path(result_path).write_text("QtWidgets and platform plugin OK", encoding="utf-8")
    sys.exit(0)

from gui.theme import apply_accela_theme, get_resource_dir
from gui.main_window import MuxellaMainWindow

def main():
    app = QApplication(sys.argv)
    app.setApplicationName("MUXELLA")

    icon_path = get_resource_dir() / "icons" / "app.ico"
    if icon_path.exists():
        app.setWindowIcon(QIcon(str(icon_path)))

    # Apply the blocky Cyberpunk / Accela theme
    apply_accela_theme(app)

    window = MuxellaMainWindow()
    if "--minimized" in sys.argv or "--tray" in sys.argv:
        window.hide()
    else:
        window.show()

    sys.exit(app.exec())

if __name__ == "__main__":
    main()
