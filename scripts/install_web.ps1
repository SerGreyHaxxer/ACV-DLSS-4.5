# ============================================================================
# AC Valhalla DLSS 4.5 Mod — One-Line Web Installer
# ============================================================================
# Usage (paste into PowerShell as Admin):
#   iwr -useb https://raw.githubusercontent.com/AcerThyRacer/ACV-DLSS-4.5/main/scripts/install_web.ps1 | iex
#
# What this does:
#   1. Downloads the latest release ZIP from GitHub
#   2. Extracts to a temp folder
#   3. Runs install.ps1 which auto-finds the game
#   4. Cleans up temp files
# ============================================================================

$ErrorActionPreference = "Stop"
$RepoOwner = "AcerThyRacer"
$RepoName = "ACV-DLSS-4.5"
$TempDir = Join-Path $env:TEMP "ACValhalla_DLSS_Install"

Write-Host ""
Write-Host "  ╔══════════════════════════════════════════════════════════╗" -ForegroundColor Magenta
Write-Host "  ║     AC Valhalla DLSS 4.5 — Web Installer               ║" -ForegroundColor Magenta
Write-Host "  ╚══════════════════════════════════════════════════════════╝" -ForegroundColor Magenta
Write-Host ""

# ── Clean Temp ─────────────────────────────────────────────────────────────
if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
New-Item -Path $TempDir -ItemType Directory | Out-Null

# ── Find Latest Release ───────────────────────────────────────────────────
Write-Host "  [1] Fetching latest release from GitHub..." -ForegroundColor Cyan
try {
    $apiUrl = "https://api.github.com/repos/$RepoOwner/$RepoName/releases/latest"
    $release = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing -ErrorAction Stop
    $zipAsset = $release.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1

    if (-not $zipAsset) {
        # Fallback: download source zipball
        $downloadUrl = $release.zipball_url
        $zipName = "$RepoName-$($release.tag_name).zip"
    }
    else {
        $downloadUrl = $zipAsset.browser_download_url
        $zipName = $zipAsset.name
    }

    Write-Host "   ✓  Release: $($release.tag_name) ($zipName)" -ForegroundColor Green
}
catch {
    # Fallback to direct main branch download if no releases
    Write-Host "   ⚠  No releases found, downloading main branch..." -ForegroundColor Yellow
    $downloadUrl = "https://github.com/$RepoOwner/$RepoName/archive/refs/heads/main.zip"
    $zipName = "$RepoName-main.zip"
}

# ── Download ──────────────────────────────────────────────────────────────
$zipPath = "$TempDir\$zipName"
Write-Host "  [2] Downloading..." -ForegroundColor Cyan
try {
    # Use BITS for progress, fall back to Invoke-WebRequest
    $useBits = $true
    try { Import-Module BitsTransfer -ErrorAction Stop } catch { $useBits = $false }

    if ($useBits) {
        Start-BitsTransfer -Source $downloadUrl -Destination $zipPath -Description "Downloading DLSS 4.5 Mod"
    }
    else {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -UseBasicParsing
        $ProgressPreference = 'Continue'
    }
    $dlSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Write-Host "   ✓  Downloaded ($dlSize MB)" -ForegroundColor Green
}
catch {
    Write-Host "   ✗  Download failed: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "  Try manual download:" -ForegroundColor Yellow
    Write-Host "    https://github.com/$RepoOwner/$RepoName/releases" -ForegroundColor White
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# ── Extract ───────────────────────────────────────────────────────────────
Write-Host "  [3] Extracting..." -ForegroundColor Cyan
$extractDir = "$TempDir\extracted"
Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

# Find the install script (may be in a subfolder if downloaded as source)
$installScript = Get-ChildItem $extractDir -Recurse -Filter "install.ps1" | Select-Object -First 1
if (-not $installScript) {
    Write-Host "   ✗  install.ps1 not found in archive!" -ForegroundColor Red
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

$modRoot = $installScript.DirectoryName
Write-Host "   ✓  Extracted to: $modRoot" -ForegroundColor Green

# ── Run Installer ─────────────────────────────────────────────────────────
Write-Host "  [4] Running auto-installer..." -ForegroundColor Cyan
Write-Host ""

& $installScript.FullName -Silent:$false
$exitCode = $LASTEXITCODE

# ── Cleanup ───────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  Cleaning up temp files..." -ForegroundColor DarkGray
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

if ($exitCode -ne 0) {
    Write-Host "  Installation encountered an issue (exit code: $exitCode)" -ForegroundColor Yellow
}
else {
    Write-Host "  Done! Temp files cleaned up." -ForegroundColor Green
}
Write-Host ""
