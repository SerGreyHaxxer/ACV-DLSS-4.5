# ============================================================================
# DLSS 4 Proxy DLL - PowerShell Build Script
# ============================================================================
# This script builds the proxy DLL using Visual Studio's compiler.
# Run from PowerShell - it will auto-detect VS installation.
# ============================================================================

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "DLSS 4 Proxy DLL Build Script" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

$ErrorActionPreference = "Stop"

# Function to find Visual Studio
function Find-VisualStudio {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            return $vsPath
        }
    }
    
    # Try common paths
    $versions = @("2022", "2019", "2017")
    $editions = @("Enterprise", "Professional", "Community", "BuildTools")
    
    foreach ($ver in $versions) {
        foreach ($ed in $editions) {
            $paths = @(
                "C:\Program Files\Microsoft Visual Studio\$ver\$ed",
                "C:\Program Files (x86)\Microsoft Visual Studio\$ver\$ed"
            )
            foreach ($p in $paths) {
                if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") {
                    return $p
                }
            }
        }
    }
    
    return $null
}

# Find VS
$vsPath = Find-VisualStudio
if (-not $vsPath) {
    Write-Host "ERROR: Could not find Visual Studio!" -ForegroundColor Red
    Write-Host "Please install Visual Studio with C++ workload." -ForegroundColor Yellow
    exit 1
}

Write-Host "Found Visual Studio at: $vsPath" -ForegroundColor Green

# Set up environment
$vcvars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
Write-Host "Setting up build environment..."

# Run vcvars and capture environment
$envCmd = "`"$vcvars`" && set"
$envOutput = cmd /c $envCmd

foreach ($line in $envOutput) {
    if ($line -match "^(.+?)=(.*)$") {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

# Create output directory
$binDir = "bin"
if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir | Out-Null
}

Write-Host "`nBuilding DLSS 4 Proxy DLL..." -ForegroundColor Yellow

# Build command
$buildArgs = @(
    "/LD",
    "/EHsc",
    "/std:c++17",
    "/O2",
    "/DUNICODE",
    "/D_UNICODE", 
    "/DWIN32_LEAN_AND_MEAN",
    "/DNOMINMAX",
    "/I.",
    "/Fe:bin\dxgi.dll",
    "main.cpp",
    "src\proxy.cpp",
    "src\hooks.cpp",
    "src\ngx_wrapper.cpp",
    "src\streamline_integration.cpp",
    "src\d3d12_wrappers.cpp",
    "src\dxgi_wrappers.cpp",
    "src\resource_detector.cpp",
    "src\crash_handler.cpp",
    "src\pattern_scanner.cpp",
    "src\input_handler.cpp",
    "src\config_manager.cpp",
    "src\overlay.cpp",
    "src\iat_utils.cpp",
    "/link",
    "d3d12.lib",
    "dxgi.lib",
    "dxguid.lib",
    "user32.lib",
    "gdi32.lib",
    "shell32.lib",
    "/DEF:dxgi.def",
    "/DLL"
)

try {
    & cl $buildArgs
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`n============================================" -ForegroundColor Green
        Write-Host "BUILD SUCCESSFUL!" -ForegroundColor Green
        Write-Host "============================================" -ForegroundColor Green
        Write-Host "Output: bin\dxgi.dll" -ForegroundColor Cyan
        Write-Host "`nNext steps:" -ForegroundColor Yellow
        Write-Host "  1. Copy bin\dxgi.dll to AC Valhalla folder"
        Write-Host "  2. Add nvngx_dlss.dll and nvngx_dlssg.dll"
        Write-Host "  3. Run the game!"
    } else {
        Write-Host "`nBUILD FAILED!" -ForegroundColor Red
    }
} catch {
    Write-Host "Build error: $_" -ForegroundColor Red
}
