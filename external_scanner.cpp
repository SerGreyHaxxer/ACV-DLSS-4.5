#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <thread>

#pragma comment(lib, "psapi.lib")

struct Matrix4x4 { float m[4][4]; };

bool IsValid(float f) { return std::isfinite(f) && !std::isnan(f); }

bool IsViewMatrix(const Matrix4x4& mat) {
    for(int i=0; i<16; ++i) if(!IsValid(((float*)&mat)[i])) return false;
    if (abs(mat.m[0][3]) > 0.01f || abs(mat.m[1][3]) > 0.01f || abs(mat.m[2][3]) > 0.01f || abs(mat.m[3][3] - 1.0f) > 0.01f) return false;
    float tx = mat.m[3][0], ty = mat.m[3][1], tz = mat.m[3][2];
    float distSq = tx*tx + ty*ty + tz*tz;
    if (distSq < 100.0f || distSq > 1.0e14f) return false;
    return true;
}

DWORD GetProcId(const wchar_t* procName) {
    DWORD procId = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do { if (!_wcsicmp(pe.szExeFile, procName)) { procId = pe.th32ProcessID; break; } } while (Process32NextW(hSnap, &pe));
        }
    }
    CloseHandle(hSnap);
    return procId;
}

int main() {
    std::cout << "HEADLESS SCANNER STARTING...\n";
    
    DWORD pid = GetProcId(L"ACValhalla.exe");
    if (!pid) { std::cout << "ERROR: ACValhalla.exe not found.\n"; return 1; }
    
    HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) { std::cout << "ERROR: Failed to open process.\n"; return 1; }

    SYSTEM_INFO sysInfo; GetSystemInfo(&sysInfo);
    uintptr_t currentAddr = (uintptr_t)sysInfo.lpMinimumApplicationAddress;
    uintptr_t maxAddr = (uintptr_t)sysInfo.lpMaximumApplicationAddress;
    
    uintptr_t cameraAddr = 0;
    std::vector<uint8_t> buffer;

    std::cout << "Scanning for Camera...\n";
    while (currentAddr < maxAddr && !cameraAddr) {
        MEMORY_BASIC_INFORMATION memInfo;
        if (VirtualQueryEx(hProcess, (LPCVOID)currentAddr, &memInfo, sizeof(memInfo))) {
            if (memInfo.State == MEM_COMMIT && (memInfo.Protect & (PAGE_READWRITE|PAGE_EXECUTE_READWRITE))) {
                buffer.resize(memInfo.RegionSize);
                SIZE_T br;
                if (ReadProcessMemory(hProcess, memInfo.BaseAddress, buffer.data(), memInfo.RegionSize, &br)) {
                    for (size_t i = 0; i < br - sizeof(Matrix4x4); i += 8) {
                        if (IsViewMatrix(*(Matrix4x4*)&buffer[i])) {
                            cameraAddr = (uintptr_t)memInfo.BaseAddress + i;
                            break;
                        }
                    }
                }
            }
            currentAddr += memInfo.RegionSize;
        } else currentAddr += 4096;
    }

    uintptr_t jitterAddr = 0;
    if (cameraAddr) {
        std::cout << "Camera found at 0x" << std::hex << cameraAddr << ". Scanning for Jitter (Keep moving camera!)...\n";
        const size_t RANGE = 128 * 1024 * 1024;
        uintptr_t start = (cameraAddr > RANGE) ? cameraAddr - RANGE : 0;
        std::vector<uint8_t> dump1(RANGE*2), dump2(RANGE*2);
        
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::cout << "Attempt " << (attempt+1) << "/10...\n";
            SIZE_T br;
            ReadProcessMemory(hProcess, (LPCVOID)start, dump1.data(), RANGE*2, &br);
            Sleep(500); // 500ms wait
            ReadProcessMemory(hProcess, (LPCVOID)start, dump2.data(), RANGE*2, &br);
            
            for (size_t i = 0; i < (RANGE*2) - 8; i += 4) {
                float x1 = *(float*)&dump1[i], y1 = *(float*)&dump1[i+4];
                float x2 = *(float*)&dump2[i], y2 = *(float*)&dump2[i+4];
                
                // Check if changed and is valid small float (Jitter is usually < 1.0)
                if ((x1 != x2 || y1 != y2) && 
                    abs(x1) < 1.5f && abs(y1) < 1.5f && 
                    (abs(x1) > 1e-6f || abs(y1) > 1e-6f)) {
                    
                    jitterAddr = start + i; 
                    goto FoundJitter;
                }
            }
        }
    }
FoundJitter:

    std::ofstream report("scanner_report.txt");
    report << "CAMERA_ADDR=0x" << std::hex << cameraAddr << "\n";
    report << "JITTER_ADDR=0x" << std::hex << jitterAddr << "\n";
    report.close();

    std::cout << "DONE. Report saved to scanner_report.txt\n";
    CloseHandle(hProcess);
    return 0;
}
