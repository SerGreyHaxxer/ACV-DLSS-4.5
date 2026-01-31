@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /LD /EHsc /O2 /std:c++17 /DUNICODE /D_UNICODE /Fe:bin\dxgi.dll dlss4_stable.cpp /link user32.lib /DEF:dxgi.def /DLL
