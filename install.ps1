# DLSS 4.5 Mod - Automatic Installer
$ErrorActionPreference = "Stop"

Write-Host "==============================================" -ForegroundColor Cyan
Write-Host "   AC Valhalla DLSS 4.5 Mod Installer" -ForegroundColor Cyan
Write-Host "==============================================" -ForegroundColor Cyan

# 1. Verify Mod Binary
if (-not (Test-Path "bin\dxgi.dll")) {
    Write-Host "Warning: 'bin\dxgi.dll' not found." -ForegroundColor Yellow
    Write-Host "Attempting to use pre-compiled release..."
    # In a real scenario, this would be the commit check. 
    # For now, we assume if it's missing, the user might need to build, but we try to avoid that.
    Write-Host "Error: dxgi.dll is missing! Did you download the full release?" -ForegroundColor Red
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

# 3. Locate SDK Files (Auto-Search Downloads)
$sdkSource = $null
$downloads = "$env:USERPROFILE\Downloads"
$sdkDlls = @("sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll", "sl.dlss_d.dll", "sl.deepdvc.dll", "sl.interposer.dll", "sl.reflex.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssd.dll", "nvngx_deepdvc.dll")

# Prefer explicit SDK bin if supplied by build script
if ($env:DLSS4_SDK_BIN -and (Test-Path "$env:DLSS4_SDK_BIN\sl.interposer.dll")) {
    $sdkSource = $env:DLSS4_SDK_BIN
}
# Check bin first (ensure DeepDVC bits are present)
elseif ((Test-Path "bin\sl.interposer.dll") -and (Test-Path "bin\nvngx_dlss.dll") -and (Test-Path "bin\sl.deepdvc.dll") -and (Test-Path "bin\nvngx_deepdvc.dll")) {
    $sdkSource = "bin"
} 
# Check external
elseif (Test-Path "external\streamline\lib\x64\sl.interposer.dll") {
    $sdkSource = "external\streamline\lib\x64"
}
# Check Downloads
else {
    Write-Host "Searching for Streamline SDK in Downloads..."
    $sdkZips = Get-ChildItem "$downloads\streamline-sdk-v*.zip" | Sort-Object LastWriteTime -Descending
    $sdkFolders = Get-ChildItem "$downloads" -Directory | Where-Object { $_.Name -like "streamline-sdk-v*" } | Sort-Object LastWriteTime -Descending

    if ($sdkFolders) {
        $found = $sdkFolders[0].FullName + "\lib\x64"
        if (Test-Path "$found\sl.interposer.dll") { $sdkSource = $found }
        if (-not $sdkSource) {
            $found = $sdkFolders[0].FullName + "\bin\x64"
            if (Test-Path "$found\sl.interposer.dll") { $sdkSource = $found }
        }
    }
    
    if (-not $sdkSource -and $sdkZips) {
        Write-Host "Found ZIP: $($sdkZips[0].Name). Extracting..."
        Expand-Archive -Path $sdkZips[0].FullName -DestinationPath "$downloads\temp_sdk_extract" -Force
        $sdkSource = "$downloads\temp_sdk_extract\lib\x64"
        if (-not (Test-Path "$sdkSource\sl.interposer.dll")) {
             # Try nested folder structure
             $nested = Get-ChildItem "$downloads\temp_sdk_extract" -Directory
             if ($nested) { $sdkSource = "$downloads\temp_sdk_extract\" + $nested[0].Name + "\lib\x64" }
        }
        if (-not (Test-Path "$sdkSource\sl.interposer.dll")) {
             $sdkSource = "$downloads\temp_sdk_extract\bin\x64"
        }
        if (-not (Test-Path "$sdkSource\sl.interposer.dll")) {
             # Try nested bin\x64
             $nested = Get-ChildItem "$downloads\temp_sdk_extract" -Directory
             if ($nested) { $sdkSource = "$downloads\temp_sdk_extract\" + $nested[0].Name + "\bin\x64" }
        }
    }
}

if (-not $sdkSource) {
    Write-Host "Error: NVIDIA Streamline SDK not found!" -ForegroundColor Red
    Write-Host "1. Download it from: https://developer.nvidia.com/rtx/streamline"
    Write-Host "2. Put the ZIP or folder in your Downloads folder."
    Write-Host "3. Run this installer again."
    Pause
    exit
}

Write-Host "Using SDK files from: $sdkSource" -ForegroundColor Gray

# 4. Copy Files
Write-Host "Copying files to game folder..."

# Copy Main Mod
Copy-Item -Path "bin\dxgi.dll" -Destination "$gamePath\dxgi.dll" -Force
Write-Host "  [OK] dxgi.dll (Mod)" -ForegroundColor Green

# Copy SDK DLLs
foreach ($file in $sdkDlls) {
    $srcFile = "$sdkSource\$file"
    if (Test-Path $srcFile) {
        Copy-Item -Path $srcFile -Destination "$gamePath\$file" -Force
        Write-Host "  [OK] $file" -ForegroundColor Gray
    } else {
        Write-Host "  [MISSING] $file" -ForegroundColor Red
    }
}

# 5. Registry Fix
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
Write-Host "2. Press your Menu hotkey (default F5) for the DLSS Menu."
Write-Host "3. Check video settings: Scale 50%, Borderless Windowed."
Write-Host ""
if (-not $env:DLSS4_SKIP_PAUSE) {
    Pause
}
