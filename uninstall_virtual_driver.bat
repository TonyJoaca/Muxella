@echo off
echo ========================================================
echo  Muxella Device Restore
echo ========================================================
echo.
echo Requesting Administrator privileges to restore names...
powershell -Command "Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0restore_original_devices.ps1\"' -Verb RunAs -Wait"
echo.
echo Restore finished.
pause
