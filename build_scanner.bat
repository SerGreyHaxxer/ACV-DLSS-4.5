@echo off
cl /EHsc external_scanner.cpp /Fe:ACV_Scanner.exe user32.lib kernel32.lib
if %errorlevel% equ 0 (
    echo.
    echo Scanner built successfully: ACV_Scanner.exe
)
