# ============================================================================
# DLSS 4 Mod Deployment Script
# ============================================================================
# This script copies all required files to the game folder
# ============================================================================

param(
    [Parameter(Mandatory=$false)]
    [string]$GamePath = "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla"
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "DLSS 4.5 Mod Deployment" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if game path exists
if (-not (Test-Path $GamePath)) {
    Write-Host "ERROR: Game folder not found at:" -ForegroundColor Red
    Write-Host $GamePath -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Please provide the correct path:" -ForegroundColor White
    Write-Host "  .\deploy_mod.ps1 -GamePath 'C:\Your\Game\Path'" -ForegroundColor Gray
    exit 1
}

# Define source files
$SourceFiles = @(
    "bin\dxgi.dll"
    "bin\sl.interposer.dll"
    "bin\sl.common.dll"
    "bin\sl.dlss.dll"
    "bin\sl.dlss_g.dll"
    "bin\sl.reflex.dll"
    "bin\nvngx_dlss.dll"
    "bin\nvngx_dlssg.dll"
)

# Check if all source files exist
$MissingFiles = @()
foreach ($file in $SourceFiles) {
    if (-not (Test-Path $file)) {
        $MissingFiles += $file
    }
}

if ($MissingFiles.Count -gt 0) {
    Write-Host "ERROR: Missing required files:" -ForegroundColor Red
    foreach ($file in $MissingFiles) {
        Write-Host "  - $file" -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "Please build the project first using:" -ForegroundColor White
    Write-Host "  build.bat" -ForegroundColor Gray
    Write-Host "or" -ForegroundColor White
    Write-Host "  cmake --build build --config Release" -ForegroundColor Gray
    exit 1
}

# Copy files
Write-Host "Copying files to game folder..." -ForegroundColor Green
Write-Host "Destination: $GamePath" -ForegroundColor Gray
Write-Host ""

foreach ($file in $SourceFiles) {
    $filename = Split-Path $file -Leaf
    Write-Host "  Copying $filename..." -NoNewline
    try {
        Copy-Item -Path $file -Destination $GamePath -Force
        Write-Host " OK" -ForegroundColor Green
    } catch {
        Write-Host " FAILED" -ForegroundColor Red
        Write-Host "ERROR: $_" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "Deployment Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Files installed to:" -ForegroundColor White
Write-Host "  $GamePath" -ForegroundColor Gray
Write-Host ""
Write-Host "Controls:" -ForegroundColor White
Write-Host "  F5 - Open DLSS Control Panel" -ForegroundColor Gray
Write-Host "  F6 - Toggle FPS Counter" -ForegroundColor Gray
Write-Host "  F7 - Toggle Vignette" -ForegroundColor Gray
Write-Host "  F8 - Debug Camera Status" -ForegroundColor Gray
Write-Host ""
Write-Host "Recommended Settings:" -ForegroundColor White
Write-Host "  1. Launch the game" -ForegroundColor Gray
Write-Host "  2. Press F5 to open the menu" -ForegroundColor Gray
Write-Host "  3. Set Frame Generation to 4x" -ForegroundColor Gray
Write-Host "  4. Set DLSS Mode to Performance" -ForegroundColor Gray
Write-Host "  5. In game settings, set Resolution Scale to 50%" -ForegroundColor Gray
