@echo off
REM ============================================================================
REM DLSS 4 Proxy DLL - Build Script for Visual Studio
REM ============================================================================
REM This script builds the proxy DLL using Visual Studio's compiler.
REM Run from Visual Studio Developer Command Prompt, or it will auto-detect VS.
REM ============================================================================

setlocal enabledelayedexpansion

echo ============================================
echo DLSS 4 Proxy DLL Build Script
echo ============================================

REM Check for NVIDIA Streamline SDK
set "STREAMLINE_SDK=%USERPROFILE%\Downloads\streamline-sdk-v2.10.3"
if not exist "%STREAMLINE_SDK%\lib\x64\sl.interposer.lib" (
    echo ERROR: NVIDIA Streamline SDK not found!
    echo Expected at: %STREAMLINE_SDK%
    echo.
    echo Please download the SDK from:
    echo https://developer.nvidia.com/rtx/streamline
    echo.
    echo Or update this script with the correct path.
    exit /b 1
)
echo Found Streamline SDK: %STREAMLINE_SDK%

REM Check if cl.exe is available
where cl >nul 2>&1
if %errorlevel% equ 0 (
    echo Compiler found in PATH
    goto :build
)

REM Try to find Visual Studio
echo Looking for Visual Studio installation...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)

if defined VS_PATH (
    echo Found Visual Studio at: !VS_PATH!
    call "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat"
    goto :build
)

REM Try common VS paths
for %%V in (2022 2019 2017) do (
    for %%E in (Enterprise Professional Community BuildTools) do (
        set "TRYPATH=C:\Program Files\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if exist "!TRYPATH!" (
            echo Found VS %%V %%E
            call "!TRYPATH!"
            goto :build
        )
        set "TRYPATH=C:\Program Files (x86)\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if exist "!TRYPATH!" (
            echo Found VS %%V %%E (x86)
            call "!TRYPATH!"
            goto :build
        )
    )
)

echo ERROR: Could not find Visual Studio installation!
echo Please run this script from a Visual Studio Developer Command Prompt.
echo Or install Visual Studio with C++ workload.
exit /b 1

:build
echo.
echo Building DLSS 4 Proxy DLL...
echo.

REM Create output directory
if not exist "bin" mkdir bin

REM Compile all source files
cl /LD /EHsc /std:c++17 /O2 ^
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /I. ^
    /I"local_headers" ^
    /I"%USERPROFILE%\Downloads\streamline-sdk-v2.10.3\include" ^
    /Fe:bin\dxgi.dll ^
    main.cpp ^
    src\proxy.cpp ^
    src\dxgi_wrappers.cpp ^
    src\d3d12_wrappers.cpp ^
    src\streamline_integration.cpp ^
    src\resource_detector.cpp ^
    src\crash_handler.cpp ^
    src\config_manager.cpp ^
    src\overlay.cpp ^
    src\ngx_wrapper.cpp ^
    src\hooks.cpp ^
    src\input_handler.cpp ^
    src\pattern_scanner.cpp ^
    src\iat_utils.cpp ^
    /link ^
    d3d12.lib dxgi.lib dxguid.lib user32.lib dbghelp.lib gdi32.lib shell32.lib ^
    "%USERPROFILE%\Downloads\streamline-sdk-v2.10.3\lib\x64\sl.interposer.lib" ^
    /DEF:dxgi.def ^
    /DLL

if %errorlevel% equ 0 (
    echo.
    echo ============================================
    echo BUILD SUCCESSFUL!
    echo ============================================
    echo Output: bin\dxgi.dll
    echo.
    echo Next steps:
    echo   1. Copy bin\dxgi.dll to AC Valhalla folder
    echo   2. Add nvngx_dlss.dll and nvngx_dlssg.dll
    echo   3. Run the game!
    echo ============================================
) else (
    echo.
    echo BUILD FAILED! Check errors above.
)