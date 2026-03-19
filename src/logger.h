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

#include "dlss4_config.h"
#include <memory>
#include <source_location>
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
          "acv-dlss", file_sink, spdlog::thread_pool(),
          spdlog::async_overflow_policy::block);

      spdlog::set_default_logger(logger);
      spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");

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
    } catch (const spdlog::spdlog_ex &) {
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

// ============================================================================
// Macro-based Source-Location-Aware Logging
// ============================================================================
// Uses spdlog's built-in macro logic to avoid template resolution errors
// across compilers while still capturing file/line/function exactly.
// ============================================================================

#define LOG_INFO(...)  SPDLOG_LOGGER_INFO(spdlog::default_logger(), __VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_LOGGER_WARN(spdlog::default_logger(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(spdlog::default_logger(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(spdlog::default_logger(), __VA_ARGS__)
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(spdlog::default_logger(), __VA_ARGS__)
