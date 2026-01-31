# ============================================================================
# DLSS 4 Proxy - Installation Helper
# ============================================================================
# This script helps you set up everything needed to run the DLSS 4 proxy.
# ============================================================================

param(
    [string]$GamePath = "",
    [switch]$Help
)

# Configuration
$StreamlineSDK = "C:\Users\serge\Downloads\streamline-sdk-v2.10.3"

if ($Help) {
    Write-Host @"
DLSS 4 Proxy Installation Helper

Usage:
    .\install.ps1 -GamePath "C:\Games\AC Valhalla"
    
This script will:
    1. Build the proxy DLL (if not already built)
    2. Copy dxgi.dll to the game folder
    3. Install Streamline SDK components from Downloads
    4. Check for required NVIDIA DLLs
"@
    exit 0
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "DLSS 4 Proxy Installation Helper" -ForegroundColor Cyan  
Write-Host "============================================" -ForegroundColor Cyan

# Step 1: Check if DLL is built
$dllPath = "bin\dxgi.dll"
if (-not (Test-Path $dllPath)) {
    Write-Host "`nBuilding DLL first..." -ForegroundColor Yellow
    & .\build.ps1
    
    if (-not (Test-Path $dllPath)) {
        Write-Host "Build failed! Cannot continue." -ForegroundColor Red
        exit 1
    }
}

Write-Host "`n[OK] dxgi.dll found at: $dllPath" -ForegroundColor Green

# Step 2: Get game path if not provided
if (-not $GamePath) {
    # Try to find AC Valhalla automatically
    $commonPaths = @(
        "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla",
        "C:\Program Files\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla",
        "D:\Games\Assassin's Creed Valhalla",
        "E:\Games\Assassin's Creed Valhalla",
        "C:\Games\Assassin's Creed Valhalla"
    )
    
    foreach ($path in $commonPaths) {
        if (Test-Path "$path\ACValhalla.exe") {
            $GamePath = $path
            Write-Host "`n[FOUND] AC Valhalla at: $GamePath" -ForegroundColor Green
            break
        }
    }
    
    if (-not $GamePath) {
        Write-Host "`nCould not find AC Valhalla automatically." -ForegroundColor Yellow
        $GamePath = Read-Host "Enter the game folder path (containing ACValhalla.exe)"
    }
}

# Validate game path
if (-not (Test-Path $GamePath)) {
    Write-Host "ERROR: Path does not exist: $GamePath" -ForegroundColor Red
    exit 1
}

$exePath = Join-Path $GamePath "ACValhalla.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "WARNING: ACValhalla.exe not found in $GamePath" -ForegroundColor Yellow
    Write-Host "Continuing anyway..." -ForegroundColor Yellow
}

# Step 3: Copy our DLL
Write-Host "`nInstalling proxy DLL..." -ForegroundColor Yellow
$destDll = Join-Path $GamePath "dxgi.dll"

if (Test-Path $destDll) {
    $backup = Join-Path $GamePath "dxgi.dll.backup"
    Write-Host "Backing up existing dxgi.dll to dxgi.dll.backup" -ForegroundColor Yellow
    Copy-Item $destDll $backup -Force
}

Copy-Item $dllPath $destDll -Force
Write-Host "[OK] Installed dxgi.dll" -ForegroundColor Green

# Step 3.5: Install Streamline SDK
Write-Host "`nInstalling Streamline SDK..." -ForegroundColor Yellow

if (Test-Path $StreamlineSDK) {
    # 1. Copy Interposer
    $slInterposer = "$StreamlineSDK\bin\x64\sl.interposer.dll"
    if (Test-Path $slInterposer) {
        Copy-Item $slInterposer "$GamePath\sl.interposer.dll" -Force
        Write-Host "[OK] Copied sl.interposer.dll" -ForegroundColor Green
    } else {
        Write-Host "[MISSING] sl.interposer.dll not found in SDK" -ForegroundColor Red
    }
    
    # 2. Copy Plugins
    # We will copy plugins to root to ensure loading, as pref.numPathsToPlugins=0
    # or to 'plugins' if we assume standard loader behavior.
    # Safe bet: Copy to root for this simple proxy integration if loader supports it,
    # BUT standard Streamline requires them in specific paths if not configured.
    # Given we pass 0 paths, it looks alongside interposer usually.
    
    $slPluginsSrc = "$StreamlineSDK\bin\x64\plugins"
    if (Test-Path $slPluginsSrc) {
        $dlssPlugin = "$slPluginsSrc\sl.dlss.dll"
        $dlssgPlugin = "$slPluginsSrc\sl.dlss_g.dll"
        
        if (Test-Path $dlssPlugin) {
            Copy-Item $dlssPlugin "$GamePath\sl.dlss.dll" -Force
            Write-Host "[OK] Copied sl.dlss.dll" -ForegroundColor Green
        }
        if (Test-Path $dlssgPlugin) {
            Copy-Item $dlssgPlugin "$GamePath\sl.dlss_g.dll" -Force
            Write-Host "[OK] Copied sl.dlss_g.dll" -ForegroundColor Green
        }
    }
    
    # 3. Copy Common (if needed)
    $slCommon = "$StreamlineSDK\bin\x64\sl.common.dll"
    if (Test-Path $slCommon) {
        Copy-Item $slCommon "$GamePath\sl.common.dll" -Force
        Write-Host "[OK] Copied sl.common.dll" -ForegroundColor Green
    }
    
} else {
    Write-Host "[WARNING] Streamline SDK not found at $StreamlineSDK" -ForegroundColor Red
    Write-Host "You must manually copy sl.interposer.dll and plugins to the game folder." -ForegroundColor Yellow
}

# Step 4: Check for NVIDIA DLLs
Write-Host "`nChecking for NVIDIA DLSS DLLs..." -ForegroundColor Yellow

$dlssDll = Join-Path $GamePath "nvngx_dlss.dll"
$dlssgDll = Join-Path $GamePath "nvngx_dlssg.dll"

$dlssOk = Test-Path $dlssDll
$dlssgOk = Test-Path $dlssgDll

if ($dlssOk) {
    Write-Host "[OK] nvngx_dlss.dll found" -ForegroundColor Green
} else {
    Write-Host "[MISSING] nvngx_dlss.dll" -ForegroundColor Red
}

if ($dlssgOk) {
    Write-Host "[OK] nvngx_dlssg.dll found (Frame Gen ready)" -ForegroundColor Green
} else {
    Write-Host "[MISSING] nvngx_dlssg.dll (Frame Gen will be disabled)" -ForegroundColor Yellow
}

# Step 5: Instructions for missing DLLs
if (-not $dlssOk -or -not $dlssgOk) {
    Write-Host @"

============================================
IMPORTANT: Missing NVIDIA DLLs
============================================

To get the required NVIDIA DLLs:

Option 1: Copy from another DLSS 4 game
    - Look in games like Cyberpunk 2077, Alan Wake 2, etc.
    - Find nvngx_dlss.dll (usually in game folder)
    - For Frame Gen: nvngx_dlssg.dll

Option 2: NVIDIA Developer Portal
    - Visit: https://developer.nvidia.com/rtx/dlss
    - Download DLSS SDK
    - Extract the DLLs

Option 3: TechPowerUp DLSS Archive
    - https://www.techpowerup.com/download/nvidia-dlss-dll/
    - Download latest version
    - Place in game folder

After obtaining the DLLs, copy them to:
    $GamePath

"@ -ForegroundColor Yellow
}

# Final summary
Write-Host "`n============================================" -ForegroundColor Cyan
Write-Host "Installation Summary" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Game Path: $GamePath"
Write-Host "Proxy DLL: Installed"
Write-Host "Streamline: Installed (if SDK found)"
Write-Host "DLSS DLL: $(if($dlssOk){'Ready'}else{'MISSING'})"
Write-Host "Frame Gen DLL: $(if($dlssgOk){'Ready'}else{'MISSING'})"
Write-Host ""

if ($dlssOk) {
    Write-Host "You can now run the game!" -ForegroundColor Green
    Write-Host "Check for 'dlss4_proxy.log' in the game folder for debug output."
} else {
    Write-Host "Please add the missing NVIDIA DLLs before running the game." -ForegroundColor Yellow
}
