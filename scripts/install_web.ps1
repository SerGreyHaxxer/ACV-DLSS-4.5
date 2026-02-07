# ============================================================================
# AC Valhalla DLSS 4.5 Mod - One-Line Web Installer
# ============================================================================
# Usage (paste into PowerShell as Admin):
#   iwr -useb https://raw.githubusercontent.com/AcerThyRacer/ACV-DLSS-4.5/main/scripts/install_web.ps1 | iex
#
# What this does:
#   1. Downloads dxgi.dll from the latest GitHub release
#   2. Downloads Streamline SDK DLLs from the repo
#   3. Auto-detects your AC Valhalla game folder
#   4. Copies everything to the game folder
# ============================================================================

$ErrorActionPreference = "Stop"
$RepoOwner = "AcerThyRacer"
$RepoName = "ACV-DLSS-4.5"
$TempDir = Join-Path $env:TEMP "ACValhalla_DLSS_Install"
$Branch = "main"

Write-Host ""
Write-Host "  ====================================================" -ForegroundColor Magenta
Write-Host "       AC Valhalla DLSS 4.5 - Web Installer           " -ForegroundColor Magenta
Write-Host "  ====================================================" -ForegroundColor Magenta
Write-Host ""

# -- Clean Temp ---------------------------------------------------------------
if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
New-Item -Path $TempDir -ItemType Directory | Out-Null

# -- Step 1: Download dxgi.dll from latest release ----------------------------
Write-Host "  [1] Fetching latest release from GitHub..." -ForegroundColor Cyan
$dxgiPath = Join-Path $TempDir "dxgi.dll"

try {
    $apiUrl = "https://api.github.com/repos/$RepoOwner/$RepoName/releases/latest"
    $headers = @{ "User-Agent" = "ACValhalla-DLSS-Installer" }
    $release = Invoke-RestMethod -Uri $apiUrl -Headers $headers -UseBasicParsing -ErrorAction Stop
    $tag = $release.tag_name

    # Look for dxgi.dll directly as a release asset
    $dllAsset = $release.assets | Where-Object { $_.name -eq "dxgi.dll" } | Select-Object -First 1

    if ($dllAsset) {
        Write-Host "   OK  Release: $tag (dxgi.dll asset found)" -ForegroundColor Green
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $dllAsset.browser_download_url -OutFile $dxgiPath -UseBasicParsing
        $ProgressPreference = 'Continue'
    }
    else {
        # Try zip asset that might contain dxgi.dll
        $zipAsset = $release.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1
        if ($zipAsset) {
            Write-Host "   OK  Release: $tag ($($zipAsset.name))" -ForegroundColor Green
            $zipPath = Join-Path $TempDir $zipAsset.name
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $zipAsset.browser_download_url -OutFile $zipPath -UseBasicParsing
            $ProgressPreference = 'Continue'
            $extractDir = Join-Path $TempDir "zip_extract"
            Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force
            $foundDll = Get-ChildItem $extractDir -Recurse -Filter "dxgi.dll" | Select-Object -First 1
            if ($foundDll) {
                Copy-Item $foundDll.FullName $dxgiPath -Force
            }
            else {
                throw "dxgi.dll not found inside release zip"
            }
        }
        else {
            throw "No dxgi.dll or zip asset found in release $tag"
        }
    }
    $dlSize = [math]::Round((Get-Item $dxgiPath).Length / 1KB, 0)
    Write-Host "   OK  Downloaded dxgi.dll ($dlSize KB)" -ForegroundColor Green
}
catch {
    Write-Host "   FAIL  Could not download from release: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Trying direct download from repo bin/ folder..." -ForegroundColor Yellow
    try {
        $directUrl = "https://raw.githubusercontent.com/$RepoOwner/$RepoName/$Branch/bin/dxgi.dll"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $directUrl -OutFile $dxgiPath -UseBasicParsing
        $ProgressPreference = 'Continue'
        $dlSize = [math]::Round((Get-Item $dxgiPath).Length / 1KB, 0)
        Write-Host "   OK  Downloaded dxgi.dll from repo ($dlSize KB)" -ForegroundColor Green
    }
    catch {
        Write-Host "   FAIL  Could not download dxgi.dll!" -ForegroundColor Red
        Write-Host ""
        Write-Host "  Manual fix:" -ForegroundColor Yellow
        Write-Host "    1. Go to: https://github.com/$RepoOwner/$RepoName/releases" -ForegroundColor White
        Write-Host "    2. Download dxgi.dll from the latest release" -ForegroundColor White
        Write-Host "    3. Copy it to your AC Valhalla game folder" -ForegroundColor White
        Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
        exit 1
    }
}

# -- Step 2: Download Streamline SDK DLLs -------------------------------------
Write-Host "  [2] Downloading Streamline SDK DLLs..." -ForegroundColor Cyan

$slDlls = @(
    "sl.interposer.dll",
    "sl.common.dll",
    "sl.dlss.dll",
    "sl.dlss_g.dll",
    "nvngx_dlss.dll",
    "nvngx_dlssg.dll",
    "nvngx_dlssd.dll",
    "nvngx_deepdvc.dll"
)

$baseRawUrl = "https://raw.githubusercontent.com/$RepoOwner/$RepoName/$Branch/bin"
$downloaded = 0
$ProgressPreference = 'SilentlyContinue'

foreach ($dll in $slDlls) {
    $destPath = Join-Path $TempDir $dll
    try {
        Invoke-WebRequest -Uri "$baseRawUrl/$dll" -OutFile $destPath -UseBasicParsing
        $downloaded++
    }
    catch {
        Write-Host "   WARN  Could not download $dll (non-critical)" -ForegroundColor Yellow
    }
}
$ProgressPreference = 'Continue'
Write-Host "   OK  Downloaded $downloaded/$($slDlls.Count) SDK DLLs" -ForegroundColor Green

# -- Step 3: Find AC Valhalla -------------------------------------------------
Write-Host "  [3] Searching for AC Valhalla..." -ForegroundColor Cyan

$GamePath = $null
$candidates = @()

# Method 1: Steam library folders
$steamPaths = @(
    "$env:ProgramFiles\Steam",
    "${env:ProgramFiles(x86)}\Steam"
)
try {
    $reg = Get-ItemProperty "HKCU:\SOFTWARE\Valve\Steam" -ErrorAction SilentlyContinue
    if ($reg.SteamPath) { $steamPaths += $reg.SteamPath }
} catch {}
try {
    $reg = Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -ErrorAction SilentlyContinue
    if ($reg.InstallPath) { $steamPaths += $reg.InstallPath }
} catch {}

foreach ($steamRoot in ($steamPaths | Sort-Object -Unique)) {
    $vdfPath = "$steamRoot\steamapps\libraryfolders.vdf"
    if (Test-Path $vdfPath) {
        $vdfContent = Get-Content $vdfPath -Raw
        $paths = [regex]::Matches($vdfContent, '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
        foreach ($libPath in $paths) {
            $acv = "$libPath\steamapps\common\Assassin's Creed Valhalla"
            if ((Test-Path "$acv\ACValhalla.exe") -and ($candidates -notcontains $acv)) {
                $candidates += $acv
            }
        }
    }
    $defaultAcv = "$steamRoot\steamapps\common\Assassin's Creed Valhalla"
    if ((Test-Path "$defaultAcv\ACValhalla.exe") -and ($candidates -notcontains $defaultAcv)) {
        $candidates += $defaultAcv
    }
}

# Method 2: Ubisoft Connect
try {
    $ubiKeys = Get-ChildItem "HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs" -ErrorAction SilentlyContinue
    foreach ($key in $ubiKeys) {
        $props = Get-ItemProperty $key.PSPath -ErrorAction SilentlyContinue
        if ($props.InstallDir -and (Test-Path "$($props.InstallDir)\ACValhalla.exe")) {
            $p = $props.InstallDir.TrimEnd('\')
            if ($candidates -notcontains $p) { $candidates += $p }
        }
    }
} catch {}

# Method 3: Common paths on all drives
$commonSubPaths = @(
    "Steam\steamapps\common\Assassin's Creed Valhalla",
    "SteamLibrary\steamapps\common\Assassin's Creed Valhalla",
    "Games\Assassin's Creed Valhalla",
    "Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla",
    "Program Files\Ubisoft\Assassin's Creed Valhalla"
)
Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
    foreach ($sub in $commonSubPaths) {
        $p = "$($_.Root)$sub"
        if ((Test-Path "$p\ACValhalla.exe") -and ($candidates -notcontains $p)) {
            $candidates += $p
        }
    }
}

if ($candidates.Count -eq 0) {
    Write-Host "   !!  Could not find AC Valhalla automatically." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Please enter the full path to your AC Valhalla folder:" -ForegroundColor White
    Write-Host "  (e.g. C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla)" -ForegroundColor DarkGray
    $userInput = Read-Host "  Path"
    if (-not (Test-Path "$userInput\ACValhalla.exe")) {
        Write-Host "   FAIL  ACValhalla.exe not found at: $userInput" -ForegroundColor Red
        Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
        exit 1
    }
    $GamePath = $userInput
}
elseif ($candidates.Count -eq 1) {
    $GamePath = $candidates[0]
    Write-Host "   OK  Found: $GamePath" -ForegroundColor Green
}
else {
    Write-Host "   OK  Found $($candidates.Count) installations:" -ForegroundColor Green
    for ($i = 0; $i -lt $candidates.Count; $i++) {
        Write-Host "    [$($i+1)] $($candidates[$i])" -ForegroundColor White
    }
    Write-Host ""
    $choice = Read-Host "  Select (default 1)"
    if (-not $choice) { $choice = "1" }
    $idx = [int]$choice - 1
    if ($idx -lt 0 -or $idx -ge $candidates.Count) { $idx = 0 }
    $GamePath = $candidates[$idx]
    Write-Host "   OK  Selected: $GamePath" -ForegroundColor Green
}

# -- Step 4: Install ----------------------------------------------------------
Write-Host "  [4] Installing to game folder..." -ForegroundColor Cyan

# Copy dxgi.dll
Copy-Item $dxgiPath -Destination "$GamePath\dxgi.dll" -Force
Write-Host "   OK  dxgi.dll" -ForegroundColor Green

# Copy Streamline DLLs
$copied = 0
foreach ($dll in $slDlls) {
    $src = Join-Path $TempDir $dll
    if (Test-Path $src) {
        Copy-Item $src -Destination "$GamePath\$dll" -Force
        $copied++
    }
}
Write-Host "   OK  $copied Streamline SDK DLLs" -ForegroundColor Green

# -- Cleanup -------------------------------------------------------------------
Write-Host ""
Write-Host "  Cleaning up temp files..." -ForegroundColor DarkGray
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "  ====================================================" -ForegroundColor Green
Write-Host "       Installation Complete!                          " -ForegroundColor Green
Write-Host "  ====================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Game Folder: $GamePath" -ForegroundColor White
Write-Host ""
Write-Host "  Next Steps:" -ForegroundColor Cyan
Write-Host "    1. Launch AC Valhalla" -ForegroundColor White
Write-Host "    2. Set Borderless Windowed + Resolution Scale 50%" -ForegroundColor White
Write-Host "    3. Press F5 to open the Control Panel" -ForegroundColor White
Write-Host ""
