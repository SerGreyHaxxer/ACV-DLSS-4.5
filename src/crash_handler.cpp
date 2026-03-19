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

// ============================================================================
// LEGACY CRASH HANDLER — thin forwarding to Sentinel (Item 25)
// ============================================================================
// The full VEH implementation has been consolidated into
// sentinel_crash_handler.cpp. This file exists solely for backward
// compatibility of the InstallCrashHandler/UninstallCrashHandler API.
// ============================================================================

#include "crash_handler.h"
#include "sentinel_crash_handler.h"

#include <atomic>
#include <shellapi.h>

static std::atomic<bool> g_LogOpened(false);

void InstallCrashHandler() {
  // Forward to Sentinel — no-op if already installed
  Sentinel::Install();
}

void UninstallCrashHandler() {
  // Forward to Sentinel — no-op if not installed
  Sentinel::Uninstall();
}

void OpenCrashLog() {
  if (g_LogOpened.exchange(true)) return;
  ShellExecuteA(nullptr, "open", "dlss4_crash.log", nullptr, nullptr, SW_SHOW);
}

void OpenMainLog() {
  if (g_LogOpened.exchange(true)) return;
  ShellExecuteA(nullptr, "open", "dlss4_proxy.log", nullptr, nullptr, SW_SHOW);
}
