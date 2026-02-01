# DLSS 4.5 Mod - Automatic Installer
$ErrorActionPreference = "Stop"

Write-Host "==============================================" -ForegroundColor Cyan
Write-Host "   AC Valhalla DLSS 4.5 Mod Installer" -ForegroundColor Cyan
Write-Host "==============================================" -ForegroundColor Cyan

# 1. Verify Build
if (-not (Test-Path "bin\dxgi.dll")) {
    Write-Host "Error: Mod not built!" -ForegroundColor Red
    Write-Host "Please run 'build.bat' first to compile the DLL."
    exit
}

# 2. Find Game Path
$gamePath = $null
$potentialPaths = @(
    "C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla",
    "D:\SteamLibrary\steamapps\common\Assassin's Creed Valhalla",
    "E:\SteamLibrary\steamapps\common\Assassin's Creed Valhalla",
    "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla",
    "D:\Games\Assassin's Creed Valhalla"
)

# Registry Check (Steam)
try {
    $steamPath = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 2208920" -ErrorAction SilentlyContinue
    if ($steamPath) { $potentialPaths += $steamPath.InstallLocation }
} catch {}

# Registry Check (Ubisoft) - varying keys, often hard to rely on reliably, sticking to paths + manual

foreach ($path in $potentialPaths) {
    if (Test-Path "$path\ACValhalla.exe") {
        $gamePath = $path
        break
    }
}

# Manual Input if not found
if (-not $gamePath) {
    Write-Host "Could not automatically find Assassin's Creed Valhalla." -ForegroundColor Yellow
    Write-Host "Please drag and drop 'ACValhalla.exe' into this window and press Enter:" -ForegroundColor Green
    $inputPath = Read-Host
    $inputPath = $inputPath.Trim('"') # Remove quotes if added by drag-drop
    if (Test-Path $inputPath) {
        if ($inputPath -match ".exe$") {
            $gamePath = Split-Path -Parent $inputPath
        } else {
            $gamePath = $inputPath
        }
    }
}

if (-not $gamePath -or -not (Test-Path "$gamePath\ACValhalla.exe")) {
    Write-Host "Error: Game path not found or invalid." -ForegroundColor Red
    exit
}

Write-Host "Found Game: $gamePath" -ForegroundColor Green

# 3. Copy Files
Write-Host "Copying files..."
$files = @("dxgi.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll", "sl.interposer.dll", "sl.reflex.dll")

foreach ($file in $files) {
    $src = "bin\$file"
    if (-not (Test-Path $src)) {
        # Try finding in external folders if not in bin
        $src = "external\streamline\lib\x64\$file"
    }
    
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination "$gamePath\$file" -Force
        Write-Host "  [OK] $file" -ForegroundColor Gray
    } else {
        Write-Host "  [MISSING] $file (Make sure SDK is linked or file is in bin)" -ForegroundColor Yellow
    }
}

# 4. Registry Fix
Write-Host "Applying Registry Fix..."
$regContent = @"
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\NVIDIA Corporation\Global\NGXCore]
"EnableBetaSuperSampling"=dword:00000001
"ShowDlssIndicator"=dword:00000000

[HKEY_CURRENT_USER\Software\NVIDIA Corporation\Global\NGXCore]
"EnableBetaSuperSampling"=dword:00000001
"ShowDlssIndicator"=dword:00000000
"@

$regPath = "$gamePath\EnableNvidiaSigOverride.reg"
$regContent | Out-File -FilePath $regPath -Encoding ASCII
Write-Host "  [OK] Created $regPath" -ForegroundColor Gray

# Attempt auto-import (might ask for admin)
Start-Process "reg" -ArgumentList "import `"$regPath`"" -Verb RunAs -Wait

Write-Host "==============================================" -ForegroundColor Green
Write-Host "   INSTALLATION COMPLETE" -ForegroundColor Green
Write-Host "==============================================" -ForegroundColor Green
Write-Host "1. Launch the game."
Write-Host "2. Press F5 for the DLSS Menu."
Write-Host "3. Check video settings: Scale 50%, Borderless Windowed."
Write-Host ""
Pause
