@echo off
cd /d "%~dp0"
echo [1/2] Compiling C++ High-Performance Audio Engine and Virtual Audio Host...
cmake --build build --config Release --target muxella_server muxella_cables
if %errorlevel% neq 0 (
    echo Error during C++ compilation!
    exit /b %errorlevel%
)

echo [2/2] Bundling Single Standalone Muxella.exe...
pyinstaller --noconfirm Muxella.spec
if errorlevel 1 exit /b 1
copy /y dist\Muxella.exe . >nul

echo ========================================================
echo Build complete! Muxella.exe is ready in project root.
echo ========================================================
