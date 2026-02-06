/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include <windows.h>
#include <cstdint>

// ============================================================================
// SENTINEL CRASH HANDLER - Phase 0: Stability & Safety
// ============================================================================
// A kernel-aware vectored exception handler capable of unwinding through
// Denuvo-obfuscated code and generating comprehensive crash reports.
// ============================================================================

namespace Sentinel {

// Configuration for crash handler behavior
struct Config {
    bool enableFullMemoryDump = false;      // Include heap in minidump (large)
    bool enableStackWalk = true;            // Full stack trace with symbols
    bool enableModuleFiltering = true;      // Only include game + proxy modules
    bool openLogOnCrash = false;            // Auto-open crash log
    const char* dumpPath = nullptr;         // Custom dump path (nullptr = CWD)
    const char* logPath = nullptr;          // Custom log path (nullptr = CWD)
};

// Install the Sentinel crash handler (replaces standard VEH)
// Returns true on success, false if already installed or failed
bool Install(const Config& config = {});

// Uninstall the Sentinel crash handler
void Uninstall();

// Check if Sentinel is currently installed
bool IsInstalled();

// Get current configuration
const Config& GetConfig();

// Manually generate a crash dump (for debugging/testing)
// Returns true if dump was successfully written
bool GenerateManualDump(const char* reason = "Manual dump requested");

// Get the last crash address (useful for diagnostics)
uintptr_t GetLastCrashAddress();

// Get the last exception code
DWORD GetLastExceptionCode();

// Stack frame information for reporting
struct StackFrame {
    uintptr_t address;
    uintptr_t returnAddress;
    uintptr_t framePointer;
    char moduleName[MAX_PATH];
    char symbolName[256];
    uint32_t lineNumber;
    char fileName[MAX_PATH];
};

// Maximum stack frames to capture
constexpr size_t kMaxStackFrames = 64;

// Get captured stack trace from last crash (valid after crash handler runs)
size_t GetCapturedStackTrace(StackFrame* frames, size_t maxFrames);

} // namespace Sentinel

// Legacy compatibility - simple wrappers
inline void InstallSentinelHandler() { Sentinel::Install(); }
inline void UninstallSentinelHandler() { Sentinel::Uninstall(); }

