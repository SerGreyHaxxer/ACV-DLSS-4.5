# ============================================================================ 
# DLSS 4 PROXY - COMPLETE AUTO-SETUP 
# ============================================================================ 
# This script automates the entire process: 
# 1. Builds the Proxy DLL 
# 2. auto-detects Assassin's Creed Valhalla (Registry & Deep Scan) 
# 3. Installs Proxy + Streamline SDK files 
# ============================================================================ 

$ErrorActionPreference = "Stop"
$StreamlineSDK = "C:\Users\serge\Downloads\streamline-sdk-v2.10.3"

function Write-Step { param($msg) Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Write-Ok { param($msg) Write-Host " [OK] $msg" -ForegroundColor Green }
function Write-Warn { param($msg) Write-Host " [WARN] $msg" -ForegroundColor Yellow }
function Write-Err { param($msg) Write-Host " [ERR] $msg" -ForegroundColor Red }

# ============================================================================ 
# STEP 1: FIND THE GAME 
# ============================================================================ 
Write-Step "Step 1: Locating Assassin's Creed Valhalla" 

$GamePath = $null 

# Method A: Check Registry (Ubisoft Connect) 
# AC Valhalla AppID is usually 13504 
$uplayReg = "HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\13504" 
if (Test-Path $uplayReg) { 
    $installDir = Get-ItemProperty -Path $uplayReg -Name "InstallDir" -ErrorAction SilentlyContinue 
    if ($installDir) { 
        $GamePath = $installDir.InstallDir 
        Write-Ok "Found via Ubisoft Registry: $GamePath" 
    } 
} 

# Method B: Check Uninstall Keys (Reliable for most installs) 
if (-not $GamePath) { 
    $uninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" 
    $uninstall64 = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall" 
    
    $entries = Get-ChildItem $uninstallKey, $uninstall64 -ErrorAction SilentlyContinue 
    foreach ($entry in $entries) { 
        $props = Get-ItemProperty $entry.PSPath 
        if ($props.DisplayName -like "*Assassin's Creed*Valhalla*") { 
            if ($props.InstallLocation) { 
                $GamePath = $props.InstallLocation 
                Write-Ok "Found via Uninstall Registry: $GamePath" 
                break 
            } 
        } 
    } 
} 

# Method C: Common Paths 
if (-not $GamePath) { 
    $candidates = @( 
        "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla", 
        "C:\Program Files\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla", 
        "D:\Games\Assassin's Creed Valhalla", 
        "E:\Games\Assassin's Creed Valhalla", 
        "C:\Games\Assassin's Creed Valhalla", 
        "C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla" 
    ) 
    foreach ($p in $candidates) { 
        if (Test-Path "$p\ACValhalla.exe") { 
            $GamePath = $p 
            Write-Ok "Found via Common Paths: $GamePath" 
            break 
        } 
    } 
} 

# Method D: Deep Scan (Last Resort - Scans C:\ and D:\ Games/Program Files) 
if (-not $GamePath) { 
    Write-Warn "Quick search failed. Performing deep scan (this might take a moment)..." 
    $scanRoots = @("C:\Program Files", "C:\Program Files (x86)", "C:\Games", "D:\Games", "D:\Program Files") 
    foreach ($root in $scanRoots) { 
        if (Test-Path $root) { 
            $found = Get-ChildItem -Path $root -Filter "ACValhalla.exe" -Recurse -ErrorAction SilentlyContinue -Depth 4 | Select-Object -First 1 
            if ($found) { 
                $GamePath = $found.DirectoryName 
                Write-Ok "Found via Deep Scan: $GamePath" 
                break 
            } 
        } 
    } 
} 

if (-not $GamePath) { 
    Write-Err "Could not find AC Valhalla anywhere on the system." 
    $GamePath = Read-Host "Please paste the full path to the game folder manually" 
    if (-not (Test-Path "$GamePath\ACValhalla.exe")) { 
        Write-Err "Invalid path provided." 
        exit 1 
    } 
} 

# ============================================================================ 
# STEP 2: BUILD PROXY 
# ============================================================================ 
Write-Step "Step 2: Building Proxy DLL" 

if (Test-Path "bin\dxgi.dll") { 
    Remove-Item "bin\dxgi.dll" -Force 
} 

$buildProcess = Start-Process -FilePath ".\build.bat" -Wait -PassThru -NoNewWindow 
if ($buildProcess.ExitCode -ne 0) { 
    Write-Err "Build failed with exit code $($buildProcess.ExitCode)" 
    exit 1 
} 

if (-not (Test-Path "bin\dxgi.dll")) { 
    Write-Err "Build script finished but bin\dxgi.dll is missing." 
    exit 1 
} 
Write-Ok "Build successful." 

# ============================================================================ 
# STEP 3: INSTALL PROXY 
# ============================================================================ 
Write-Step "Step 3: Installing Proxy Files" 

$dest = "$GamePath\dxgi.dll" 
if (Test-Path $dest) { 
    Move-Item $dest "$GamePath\dxgi.dll.bak_$(Get-Date -Format 'yyyyMMdd_HHmmss')" -Force 
    Write-Ok "Backed up existing dxgi.dll" 
} 

Copy-Item "bin\dxgi.dll" $dest -Force 
Write-Ok "Copied Proxy DLL to game folder" 

# ============================================================================ 
# STEP 4: INSTALL STREAMLINE SDK 
# ============================================================================ 
Write-Step "Step 4: Installing Streamline SDK" 

if (Test-Path $StreamlineSDK) { 
    # Interposer 
    Copy-Item "$StreamlineSDK\bin\x64\sl.interposer.dll" "$GamePath\" -Force 
    Write-Ok "Copied sl.interposer.dll" 
    
    # Common 
    if (Test-Path "$StreamlineSDK\bin\x64\sl.common.dll") { 
        Copy-Item "$StreamlineSDK\bin\x64\sl.common.dll" "$GamePath\" -Force 
        Write-Ok "Copied sl.common.dll" 
    } 

        # Plugins (DLSS & Frame Gen) - Copy to root to be safe
        # Note: In this SDK version, they are in bin\x64 directly, not a plugins subdir
        $plugins = @("sl.dlss.dll", "sl.dlss_g.dll")
        foreach ($p in $plugins) {
            $src = "$StreamlineSDK\bin\x64\$p"
            if (Test-Path $src) {
                Copy-Item $src "$GamePath\" -Force
                Write-Ok "Copied plugin: $p"
            } else {
                Write-Warn "Missing plugin in SDK: $p"
            }
        }} else { 
    Write-Err "Streamline SDK not found at $StreamlineSDK" 
    Write-Warn "You will need to manually copy sl.interposer.dll and plugins." 
} 

# ============================================================================ 
# FINAL CHECK 
# ============================================================================ 
Write-Step "Final Status Check" 

$missing = @() 
if (-not (Test-Path "$GamePath\nvngx_dlss.dll")) { $missing += "nvngx_dlss.dll (DLSS AI Model)" } 
if (-not (Test-Path "$GamePath\nvngx_dlssg.dll")) { $missing += "nvngx_dlssg.dll (Frame Gen AI Model)" } 

if ($missing.Count -gt 0) { 
    Write-Warn "The installation is mostly complete, BUT specific NVIDIA AI models are missing:" 
    foreach ($m in $missing) { Write-Host " - $m" -ForegroundColor Red } 
    Write-Host "`nPlease copy these files from another modern game (Cyberpunk/Alan Wake 2) or the NVIDIA DLSS archive." -ForegroundColor Yellow 
} else { 
    Write-Ok "All NVIDIA components appear to be present!" 
    Write-Host "`nREADY TO PLAY!" -ForegroundColor Cyan 
    Write-Host "Launch ACValhalla.exe and check the directory for 'dlss4_proxy.log' if issues arise." -ForegroundColor Gray 
} 
