@echo off
setlocal

cd /d "%~dp0"
type nul > random-wave.stop
py .\vibration-control.py --stop

echo.
echo Random wave stop requested.
pause >nul
