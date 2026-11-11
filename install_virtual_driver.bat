@echo off
cd /d "%~dp0"
if exist muxella_cables.exe (
    start "" muxella_cables.exe --install-driver
) else if exist build\muxella_cables.exe (
    start "" build\muxella_cables.exe --install-driver
) else (
    echo Build Muxella first, then use Install virtual audio transport from its menu.
    pause
)
