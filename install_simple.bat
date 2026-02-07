@echo off
TITLE AC Valhalla — DLSS 4.5 Mod Installer
COLOR 0A
echo.
echo   ╔══════════════════════════════════════════════════════════╗
echo   ║     AC Valhalla — DLSS 4.5 Mod Installer               ║
echo   ╚══════════════════════════════════════════════════════════╝
echo.
echo   This will automatically find AC Valhalla and install the mod.
echo.

:: Request Admin Privileges (needed for Program Files writes)
net session >nul 2>&1
if %errorLevel% == 0 (
    echo   [OK] Admin privileges confirmed.
) else (
    echo   [..] Requesting Admin privileges...
    powershell -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

echo.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "install.ps1"
echo.
pause
