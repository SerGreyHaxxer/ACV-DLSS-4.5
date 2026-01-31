# ============================================================================
# DLSS 4.5 Mod - Complete Build & Deploy Script
# ============================================================================
# This script does EVERYTHING:
#   1. Verifies NVIDIA Streamline SDK
#   2. Finds your AC Valhalla installation automatically
#   3. Builds the mod (using MSBuild or CL.exe)
#   4. Copies all required files to the game folder
#   5. Creates a desktop shortcut (optional)
#   6. Shows installation summary
# ============================================================================

param(
    [switch]$CreateShortcut,
    [switch]$SkipBuild,
    [string]$CustomGamePath = "",
    [string]$SteamPath = "",
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "Continue"

# Script configuration
$Script:ModVersion = "4.5"
$Script:ModName = "DLSS 4.5 Mod for AC Valhalla"
$Script:ConfigFile = "mod_config.ini"
$Script:LogFile = "build_log.txt"

# Colors
$ColorSuccess = "Green"
$ColorError = "Red"
$ColorWarning = "Yellow"
$ColorInfo = "Cyan"
$ColorNormal = "White"

# ============================================================================
# UTILITY FUNCTIONS
# ============================================================================

function Write-Header($text) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor $ColorInfo
    Write-Host $text -ForegroundColor $ColorInfo
    Write-Host "========================================" -ForegroundColor $ColorInfo
    Write-Host ""
}

function Write-Success($text) { Write-Host "  [OK] $text" -ForegroundColor $ColorSuccess }
function Write-Error($text) { Write-Host "  [FAIL] $text" -ForegroundColor $ColorError }
function Write-Warning($text) { Write-Host "  [WARN] $text" -ForegroundColor $ColorWarning }
function Write-Info($text) { Write-Host "  [INFO] $text" -ForegroundColor $ColorNormal }

function Test-Admin {
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-SteamInstallation {
    # Try to find Steam installation
    $steamPaths = @(
        "${env:ProgramFiles(x86)}\Steam",
        "${env:ProgramFiles}\Steam",
        "C:\Steam"
    )
    
    # Check registry
    try {
        $regPath = Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -ErrorAction SilentlyContinue
        if ($regPath.InstallPath) { $steamPaths = @($regPath.InstallPath) + $steamPaths }
    } catch {}
    
    foreach ($path in $steamPaths) {
        if (Test-Path "$path\steam.exe") { return $path }
    }
    return $null
}

function Find-GameInstallation {
    param([string]$CustomPath)
    
    # If custom path provided, verify it
    if ($CustomPath -and (Test-Path $CustomPath)) {
        if (Test-Path "$CustomPath\ACValhalla.exe") { return $CustomPath }
    }
    
    # Try to read from config file
    if (Test-Path $Script:ConfigFile) {
        try {
            $content = Get-Content $Script:ConfigFile -Raw
            if (-not [string]::IsNullOrWhiteSpace($content)) {
                # Simple parsing instead of ConvertFrom-StringData which fails on backslashes
                foreach ($line in ($content -split '\r?\n')) {
                    if ($line -match "^GamePath=(.*)$") {
                        $path = $matches[1].Trim()
                        if (Test-Path "$path\ACValhalla.exe") { return $path }
                    }
                }
            }
        } catch {
            Write-Warning "Could not read config file, skipping."
        }
    }
    
    # Common installation paths
    $searchPaths = @(
        "${env:ProgramFiles(x86)}\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla",
        "${env:ProgramFiles}\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed Valhalla",
        "D:\Games\Assassin's Creed Valhalla",
        "D:\Games\Ubisoft\Assassin's Creed Valhalla",
        "E:\Games\Assassin's Creed Valhalla",
        "C:\Games\Assassin's Creed Valhalla"
    )
    
    # Try Steam library folders
    $steamPath = Find-SteamInstallation
    if ($steamPath) {
        $libraryFolders = @("$steamPath\steamapps\common")
        
        # Read additional library folders from vdf
        $libraryVdf = "$steamPath\steamapps\libraryfolders.vdf"
        if (Test-Path $libraryVdf) {
            $vdfContent = Get-Content $libraryVdf -Raw
            $matches = [regex]::Matches($vdfContent, '"path"\s+"([^"]+)"')
            foreach ($match in $matches) {
                $libraryPath = $match.Groups[1].Value.Replace('\\', '\')
                $libraryFolders += "$libraryPath\steamapps\common"
            }
        }
        
        foreach ($lib in $libraryFolders) {
            $searchPaths += "$lib\Assassin's Creed Valhalla"
        }
    }
    
    # Search for the game
    foreach ($path in $searchPaths) {
        if (Test-Path "$path\ACValhalla.exe") {
            return $path
        }
    }
    
    return $null
}

function Save-GamePath($path) {
    "GamePath=$path" | Out-File $Script:ConfigFile -Encoding ASCII
}

function Find-VisualStudio {
    # Try vswhere
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $installPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($installPath) {
            $msbuild = "$installPath\MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $msbuild) { return @{ MSBuild = $msbuild; VCVars = "$installPath\VC\Auxiliary\Build\vcvars64.bat" } }
        }
    }
    
    # Try common paths
    $vsVersions = @("2022", "2019", "2017")
    $editions = @("Enterprise", "Professional", "Community", "BuildTools")
    
    foreach ($ver in $vsVersions) {
        foreach ($ed in $editions) {
            $path = "${env:ProgramFiles}\Microsoft Visual Studio\$ver\$ed"
            if (Test-Path $path) {
                $msbuild = "$path\MSBuild\Current\Bin\MSBuild.exe"
                if (Test-Path $msbuild) { 
                    return @{ MSBuild = $msbuild; VCVars = "$path\VC\Auxiliary\Build\vcvars64.bat" }
                }
            }
            $path = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\$ver\$ed"
            if (Test-Path $path) {
                $msbuild = "$path\MSBuild\Current\Bin\MSBuild.exe"
                if (Test-Path $msbuild) { 
                    return @{ MSBuild = $msbuild; VCVars = "$path\VC\Auxiliary\Build\vcvars64.bat" }
                }
            }
        }
    }
    
    return $null
}

function Test-StreamlineSDK {
    $sdkPath = "$env:USERPROFILE\Downloads\streamline-sdk-v2.10.3"
    
    if (-not (Test-Path $sdkPath)) {
        # Try to find any streamline SDK in downloads
        $foundSdk = Get-ChildItem -Path "$env:USERPROFILE\Downloads" -Filter "streamline-sdk*" -Directory | Select-Object -First 1
        if ($foundSdk) { $sdkPath = $foundSdk.FullName }
    }
    
    $libPath = "$sdkPath\lib\x64\sl.interposer.lib"
    $includePath = "$sdkPath\include\sl.h"
    
    if (-not (Test-Path $libPath)) { return @{ Success = $false; Message = "sl.interposer.lib not found" } }
    if (-not (Test-Path $includePath)) { return @{ Success = $false; Message = "sl.h not found" } }
    
    return @{ 
        Success = $true
        Path = $sdkPath
        LibPath = $libPath
        IncludePath = "$sdkPath\include"
    }
}

function Invoke-BuildWithMSBuild($SDKPath) {
    # Generate project files
    if (-not (Test-Path "build")) { New-Item -ItemType Directory -Force -Path "build" | Out-Null }
    
    Push-Location "build"
    try {
        # Configure
        Write-Info "Configuring with CMake..."
        $cmakeOutput = cmake .. -G "Visual Studio 17 2022" -A x64 -DSTREAMLINE_SDK_PATH="$SDKPath" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed: $cmakeOutput" }
        Write-Success "CMake configuration complete"
        
        # Build
        Write-Info "Building Release configuration..."
        $buildOutput = cmake --build . --config Release --parallel 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $buildOutput" }
        Write-Success "Build complete"
        
        return "$PSScriptRoot\build\bin\Release\dxgi.dll"
    }
    finally {
        Pop-Location
    }
}

function Invoke-BuildWithCL($SDKPath) {
    $vs = Find-VisualStudio
    if (-not $vs) { throw "Visual Studio not found" }
    
    # Setup environment
    $env:Path = "$(Split-Path $vs.VCVars);$env:Path"
    
    # Run vcvars64
    $tempBatch = [System.IO.Path]::GetTempFileName() + ".bat"
    @"
@echo off
call "$(Split-Path $vs.VCVars)\vcvars64.bat" >nul 2>&1
set
"@ | Out-File -FilePath $tempBatch -Encoding ASCII
    
    $envVars = cmd /c "$tempBatch"
    Remove-Item $tempBatch
    
    foreach ($line in $envVars) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
    
    # Create bin directory
    if (-not (Test-Path "bin")) { New-Item -ItemType Directory -Force -Path "bin" | Out-Null }
    
    # Build
    Write-Info "Compiling with CL.exe..."
    
    $clArgs = @(
        "/LD", "/EHsc", "/std:c++17", "/O2",
        "/DUNICODE", "/D_UNICODE", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
        "/I.", "/I`"$($SDKPath)\include`"",
        "/Fe:bin\dxgi.dll",
        "main.cpp",
        "src\proxy.cpp",
        "src\dxgi_wrappers.cpp",
        "src\d3d12_wrappers.cpp",
        "src\streamline_integration.cpp",
        "src\resource_detector.cpp",
        "src\crash_handler.cpp",
        "src\config_manager.cpp",
        "src\overlay.cpp",
        "src\ngx_wrapper.cpp",
        "src\hooks.cpp",
        "src\input_handler.cpp",
        "src\pattern_scanner.cpp",
        "src\iat_utils.cpp",
        "/link",
        "d3d12.lib", "dxgi.lib", "dxguid.lib", "user32.lib", "dbghelp.lib", "gdi32.lib", "shell32.lib", "comctl32.lib",
        "`"$($SDKPath)\lib\x64\sl.interposer.lib`"",
        "/DEF:dxgi.def",
        "/DLL"
    )
    
    $clOutput = & cl.exe @clArgs 2>&1
    if ($LASTEXITCODE -ne 0) { 
        $clOutput | Out-File $Script:LogFile
        throw "Build failed. See $Script:LogFile for details."
    }
    
    return "$PSScriptRoot\bin\dxgi.dll"
}

function Copy-ModFiles($GamePath) {
    $sourceFiles = @(
        "bin\dxgi.dll",
        "bin\sl.interposer.dll",
        "bin\sl.common.dll",
        "bin\sl.dlss.dll",
        "bin\sl.dlss_g.dll",
        "bin\sl.reflex.dll",
        "bin\nvngx_dlss.dll",
        "bin\nvngx_dlssg.dll"
    )
    
    Write-Header "Deploying Mod Files"
    Write-Info "Destination: $GamePath"
    Write-Host ""
    
    $copied = 0
    foreach ($file in $sourceFiles) {
        $filename = Split-Path $file -Leaf
        if (Test-Path $file) {
            try {
                Copy-Item -Path $file -Destination $GamePath -Force
                Write-Success $filename
                $copied++
            } catch {
                Write-Error "$filename - $($_.Exception.Message)"
            }
        } else {
            Write-Warning "$filename - Not found"
        }
    }
    
    Write-Host ""
    Write-Success "Deployed $copied files"
}

function New-DesktopShortcut($GamePath) {
    $WshShell = New-Object -ComObject WScript.Shell
    $Shortcut = $WshShell.CreateShortcut("$env:USERPROFILE\Desktop\AC Valhalla DLSS.lnk")
    $Shortcut.TargetPath = "$GamePath\ACValhalla.exe"
    $Shortcut.WorkingDirectory = $GamePath
    $Shortcut.IconLocation = "$GamePath\ACValhalla.exe,0"
    $Shortcut.Description = "Assassin's Creed Valhalla with DLSS 4.5 Mod"
    $Shortcut.Save()
    Write-Success "Desktop shortcut created"
}

function Show-PostInstallInstructions {
    Write-Header "Installation Complete!"
    
    Write-Host @"

    Your game is now ready with DLSS 4.5!

    CONTROLS (In-Game):
    ===================
    F5  - Open DLSS Control Panel
    F6  - Toggle FPS Counter
    F7  - Toggle Vignette Overlay
    F8  - Debug Camera Status

    RECOMMENDED SETTINGS:
    =====================
    1. Launch the game
    2. Press F5 to open the DLSS menu
    3. Set Frame Generation to 4x (or 3x for RTX 40 series)
    4. Set DLSS Mode to Performance or Balanced
    5. In game video settings, set Resolution Scale to 50%
    6. Enjoy 200+ FPS!

    NOTES:
    ======
    - First launch may take longer as shaders compile
    - If the game crashes, disable overlays (Ubisoft Connect, Discord, etc.)
    - Check dlss4_proxy.log in the game folder for debug info
    - Mod settings are saved to dlss_settings.ini

    SUPPORT:
    ========
    If you have issues:
    1. Update GPU drivers to 560.94 or newer
    2. Disable other overlays temporarily
    3. Run game in Borderless Windowed mode
    4. Check the log file for errors

"@ -ForegroundColor $ColorNormal
}

# ============================================================================
# MAIN SCRIPT
# ============================================================================

Clear-Host
Write-Header "$Script:ModName v$Script:ModVersion"

# Check for admin (optional but helpful)
if (-not (Test-Admin)) {
    Write-Warning "Running without admin privileges (this is OK)"
}

# Step 1: Verify SDK
Write-Header "Step 1: Verifying NVIDIA Streamline SDK"
$sdkCheck = Test-StreamlineSDK
if (-not $sdkCheck.Success) {
    Write-Error $sdkCheck.Message
    Write-Host ""
    Write-Host "Please download the SDK from:" -ForegroundColor $ColorWarning
    Write-Host "https://developer.nvidia.com/rtx/streamline" -ForegroundColor $ColorInfo
    Write-Host ""
    Write-Host "Extract it to: $env:USERPROFILE\Downloads\streamline-sdk-v2.10.3" -ForegroundColor $ColorWarning
    exit 1
}
Write-Success "Found SDK: $($sdkCheck.Path)"

# Step 2: Find Game
Write-Header "Step 2: Finding Assassin's Creed Valhalla"
$gamePath = Find-GameInstallation -CustomPath $CustomGamePath

if (-not $gamePath) {
    Write-Error "Game not found automatically"
    Write-Host ""
    Write-Host "Please provide the game path:" -ForegroundColor $ColorWarning
    Write-Host "Example: .\BuildAndDeploy.ps1 -CustomGamePath 'C:\Games\AC Valhalla'" -ForegroundColor $ColorInfo
    Write-Host ""
    
    $manualPath = Read-Host "Enter game folder path (or press Enter to exit)"
    if ([string]::IsNullOrWhiteSpace($manualPath)) { exit 1 }
    
    if (-not (Test-Path "$manualPath\ACValhalla.exe")) {
        Write-Error "ACValhalla.exe not found in: $manualPath"
        exit 1
    }
    $gamePath = $manualPath
}

Write-Success "Found game: $gamePath"
Save-GamePath $gamePath

# Step 3: Build
if (-not $SkipBuild) {
    Write-Header "Step 3: Building Mod"
    
    # Check for CMake
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        Write-Info "Using CMake build system"
        try {
            $builtDll = Invoke-BuildWithMSBuild -SDKPath $sdkCheck.Path
        } catch {
            Write-Warning "CMake build failed, falling back to CL.exe"
            $builtDll = Invoke-BuildWithCL -SDKPath $sdkCheck.Path
        }
    } else {
        Write-Info "Using CL.exe build system"
        $builtDll = Invoke-BuildWithCL -SDKPath $sdkCheck.Path
    }
    
    Write-Success "Build successful: $builtDll"
} else {
    Write-Info "Skipping build (using existing files)"
}

# Step 4: Deploy
Write-Header "Step 4: Deploying to Game Folder"
Copy-ModFiles -GamePath $gamePath

# Step 5: Create shortcut
if ($CreateShortcut) {
    Write-Header "Step 5: Creating Shortcut"
    New-DesktopShortcut -GamePath $gamePath
}

# Step 6: Instructions
Show-PostInstallInstructions

Write-Host "Press any key to exit..." -ForegroundColor $ColorInfo
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
