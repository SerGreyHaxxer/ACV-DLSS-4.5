@echo off
REM ============================================================================
REM DLSS 4.5 Mod - Quick Installer
REM ============================================================================
REM This launches the PowerShell installer
REM ============================================================================

title DLSS 4.5 Mod Installer

echo.
echo  ============================================
echo   DLSS 4.5 Mod for AC Valhalla
echo  ============================================
echo.
echo  This will build and install the mod automatically.
echo.

REM Check for PowerShell
powershell -Command "Get-Host" >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: PowerShell is required but not available.
    pause
    exit /b 1
)

REM Run the main installer
powershell -ExecutionPolicy Bypass -File "%~dp0BuildAndDeploy.ps1" %*

if %errorlevel% neq 0 (
    echo.
    echo Installation failed. See error messages above.
    pause
    exit /b 1
)

pause
