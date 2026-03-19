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
// C++20 Source-Location-Aware Logging
// ============================================================================
// These wrappers capture file_name(), line(), and function_name() at each
// call site via std::source_location::current() in a consteval constructor.
// The result: every log line includes the originating file:line in a deeply
// injected proxy, making debugging trivially easy.
// ============================================================================

namespace detail {

// Format-string wrapper that captures source_location at the call site.
// The consteval constructor ensures source_location::current() resolves to
// the actual caller, not this template.
template <typename... Args>
struct LocFmt {
    fmt::format_string<Args...> fmt;
    std::source_location loc;

    // consteval ensures this is resolved at compile time at the call site
    template <typename S, typename = std::enable_if_t<std::is_constructible_v<fmt::format_string<Args...>, S>>>
    consteval LocFmt(const S& s,
                     std::source_location loc = std::source_location::current())
        : fmt(s), loc(loc) {}
};

struct Log {
    template <typename... Args>
    static void Info(LocFmt<Args...> str, Args&&... args) {
        spdlog::default_logger()->log(
            spdlog::source_loc{str.loc.file_name(),
                               static_cast<int>(str.loc.line()),
                               str.loc.function_name()},
            spdlog::level::info, str.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Warn(LocFmt<Args...> str, Args&&... args) {
        spdlog::default_logger()->log(
            spdlog::source_loc{str.loc.file_name(),
                               static_cast<int>(str.loc.line()),
                               str.loc.function_name()},
            spdlog::level::warn, str.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Error(LocFmt<Args...> str, Args&&... args) {
        spdlog::default_logger()->log(
            spdlog::source_loc{str.loc.file_name(),
                               static_cast<int>(str.loc.line()),
                               str.loc.function_name()},
            spdlog::level::err, str.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Debug(LocFmt<Args...> str, Args&&... args) {
        spdlog::default_logger()->log(
            spdlog::source_loc{str.loc.file_name(),
                               static_cast<int>(str.loc.line()),
                               str.loc.function_name()},
            spdlog::level::debug, str.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Trace(LocFmt<Args...> str, Args&&... args) {
        spdlog::default_logger()->log(
            spdlog::source_loc{str.loc.file_name(),
                               static_cast<int>(str.loc.line()),
                               str.loc.function_name()},
            spdlog::level::trace, str.fmt, std::forward<Args>(args)...);
    }
};

} // namespace detail

// Seamless macro replacement — same API, now with automatic source location
#define LOG_INFO(...) detail::Log::Info(__VA_ARGS__)
#define LOG_WARN(...) detail::Log::Warn(__VA_ARGS__)
#define LOG_ERROR(...) detail::Log::Error(__VA_ARGS__)
#define LOG_DEBUG(...) detail::Log::Debug(__VA_ARGS__)
#define LOG_TRACE(...) detail::Log::Trace(__VA_ARGS__)
