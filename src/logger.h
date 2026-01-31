#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "dlss4_config.h"

// ============================================================================
// LOGGING UTILITY (Loader-Safe C-Style)
// ============================================================================

class Logger {
private:
    FILE* logFile = nullptr;
    CRITICAL_SECTION logCS;
    bool initialized = false;

public:
    Logger() {
        InitializeCriticalSection(&logCS);
    }

    ~Logger() {
        Close();
        DeleteCriticalSection(&logCS);
    }

    static Logger& Instance() {
        // Use a pointer to avoid "magic static" thread-safe init (which locks)
        static Logger* instance = new Logger();
        return *instance;
    }

    bool Initialize(const char* filename) {
        EnterCriticalSection(&logCS);
        if (initialized) {
            LeaveCriticalSection(&logCS);
            return true;
        }
        
        if (fopen_s(&logFile, filename, "w") == 0) {
            initialized = true;
            LogUnsafe("INFO", "==============================================");
            LogUnsafe("INFO", "DLSS 4 Proxy v%s Initialized", DLSS4_PROXY_VERSION);
            LogUnsafe("INFO", "Frame Gen Multiplier: %dx", DLSS4_FRAME_GEN_MULTIPLIER);
            LogUnsafe("INFO", "==============================================");
            LeaveCriticalSection(&logCS);
            return true;
        }
        
        LeaveCriticalSection(&logCS);
        return false;
    }

    void Log(const char* level, const char* format, ...) {
        if (!initialized || !ENABLE_LOGGING) return;
        
        EnterCriticalSection(&logCS);
        
        va_list args;
        va_start(args, format);
        
        // Timestamp
        time_t t = time(NULL);
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char timeBuf[32];
        strftime(timeBuf, 32, "%Y-%m-%d %H:%M:%S", &tm_info);
        
        fprintf(logFile, "[%s] [%s] ", timeBuf, level);
        vfprintf(logFile, format, args);
        fprintf(logFile, "\n");
        fflush(logFile);
        
        va_end(args);
        
        LeaveCriticalSection(&logCS);
    }

    void Close() {
        EnterCriticalSection(&logCS);
        if (logFile) {
            fprintf(logFile, "[INFO] Logger shutting down.\n");
            fclose(logFile);
            logFile = nullptr;
        }
        initialized = false;
        LeaveCriticalSection(&logCS);
    }

private:
    void LogUnsafe(const char* level, const char* format, ...) {
        if (!logFile) return;
        va_list args;
        va_start(args, format);
        fprintf(logFile, "[INIT] [%s] ", level);
        vfprintf(logFile, format, args);
        fprintf(logFile, "\n");
        fflush(logFile);
        va_end(args);
    }
};

#define LOG_INFO(fmt, ...)  Logger::Instance().Log("INFO", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::Instance().Log("WARN", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::Instance().Log("ERROR", fmt, ##__VA_ARGS__)

#if LOG_VERBOSE
#define LOG_DEBUG(fmt, ...) Logger::Instance().Log("DEBUG", fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif
