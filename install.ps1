# ============================================================================
# AC Valhalla DLSS 4.5 Mod — Universal Auto-Installer
# ============================================================================
# Automatically finds AC Valhalla across ALL installation methods:
#   • All Steam library folders (parses libraryfolders.vdf)
#   • Ubisoft Connect (registry + default paths)
#   • Epic Games Store (manifests)
#   • Common game directories on all drives
#
# Usage:
#   .\install.ps1                           # Auto-detect everything
#   .\install.ps1 -GamePath "D:\Games\ACV"  # Manual game path
#   .\install.ps1 -SkipSDK                  # Only install dxgi.dll
#   .\install.ps1 -Uninstall                # Remove mod files
# ============================================================================

param(
    [string]$GamePath = "",
    [switch]$SkipSDK,
    [switch]$Uninstall,
    [switch]$Silent
)

$ErrorActionPreference = "Stop"
$baseDir = $PSScriptRoot

# ── Pretty Output ──────────────────────────────────────────────────────────
function Write-Banner {
    Write-Host ""
    Write-Host "  ╔══════════════════════════════════════════════════════════╗" -ForegroundColor Magenta
    Write-Host "  ║        AC Valhalla — DLSS 4.5 Mod Installer            ║" -ForegroundColor Magenta
    Write-Host "  ║        github.com/AcerThyRacer/ACV-DLSS-4.5           ║" -ForegroundColor DarkMagenta
    Write-Host "  ╚══════════════════════════════════════════════════════════╝" -ForegroundColor Magenta
    Write-Host ""
}

function Write-Step { param($n, $msg) Write-Host "  [$n] $msg" -ForegroundColor Cyan }
function Write-OK { param($msg) Write-Host "   ✓  $msg" -ForegroundColor Green }
function Write-Skip { param($msg) Write-Host "   ○  $msg" -ForegroundColor DarkGray }
function Write-Warn { param($msg) Write-Host "   ⚠  $msg" -ForegroundColor Yellow }
function Write-Fail { param($msg) Write-Host "   ✗  $msg" -ForegroundColor Red }

Write-Banner

# ── Step 1: Locate dxgi.dll ────────────────────────────────────────────────
Write-Step 1 "Locating mod files..."

$dxgiPath = $null
$searchPaths = @(
    "$baseDir\bin\dxgi.dll",
    "$baseDir\dxgi.dll",
    "$baseDir\build\Release\dxgi.dll"
)
foreach ($p in $searchPaths) {
    if (Test-Path $p) { $dxgiPath = $p; break }
}

if (-not $dxgiPath) {
    Write-Fail "dxgi.dll not found! Make sure you extracted the full release."
    Write-Host ""
    Write-Host "  Expected locations:" -ForegroundColor Yellow
    foreach ($p in $searchPaths) { Write-Host "    • $p" -ForegroundColor DarkGray }
    Write-Host ""
    if (-not $Silent) { Pause }
    exit 1
}

$dllSize = [math]::Round((Get-Item $dxgiPath).Length / 1KB)
Write-OK "Found dxgi.dll ($dllSize KB) at: $dxgiPath"

# ── Step 2: Find AC Valhalla ───────────────────────────────────────────────
Write-Step 2 "Searching for Assassin's Creed Valhalla..."

function Find-ACValhalla {
    $candidates = @()

    # ── Method 1: All Steam Library Folders ──
    $steamPaths = @(
        "$env:ProgramFiles\Steam",
        "${env:ProgramFiles(x86)}\Steam"
    )
    # Check registry for Steam install location
    try {
        $steamReg = Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue
        if ($steamReg.SteamPath) { $steamPaths += $steamReg.SteamPath -replace '/', '\' }
    }
    catch {}
    try {
        $steamReg = Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -ErrorAction SilentlyContinue
        if ($steamReg.InstallPath) { $steamPaths += $steamReg.InstallPath }
    }
    catch {}

    foreach ($steamRoot in ($steamPaths | Sort-Object -Unique)) {
        $vdfPath = "$steamRoot\steamapps\libraryfolders.vdf"
        if (Test-Path $vdfPath) {
            # Parse ALL Steam library folder paths from libraryfolders.vdf
            $vdfContent = Get-Content $vdfPath -Raw
            $libPaths = [regex]::Matches($vdfContent, '"path"\s+"([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
            foreach ($lib in $libPaths) {
                $acvPath = "$lib\steamapps\common\Assassin's Creed Valhalla"
                if (Test-Path "$acvPath\ACValhalla.exe") {
                    $candidates += @{ Path = $acvPath; Source = "Steam" }
                }
            }
        }
        # Also check the default steamapps folder directly
        $defaultAcv = "$steamRoot\steamapps\common\Assassin's Creed Valhalla"
        if ((Test-Path "$defaultAcv\ACValhalla.exe") -and ($candidates.Path -notcontains $defaultAcv)) {
            $candidates += @{ Path = $defaultAcv; Source = "Steam" }
        }
    }

    # ── Method 2: Ubisoft Connect Registry ──
    try {
        $ubiKeys = Get-ChildItem "HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs" -ErrorAction SilentlyContinue
        foreach ($key in $ubiKeys) {
            $props = Get-ItemProperty $key.PSPath -ErrorAction SilentlyContinue
            if ($props.InstallDir -and $props.InstallDir -like "*Valhalla*") {
                $ubiPath = $props.InstallDir.TrimEnd('\')
                if (Test-Path "$ubiPath\ACValhalla.exe") {
                    $candidates += @{ Path = $ubiPath; Source = "Ubisoft Connect" }
                }
            }
        }
    }
    catch {}

    # ── Method 3: Ubisoft Connect Default Paths ──
    $ubiDefaults = @(
        "$env:ProgramFiles\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla",
        "${env:ProgramFiles(x86)}\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla"
    )
    foreach ($p in $ubiDefaults) {
        if ((Test-Path "$p\ACValhalla.exe") -and ($candidates.Path -notcontains $p)) {
            $candidates += @{ Path = $p; Source = "Ubisoft Connect" }
        }
    }

    # ── Method 4: Epic Games Store Manifests ──
    $epicManifests = "$env:ProgramData\Epic\EpicGamesLauncher\Data\Manifests"
    if (Test-Path $epicManifests) {
        try {
            Get-ChildItem "$epicManifests\*.item" -ErrorAction SilentlyContinue | ForEach-Object {
                $manifest = Get-Content $_.FullName -Raw | ConvertFrom-Json -ErrorAction SilentlyContinue
                if ($manifest.DisplayName -like "*Valhalla*" -and $manifest.InstallLocation) {
                    $epicPath = $manifest.InstallLocation
                    if (Test-Path "$epicPath\ACValhalla.exe") {
                        $candidates += @{ Path = $epicPath; Source = "Epic Games" }
                    }
                }
            }
        }
        catch {}
    }

    # ── Method 5: Steam App Registry ──
    try {
        $steamApp = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 2208920" -ErrorAction SilentlyContinue
        if ($steamApp.InstallLocation -and (Test-Path "$($steamApp.InstallLocation)\ACValhalla.exe")) {
            $p = $steamApp.InstallLocation
            if ($candidates.Path -notcontains $p) {
                $candidates += @{ Path = $p; Source = "Steam Registry" }
            }
        }
    }
    catch {}

    # ── Method 6: Brute-force all drives ──
    $commonSubPaths = @(
        "Steam\steamapps\common\Assassin's Creed Valhalla",
        "SteamLibrary\steamapps\common\Assassin's Creed Valhalla",
        "Games\Assassin's Creed Valhalla",
        "Ubisoft\Assassin's Creed Valhalla",
        "Epic Games\AssassinsCreedValhalla"
    )
    Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue |
    Where-Object { $_.Used -gt 0 } |
    ForEach-Object {
        $drive = $_.Root
        foreach ($sub in $commonSubPaths) {
            $p = Join-Path $drive $sub
            if ((Test-Path "$p\ACValhalla.exe") -and ($candidates.Path -notcontains $p)) {
                $candidates += @{ Path = $p; Source = "Drive Scan ($drive)" }
            }
        }
        # Also check Program Files on each drive
        foreach ($pf in @("Program Files", "Program Files (x86)")) {
            $p = Join-Path $drive "$pf\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla"
            if ((Test-Path "$p\ACValhalla.exe") -and ($candidates.Path -notcontains $p)) {
                $candidates += @{ Path = $p; Source = "Drive Scan ($drive)" }
            }
        }
    }

    return $candidates
}

if ($GamePath -and (Test-Path "$GamePath\ACValhalla.exe")) {
    Write-OK "Using provided path: $GamePath"
}
else {
    $found = Find-ACValhalla
    if ($found.Count -eq 0) {
        if ($Silent) {
            Write-Fail "Could not find AC Valhalla automatically."
            exit 1
        }
        Write-Warn "Could not find AC Valhalla automatically."
        Write-Host ""
        Write-Host "  Drag & drop ACValhalla.exe here, or type the game folder path:" -ForegroundColor Yellow
        $userInput = Read-Host "  Path"
        $userInput = $userInput.Trim('"').Trim("'")
        if ($userInput -match "\.exe$") { $userInput = Split-Path -Parent $userInput }
        if (Test-Path "$userInput\ACValhalla.exe") {
            $GamePath = $userInput
        }
        else {
            Write-Fail "ACValhalla.exe not found at: $userInput"
            Pause
            exit 1
        }
    }
    elseif ($found.Count -eq 1) {
        $GamePath = $found[0].Path
        Write-OK "Found via $($found[0].Source): $GamePath"
    }
    else {
        Write-OK "Found $($found.Count) installations:"
        for ($i = 0; $i -lt $found.Count; $i++) {
            Write-Host "    [$($i+1)] $($found[$i].Path) ($($found[$i].Source))" -ForegroundColor White
        }
        Write-Host ""
        $choice = Read-Host "  Select installation (1-$($found.Count)) [default: 1]"
        if (-not $choice) { $choice = "1" }
        $idx = [int]$choice - 1
        if ($idx -lt 0 -or $idx -ge $found.Count) { $idx = 0 }
        $GamePath = $found[$idx].Path
        Write-OK "Selected: $GamePath"
    }
}

# ── Uninstall ──────────────────────────────────────────────────────────────
if ($Uninstall) {
    Write-Step 3 "Removing mod files from game folder..."
    $modFiles = @("dxgi.dll", "sl.*.dll", "nvngx_*.dll", "dlss_settings.ini",
        "dlss4_proxy.log", "dlss4_crash.log", "startup_trace.log")
    $removed = 0
    foreach ($pattern in $modFiles) {
        Get-ChildItem "$GamePath\$pattern" -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item $_.FullName -Force
            Write-OK "Removed $($_.Name)"
            $removed++
        }
    }
    if ($removed -eq 0) { Write-Skip "No mod files found to remove." }
    Write-Host ""
    Write-Host "  Uninstall complete. The game is restored to vanilla." -ForegroundColor Green
    Write-Host ""
    if (-not $Silent) { Pause }
    exit 0
}

# ── Step 3: Copy Mod DLL ──────────────────────────────────────────────────
Write-Step 3 "Installing mod..."

Copy-Item -Path $dxgiPath -Destination "$GamePath\dxgi.dll" -Force
Write-OK "dxgi.dll -> game folder"

# ── Step 4: Find & Copy Streamline SDK ────────────────────────────────────
if ($SkipSDK) {
    Write-Skip "SDK copy skipped (-SkipSDK flag)"
}
else {
    Write-Step 4 "Looking for NVIDIA Streamline SDK..."

    $sdkDlls = @(
        "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll",
        "sl.dlss_d.dll", "sl.deepdvc.dll", "sl.reflex.dll",
        "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssd.dll", "nvngx_deepdvc.dll"
    )

    $sdkSource = $null

    # Priority 1: Environment variable
    if ($env:DLSS4_SDK_BIN -and (Test-Path "$env:DLSS4_SDK_BIN\sl.interposer.dll")) {
        $sdkSource = $env:DLSS4_SDK_BIN
    }
    # Priority 2: bin/ folder (release package)
    if (-not $sdkSource -and (Test-Path "$baseDir\bin\sl.interposer.dll")) {
        $sdkSource = "$baseDir\bin"
    }
    # Priority 3: external/ folder (dev build)
    if (-not $sdkSource) {
        foreach ($sub in @("lib\x64", "bin\x64")) {
            $p = "$baseDir\external\streamline\$sub"
            if (Test-Path "$p\sl.interposer.dll") { $sdkSource = $p; break }
        }
    }
    # Priority 4: Already in game folder
    if (-not $sdkSource -and (Test-Path "$GamePath\sl.interposer.dll")) {
        $sdkSource = $GamePath
        Write-OK "Streamline SDK already present in game folder"
    }
    # Priority 5: Downloads folder
    if (-not $sdkSource) {
        $downloads = "$env:USERPROFILE\Downloads"
        $sdkFolders = Get-ChildItem "$downloads" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "streamline*" -or $_.Name -like "sl-sdk*" } |
        Sort-Object LastWriteTime -Descending
        foreach ($folder in $sdkFolders) {
            foreach ($sub in @("lib\x64", "bin\x64", "")) {
                $p = if ($sub) { "$($folder.FullName)\$sub" } else { $folder.FullName }
                if (Test-Path "$p\sl.interposer.dll") { $sdkSource = $p; break }
            }
            if ($sdkSource) { break }
        }
        # Try extracting ZIP if no folder found
        if (-not $sdkSource) {
            $sdkZip = Get-ChildItem "$downloads\streamline*.zip" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($sdkZip) {
                Write-Host "   …  Extracting $($sdkZip.Name)..." -ForegroundColor DarkGray
                $extractDir = "$env:TEMP\acv_dlss_sdk"
                if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
                Expand-Archive -Path $sdkZip.FullName -DestinationPath $extractDir -Force
                $searchDirs = @(Get-ChildItem $extractDir -Recurse -Filter "sl.interposer.dll" -ErrorAction SilentlyContinue)
                if ($searchDirs) {
                    $sdkSource = Split-Path $searchDirs[0].FullName
                }
            }
        }
    }

    if ($sdkSource -and $sdkSource -ne $GamePath) {
        Write-OK "SDK found: $sdkSource"
        $copied = 0; $missing = 0
        foreach ($dll in $sdkDlls) {
            $src = "$sdkSource\$dll"
            if (Test-Path $src) {
                Copy-Item -Path $src -Destination "$GamePath\$dll" -Force
                $copied++
            }
            else {
                $missing++
            }
        }
        $msg = "Copied $copied SDK DLLs"
        if ($missing -gt 0) { $msg += " ($missing optional DLLs not found)" }
        Write-OK $msg
    }
    elseif ($sdkSource -eq $GamePath) {
        # Already there, nothing to do
    }
    else {
        Write-Warn "Streamline SDK not found (DLSS features won't work without it)"
        Write-Host "   Download from: https://developer.nvidia.com/rtx/streamline" -ForegroundColor DarkGray
        Write-Host "   Place the SDK ZIP in your Downloads folder and re-run." -ForegroundColor DarkGray
    }
}

# ── Done ───────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  ╔══════════════════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "  ║              INSTALLATION COMPLETE ✓                    ║" -ForegroundColor Green
Write-Host "  ╚══════════════════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
Write-Host "  Game Folder: $GamePath" -ForegroundColor White
Write-Host ""
Write-Host "  Next Steps:" -ForegroundColor Cyan
Write-Host "    1. Launch AC Valhalla" -ForegroundColor White
Write-Host "    2. Set Borderless Windowed + Resolution Scale 50%" -ForegroundColor White
Write-Host "    3. Press F5 to open the DLSS Control Panel" -ForegroundColor White
Write-Host ""
if (-not $Silent -and -not $env:DLSS4_SKIP_PAUSE) { Pause }
