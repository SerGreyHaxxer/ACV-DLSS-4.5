#pragma once

#include "dlss4_config.h"
#include <memory>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <string>


class Logger {
public:
  static Logger &Instance() {
    static Logger instance;
    return instance;
  }

  static bool Initialize(const std::string &filename) {
    try {
      auto file_sink =
          std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
      auto logger = std::make_shared<spdlog::async_logger>(
          "tensor-curie", file_sink, spdlog::thread_pool(),
          spdlog::async_overflow_policy::block);

      spdlog::set_default_logger(logger);
      spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

#ifdef _DEBUG
      spdlog::set_level(spdlog::level::debug);
#else
      spdlog::set_level(spdlog::level::info);
#endif

      spdlog::info("==============================================");
      spdlog::info("DLSS 4 Proxy v{} Initialized (Modern)",
                   dlss4::kProxyVersion);
      spdlog::info("==============================================");

      return true;
    } catch (const spdlog::spdlog_ex &ex) {
      return false;
    }
  }

  static void Shutdown() { spdlog::shutdown(); }

  void Close(bool flush = true) {
    if (flush) {
      spdlog::default_logger()->flush();
    }
    Shutdown();
  }

private:
  Logger() = default;
};

int GetLogVerbosity();

#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
