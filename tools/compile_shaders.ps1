# Tensor-Curie Shader Compiler
# Compiles HLSL shaders to precompiled C++ byte array headers.
# Requires Windows SDK fxc.exe (included with Visual Studio).
#
# Usage: .\tools\compile_shaders.ps1

$ErrorActionPreference = "Stop"

# Find fxc.exe
$fxcSearch = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if (-not $fxcSearch) {
    Write-Host "[ERROR] fxc.exe not found. Install the Windows SDK." -ForegroundColor Red
    exit 1
}
$fxc = $fxcSearch.FullName
Write-Host "[INFO] Using: $fxc" -ForegroundColor Cyan

# --- Compile scanner compute shader ---
$hlslPath = "src\shaders\resource_analysis.hlsl"
$csoPath  = "src\shaders\scanner_cs.cso"
$hdrPath  = "src\shaders\scanner_cs.h"

Write-Host "[BUILD] Compiling $hlslPath -> $csoPath" -ForegroundColor Cyan
& $fxc /T cs_5_0 /E CSMain /O2 /Fo $csoPath $hlslPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Shader compilation failed!" -ForegroundColor Red
    exit 1
}

# Convert CSO to C++ header
$bytes = [System.IO.File]::ReadAllBytes($csoPath)
Write-Host "[INFO] Bytecode size: $($bytes.Length) bytes" -ForegroundColor Green

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("// Auto-generated from resource_analysis.hlsl (cs_5_0, CSMain)")
[void]$sb.AppendLine("// Compiled with fxc.exe /T cs_5_0 /E CSMain /O2")
[void]$sb.AppendLine("// Do NOT edit manually -- regenerate with: tools\compile_shaders.ps1")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const unsigned char g_ScannerCS[] = {")

for ($row = 0; $row -lt $bytes.Length; $row += 16) {
    $end = [Math]::Min($row + 16, $bytes.Length)
    $line = "    "
    for ($col = $row; $col -lt $end; $col++) {
        $line += "0x" + $bytes[$col].ToString("X2")
        if ($col -lt $bytes.Length - 1) { $line += "," }
    }
    [void]$sb.AppendLine($line)
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const unsigned int g_ScannerCS_Size = $($bytes.Length);")

[System.IO.File]::WriteAllText($hdrPath, $sb.ToString())
Write-Host "[OK] Generated $hdrPath ($($bytes.Length) bytes)" -ForegroundColor Green

# Cleanup CSO
Remove-Item $csoPath -Force
Write-Host "[OK] Cleaned up $csoPath" -ForegroundColor Green


# --- Compile SSRT compute shader ---
$hlslPath = "src\shaders\ssrt_compute.hlsl"
$csoPath  = "src\shaders\ssrt_compute.cso"
$hdrPath  = "src\shaders\ssrt_compute.h"

Write-Host "[BUILD] Compiling $hlslPath -> $csoPath" -ForegroundColor Cyan
& $fxc /T cs_5_0 /E CSMain /O2 /Fo $csoPath $hlslPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Shader compilation failed!" -ForegroundColor Red
    exit 1
}

# Convert CSO to C++ header
$bytes = [System.IO.File]::ReadAllBytes($csoPath)
Write-Host "[INFO] Bytecode size: $($bytes.Length) bytes" -ForegroundColor Green

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("// Auto-generated from ssrt_compute.hlsl (cs_5_0, CSMain)")
[void]$sb.AppendLine("// Compiled with fxc.exe /T cs_5_0 /E CSMain /O2")
[void]$sb.AppendLine("// Do NOT edit manually -- regenerate with: tools\compile_shaders.ps1")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const unsigned char g_SSRT_CS[] = {")

for ($row = 0; $row -lt $bytes.Length; $row += 16) {
    $end = [Math]::Min($row + 16, $bytes.Length)
    $line = "    "
    for ($col = $row; $col -lt $end; $col++) {
        $line += "0x" + $bytes[$col].ToString("X2")
        if ($col -lt $bytes.Length - 1) { $line += "," }
    }
    [void]$sb.AppendLine($line)
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("static const unsigned int g_SSRT_CS_Size = $($bytes.Length);")

[System.IO.File]::WriteAllText($hdrPath, $sb.ToString())
Write-Host "[OK] Generated $hdrPath ($($bytes.Length) bytes)" -ForegroundColor Green

# Cleanup CSO
Remove-Item $csoPath -Force
Write-Host "[OK] Cleaned up $csoPath" -ForegroundColor Green
