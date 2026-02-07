@echo off
TITLE AC Valhalla DLSS Mod Uninstaller
COLOR 0C
ECHO ========================================================
ECHO    AC Valhalla DLSS 4.5 Mod - Uninstaller
ECHO ========================================================
ECHO.
ECHO This script will REMOVE the mod files from the game folder.
ECHO.

:: Request Admin Privileges if needed for Program Files
net session >nul 2>&1
if %errorLevel% == 0 (
    echo [OK] Admin privileges confirmed.
) else (
    echo [INFO] Requesting Admin privileges...
    powershell Start-Process -Verb RunAs -FilePath "%0"
    exit /b
)

set /p gamePath="Enter Game Folder Path (leave empty to try auto-detect): "
if "%gamePath%"=="" (
    echo Attempting auto-detection...
    powershell -Command "$p = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 2208920' -ErrorAction SilentlyContinue).InstallLocation; if ($p) { echo $p } else { echo 'NOT_FOUND' }" > temp_path.txt
    set /p autoPath=<temp_path.txt
    del temp_path.txt
    if "%autoPath%"=="NOT_FOUND" (
        echo Game not found automatically. Please run manually from game folder.
        pause
        exit /b
    )
    set "gamePath=%autoPath%"
)

echo Target: "%gamePath%"
echo Deleting files...
if exist "%gamePath%\dxgi.dll" del "%gamePath%\dxgi.dll" /q
for %%f in ("%gamePath%\sl.*.dll") do del "%%f" /q
for %%f in ("%gamePath%\nvngx_*.dll") do del "%%f" /q
if exist "%gamePath%\dlss_settings.ini" del "%gamePath%\dlss_settings.ini" /q
if exist "%gamePath%\dlss4_proxy.log" del "%gamePath%\dlss4_proxy.log" /q
if exist "%gamePath%\dlss4_crash.log" del "%gamePath%\dlss4_crash.log" /q
if exist "%gamePath%\startup_trace.log" del "%gamePath%\startup_trace.log" /q

echo.
echo [OK] Uninstallation Complete.
pause
