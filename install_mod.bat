@echo off
cd /d "%~dp0"
:: ============================================================================
:: Assassin's Creed Valhalla - DLSS 4.5 Mod Auto-Installer
:: ============================================================================
:: This script finds the game, builds the mod (if needed), and installs files.
:: ============================================================================

echo ========================================================
echo      DLSS 4.5 ^& Frame Gen Mod Installer for ACV
echo ========================================================
echo.

:: 1. Check for Admin Privileges (File Copying often needs it)
net session >nul 2>&1
if %errorLevel% == 0 (
    echo [OK] Running as Administrator
) else (
    echo [!] Requesting Administrator privileges...
    powershell -Command "Start-Process '%0' -Verb RunAs"
    exit /b
)

:: Ensure we are still in the correct directory after admin elevation
cd /d "%~dp0"

:: 2. Find Game Path (Steam & Ubisoft Connect)
echo.
echo [1/4] Searching for Assassin's Creed Valhalla...
set "GAME_PATH="

:: Try Registry (Steam)
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 2208920" /v InstallLocation 2^>nul') do set "GAME_PATH=%%B"

:: Try Common Paths if Registry fails
if not defined GAME_PATH (
    if exist "C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla\ACValhalla.exe" set "GAME_PATH=C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla"
    if exist "C:\Program Files\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla\ACValhalla.exe" set "GAME_PATH=C:\Program Files\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla"
    if exist "D:\Games\Assassin's Creed Valhalla\ACValhalla.exe" set "GAME_PATH=D:\Games\Assassin's Creed Valhalla"
)

if not defined GAME_PATH (
    echo [ERROR] Game not found automatically!
    set /p "GAME_PATH=Please drag and drop your game folder here and press Enter: "
)

echo [OK] Found Game at: "%GAME_PATH%"

:: 3. Build Mod (if needed)
echo.
echo [2/4] Verifying Mod Files...
if not exist "bin\dxgi.dll" (
    echo [INFO] dxgi.dll not found. Attempting to build...
    powershell -ExecutionPolicy Bypass -File ".\build.ps1"
    if not exist "bin\dxgi.dll" (
        echo [ERROR] Build failed. Please check Visual Studio installation.
        pause
        exit /b
    )
)

:: 4. Install Files
echo.
echo [3/4] Installing Mod Files...

:: Backup
if exist "%GAME_PATH%\dxgi.dll" (
    if not exist "%GAME_PATH%\dxgi.dll.bak" (
        copy "%GAME_PATH%\dxgi.dll" "%GAME_PATH%\dxgi.dll.bak" >nul
        echo [INFO] Backed up original dxgi.dll
    )
)

:: Copy Mod
copy /Y "bin\dxgi.dll" "%GAME_PATH%\" >nul
echo [OK] Copied dxgi.dll (The Proxy)

:: Copy Dependencies (Mocking existence for script completeness)
if exist "sl.interposer.dll" copy /Y "sl.interposer.dll" "%GAME_PATH%\" >nul
if exist "sl.common.dll" copy /Y "sl.common.dll" "%GAME_PATH%\" >nul
if exist "nvngx_dlss.dll" copy /Y "nvngx_dlss.dll" "%GAME_PATH%\" >nul
if exist "nvngx_dlssg.dll" copy /Y "nvngx_dlssg.dll" "%GAME_PATH%\" >nul

:: 5. Verification
echo.
echo [4/4] Verifying Installation...
if exist "%GAME_PATH%\dxgi.dll" (
    echo [SUCCESS] Mod Installed Successfully!
    echo.
    echo ========================================================
    echo HOW TO USE:
    echo 1. Launch Game
    echo 2. Press F5 for Control Panel
    echo 3. Press F6 for FPS Counter
    echo ========================================================
) else (
    echo [ERROR] Installation failed. Could not copy files.
)

pause
