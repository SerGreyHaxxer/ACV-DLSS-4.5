#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <atomic>
#include <vector>
#include "dlss4_config.h"

// ============================================================================
// ASYNC LOGGING UTILITY
// ============================================================================

class Logger {
private:
    FILE* logFile = nullptr;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::queue<std::string> logQueue;
    std::thread loggerThread;
    std::atomic<bool> running{ false };
    bool initialized = false;

    void WorkerThread() {
        while (running) {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this] { return !logQueue.empty() || !running; });

            while (!logQueue.empty()) {
                std::string msg = std::move(logQueue.front());
                logQueue.pop();
                
                // Release lock while writing to file to allow other threads to queue faster
                lock.unlock();
                
                if (logFile) {
                    fprintf(logFile, "%s\n", msg.c_str());
                }
                
                lock.lock();
            }
            
            if (logFile) fflush(logFile);
        }
    }

public:
    Logger() = default;

    ~Logger() {
        Close();
    }

    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    bool Initialize(const char* filename) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (initialized) return true;
        
        if (fopen_s(&logFile, filename, "w") == 0) {
            initialized = true;
            running = true;
            loggerThread = std::thread(&Logger::WorkerThread, this);

            LogUnsafe("INFO", "==============================================");
            LogUnsafe("INFO", "DLSS 4 Proxy v%s Initialized (Async)", DLSS4_PROXY_VERSION);
            LogUnsafe("INFO", "Frame Gen Multiplier: %dx", DLSS4_FRAME_GEN_MULTIPLIER);
            LogUnsafe("INFO", "==============================================");
            return true;
        }
        return false;
    }

    void Log(const char* level, const char* format, ...) {
        if (!initialized || !ENABLE_LOGGING) return;
        
        // Format message first
        va_list args;
        va_start(args, format);
        
        // Timestamp
        time_t t = time(NULL);
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char timeBuf[32];
        strftime(timeBuf, 32, "%Y-%m-%d %H:%M:%S", &tm_info);
        
        // Determine buffer size
        int size = _vscprintf(format, args) + 1; // +1 for null terminator
        if (size <= 0) { va_end(args); return; }
        
        std::vector<char> buffer(size);
        vsnprintf(buffer.data(), size, format, args);
        va_end(args);

        // Combine into full log line
        char headerBuf[64];
        snprintf(headerBuf, sizeof(headerBuf), "[%s] [%s] ", timeBuf, level);
        
        std::string fullMessage = std::string(headerBuf) + std::string(buffer.data());

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            logQueue.push(std::move(fullMessage));
        }
        queueCV.notify_one();
    }

    void Close(bool flush = true) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!running) return;
            running = false;
        }
        queueCV.notify_all();
        
        if (loggerThread.joinable()) {
            loggerThread.join();
        }

        if (flush) {
            // Flush remaining queue
            while (!logQueue.empty()) {
                if (logFile) fprintf(logFile, "%s\n", logQueue.front().c_str());
                logQueue.pop();
            }

            if (logFile) {
                fprintf(logFile, "[INFO] Logger shutting down.\n");
                fclose(logFile);
                logFile = nullptr;
            }
        } else {
            std::queue<std::string> empty;
            logQueue.swap(empty);
            if (logFile) {
                fclose(logFile);
                logFile = nullptr;
            }
        }
        initialized = false;
    }

private:
    // Helper for initial logs before thread starts (or inside the thread)
    void LogUnsafe(const char* level, const char* format, ...) {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        
        std::string msg = "[INIT] [" + std::string(level) + "] " + std::string(buffer);
        
        // Push directly to queue so the thread handles it uniformly
        // NOTE: Caller (Initialize) must hold queueMutex!
        logQueue.push(msg);
    }
};

int GetLogVerbosity();

#define LOG_INFO(fmt, ...)  do { if (GetLogVerbosity() >= 1) Logger::Instance().Log("INFO", fmt, ##__VA_ARGS__); } while (0)
#define LOG_WARN(fmt, ...)  do { if (GetLogVerbosity() >= 1) Logger::Instance().Log("WARN", fmt, ##__VA_ARGS__); } while (0)
#define LOG_ERROR(fmt, ...) do { Logger::Instance().Log("ERROR", fmt, ##__VA_ARGS__); } while (0)

#if LOG_VERBOSE
#define LOG_DEBUG(fmt, ...) do { if (GetLogVerbosity() >= 2) Logger::Instance().Log("DEBUG", fmt, ##__VA_ARGS__); } while (0)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif
