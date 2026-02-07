# AC Valhalla Build Script
# Builds the DLSS 4.5 proxy DLL and optionally deploys to game folder
#
# Usage:
#   .\build.ps1                    # Build only
#   .\build.ps1 -Deploy            # Build and deploy to game folder
#   .\build.ps1 -Clean             # Clean build directory first
#   .\build.ps1 -Deploy -GamePath "D:\Games\ACV"  # Custom game path

param(
    [switch]$Deploy,
    [switch]$Clean,
    [string]$GamePath = ""
)

$ErrorActionPreference = "Stop"
$VcpkgRoot = $env:VCPKG_ROOT
if (-not $VcpkgRoot) { $VcpkgRoot = "C:\vcpkg" }

# Colors
function Write-Success { param($msg) Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Info { param($msg) Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Write-Warn { param($msg) Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Write-Err { param($msg) Write-Host "[ERROR] $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "================================================================" -ForegroundColor Magenta
Write-Host "  AC Valhalla DLSS 4.5 Mod Builder                              " -ForegroundColor Magenta
Write-Host "================================================================" -ForegroundColor Magenta
Write-Host ""

# Find CMake
$CMake = Get-Command "cmake" -ErrorAction SilentlyContinue
if (-not $CMake) {
    # Try Visual Studio installation
    $VSPaths = @(
        "C:\Program Files\Microsoft Visual Studio\18\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($pattern in $VSPaths) {
        $found = Get-Item $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) {
            $CMake = $found.FullName
            break
        }
    }
}
if (-not $CMake) {
    Write-Err "CMake not found. Please install CMake or Visual Studio with C++ tools."
    exit 1
}
Write-Info "Using CMake: $CMake"

# Clean if requested
if ($Clean -and (Test-Path "build")) {
    Write-Info "Cleaning build directory..."
    Remove-Item -Recurse -Force "build"
}

# Create build directory
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Configure CMake
Write-Host ""
Write-Host "[BUILD] Configuring CMake..." -ForegroundColor Cyan
$ConfigArgs = @("-B", "build", "-G", "Visual Studio 18 2026", "-A", "x64")
if (Test-Path "$VcpkgRoot\scripts\buildsystems\vcpkg.cmake") {
    $ConfigArgs += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake"
    Write-Info "Using vcpkg from: $VcpkgRoot"
}

& $CMake @ConfigArgs
if ($LASTEXITCODE -ne 0) {
    Write-Err "CMake configuration failed!"
    exit 1
}
Write-Success "CMake configured"

# Build
Write-Host ""
Write-Host "[BUILD] Building Release..." -ForegroundColor Cyan
& $CMake --build build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Err "Build failed!"
    exit 1
}
Write-Success "Build completed"

# Copy to bin folder
$OutputDll = "build\Release\dxgi.dll"
if (-not (Test-Path "bin")) {
    New-Item -ItemType Directory -Path "bin" | Out-Null
}
Copy-Item $OutputDll "bin\dxgi.dll" -Force
Write-Success "Copied to bin\dxgi.dll"

# Show build info
$dllInfo = Get-Item "bin\dxgi.dll"
Write-Host ""
Write-Host "================================================================" -ForegroundColor Green
Write-Host "  BUILD SUCCESSFUL                                              " -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Output:  bin\dxgi.dll" -ForegroundColor White
Write-Host "  Size:    $([math]::Round($dllInfo.Length / 1KB)) KB" -ForegroundColor White
Write-Host ""

# Deploy if requested
if ($Deploy) {
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "  DEPLOYING TO GAME FOLDER                                     " -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host ""
    
    # Find game path
    if (-not $GamePath) {
        # Try common locations
        $commonPaths = @(
            "C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla",
            "D:\Steam\steamapps\common\Assassin's Creed Valhalla",
            "E:\Steam\steamapps\common\Assassin's Creed Valhalla",
            "C:\Games\Assassin's Creed Valhalla",
            "D:\Games\Assassin's Creed Valhalla"
        )
        
        foreach ($path in $commonPaths) {
            if (Test-Path "$path\ACValhalla.exe") {
                $GamePath = $path
                break
            }
        }
        
        # Try reading from registry (Ubisoft Connect)
        if (-not $GamePath) {
            $regPath = Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\*" -ErrorAction SilentlyContinue |
            Where-Object { $_.InstallDir -like "*Valhalla*" } |
            Select-Object -First 1 -ExpandProperty InstallDir
            if ($regPath -and (Test-Path "$regPath\ACValhalla.exe")) {
                $GamePath = $regPath
            }
        }
    }
    
    if (-not $GamePath -or -not (Test-Path $GamePath)) {
        Write-Warn "Game folder not found automatically."
        Write-Host "  Please enter the path to AC Valhalla (containing ACValhalla.exe):" -ForegroundColor Yellow
        $GamePath = Read-Host "  Path"
    }
    
    if (-not (Test-Path "$GamePath\ACValhalla.exe")) {
        Write-Err "Invalid game path: $GamePath"
        Write-Err "ACValhalla.exe not found in that folder."
        exit 1
    }
    
    Write-Info "Deploying to: $GamePath"
    
    # Copy main DLL
    Copy-Item "bin\dxgi.dll" "$GamePath\dxgi.dll" -Force
    Write-Success "Copied dxgi.dll"
    
    # Copy Streamline DLLs if available
    $StreamlinePath = "external\streamline\bin\x64"
    if (-not (Test-Path $StreamlinePath)) {
        # Try Downloads folder
        $downloads = [Environment]::GetFolderPath("UserProfile") + "\Downloads"
        $slDirs = Get-ChildItem "$downloads\streamline-sdk-*" -Directory -ErrorAction SilentlyContinue
        if ($slDirs) {
            $StreamlinePath = "$($slDirs[-1].FullName)\bin\x64"
        }
    }
    
    if (Test-Path $StreamlinePath) {
        $slDlls = @(
            "sl.interposer.dll",
            "sl.common.dll",
            "sl.dlss.dll",
            "sl.dlss_g.dll"
        )
        
        foreach ($dll in $slDlls) {
            $srcPath = "$StreamlinePath\$dll"
            if (Test-Path $srcPath) {
                Copy-Item $srcPath "$GamePath\$dll" -Force
                Write-Success "Copied $dll"
            }
            else {
                Write-Warn "Streamline DLL not found: $dll"
            }
        }
        
        # Copy NGX DLLs
        $ngxDlls = Get-ChildItem "$StreamlinePath\nvngx_*.dll" -ErrorAction SilentlyContinue
        foreach ($ngx in $ngxDlls) {
            Copy-Item $ngx.FullName "$GamePath\$($ngx.Name)" -Force
            Write-Success "Copied $($ngx.Name)"
        }
    }
    else {
        Write-Warn "Streamline SDK not found. Only dxgi.dll was copied."
        Write-Warn "Please manually copy sl.*.dll and nvngx_*.dll from the Streamline SDK."
    }
    
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Green
    Write-Host "  DEPLOYMENT COMPLETE                                          " -ForegroundColor Green
    Write-Host "================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "  Game Folder: $GamePath" -ForegroundColor White
    Write-Host ""
    Write-Host "  Next Steps:" -ForegroundColor Cyan
    Write-Host "     1. Launch AC Valhalla" -ForegroundColor White
    Write-Host "     2. Set Borderless Windowed + Resolution Scale 50%" -ForegroundColor White
    Write-Host "     3. Press F5 to open the Control Panel" -ForegroundColor White
    Write-Host ""
}
else {
    Write-Host "  To deploy to game folder, run:" -ForegroundColor Cyan
    Write-Host "     .\build.ps1 -Deploy" -ForegroundColor White
    Write-Host ""
}
