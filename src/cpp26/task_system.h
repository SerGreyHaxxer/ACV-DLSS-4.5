/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

// ============================================================================
// C++26 POLYFILL: Task System (Async Execution)
// ============================================================================
// Lightweight coroutine-based task system for async work scheduling.
// Uses C++20 coroutines with a custom scheduler.
//
// Usage:
//   Task<int> computeAsync() {
//       co_return 42;
//   }
//
//   TaskSystem::Get().schedule(computeAsync());
//   TaskSystem::Get().runUntilEmpty();
//
// When MSVC adds std::execution:
//   Migrate to senders/receivers model
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>


namespace cpp26 {

// ============================================================================
// TASK PROMISE & HANDLE
// ============================================================================

template <typename T = void> class Task;

// Promise for value-returning tasks
template <typename T> struct TaskPromise {
  std::optional<T> result;
  std::exception_ptr exception;
  std::coroutine_handle<> continuation;

  Task<T> get_return_object();

  std::suspend_always initial_suspend() noexcept { return {}; }

  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<TaskPromise<T>> h) noexcept {
      if (auto cont = h.promise().continuation) {
        return cont;
      }
      return std::noop_coroutine();
    }

    void await_resume() noexcept {}
  };

  FinalAwaiter final_suspend() noexcept { return {}; }

  void return_value(T value) { result = std::move(value); }

  void unhandled_exception() { exception = std::current_exception(); }
};

// Promise for void tasks
template <> struct TaskPromise<void> {
  std::exception_ptr exception;
  std::coroutine_handle<> continuation;

  Task<void> get_return_object();

  std::suspend_always initial_suspend() noexcept { return {}; }

  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<TaskPromise<void>> h) noexcept {
      if (auto cont = h.promise().continuation) {
        return cont;
      }
      return std::noop_coroutine();
    }

    void await_resume() noexcept {}
  };

  FinalAwaiter final_suspend() noexcept { return {}; }

  void return_void() {}

  void unhandled_exception() { exception = std::current_exception(); }
};

// ============================================================================
// TASK COROUTINE TYPE
// ============================================================================

template <typename T> class Task {
public:
  using promise_type = TaskPromise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  Task() = default;
  explicit Task(handle_type h) : m_handle(h) {}

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (m_handle) m_handle.destroy();
      m_handle = other.m_handle;
      other.m_handle = nullptr;
    }
    return *this;
  }

  ~Task() {
    if (m_handle) m_handle.destroy();
  }

  // Awaiter for co_await
  struct Awaiter {
    handle_type handle;

    bool await_ready() noexcept { return handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
      handle.promise().continuation = caller;
      return handle;
    }

    T await_resume() {
      if (handle.promise().exception) {
        std::rethrow_exception(handle.promise().exception);
      }
      if constexpr (!std::is_void_v<T>) {
        return std::move(*handle.promise().result);
      }
    }
  };

  Awaiter operator co_await() && { return Awaiter{m_handle}; }

  // Start task and run to completion
  void start() {
    if (m_handle && !m_handle.done()) {
      m_handle.resume();
    }
  }

  bool done() const { return !m_handle || m_handle.done(); }

  T get() {
    if (!m_handle.done()) {
      throw std::runtime_error("Task not complete");
    }
    if (m_handle.promise().exception) {
      std::rethrow_exception(m_handle.promise().exception);
    }
    if constexpr (!std::is_void_v<T>) {
      return std::move(*m_handle.promise().result);
    }
  }

  handle_type release() {
    auto h = m_handle;
    m_handle = nullptr;
    return h;
  }

private:
  handle_type m_handle = nullptr;
};

template <typename T> Task<T> TaskPromise<T>::get_return_object() {
  return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() {
  return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

// ============================================================================
// TASK SCHEDULER
// ============================================================================

class TaskSystem {
public:
  static TaskSystem& Get() {
    static TaskSystem instance;
    return instance;
  }

  TaskSystem(const TaskSystem&) = delete;
  TaskSystem& operator=(const TaskSystem&) = delete;

  // Schedule a task for execution
  template <typename T> void schedule(Task<T> task) {
    std::lock_guard lock(m_mutex);
    m_readyQueue.push(task.release());
  }

  // Schedule a coroutine handle directly
  void schedule(std::coroutine_handle<> handle) {
    if (handle && !handle.done()) {
      std::lock_guard lock(m_mutex);
      m_readyQueue.push(handle);
    }
  }

  // Run until all scheduled tasks complete
  void runUntilEmpty() {
    while (true) {
      std::coroutine_handle<> task;
      {
        std::lock_guard lock(m_mutex);
        if (m_readyQueue.empty()) break;
        task = m_readyQueue.front();
        m_readyQueue.pop();
      }

      if (task && !task.done()) {
        task.resume();
        // If not done, reschedule
        if (!task.done()) {
          std::lock_guard lock(m_mutex);
          m_readyQueue.push(task);
        }
      }
    }
  }

  // Run one task if available (for event loop integration)
  bool runOne() {
    std::coroutine_handle<> task;
    {
      std::lock_guard lock(m_mutex);
      if (m_readyQueue.empty()) return false;
      task = m_readyQueue.front();
      m_readyQueue.pop();
    }

    if (task && !task.done()) {
      task.resume();
      if (!task.done()) {
        std::lock_guard lock(m_mutex);
        m_readyQueue.push(task);
      }
    }
    return true;
  }

  // Check if there are pending tasks
  bool hasPending() const {
    std::lock_guard lock(m_mutex);
    return !m_readyQueue.empty();
  }

  // Clear all pending tasks
  void clear() {
    std::lock_guard lock(m_mutex);
    while (!m_readyQueue.empty()) {
      auto h = m_readyQueue.front();
      m_readyQueue.pop();
      if (h) h.destroy();
    }
  }

private:
  TaskSystem() = default;
  ~TaskSystem() { clear(); }

  mutable std::mutex m_mutex;
  std::queue<std::coroutine_handle<>> m_readyQueue;
};

// ============================================================================
// THREADING UTILITIES
// ============================================================================

// Simple thread pool for parallel work
class ThreadPool {
public:
  explicit ThreadPool(std::size_t numThreads = std::thread::hardware_concurrency()) : m_stop(false) {
    for (std::size_t i = 0; i < numThreads; ++i) {
      m_workers.emplace_back([this] { workerLoop(); });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    m_cv.notify_all();
    for (auto& worker : m_workers) {
      if (worker.joinable()) worker.join();
    }
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Submit work to the pool
  template <typename Func> void submit(Func&& func) {
    {
      std::lock_guard lock(m_mutex);
      m_tasks.push(std::forward<Func>(func));
    }
    m_cv.notify_one();
  }

private:
  void workerLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stop || !m_tasks.empty(); });

        if (m_stop && m_tasks.empty()) return;

        task = std::move(m_tasks.front());
        m_tasks.pop();
      }
      if (task) task();
    }
  }

  std::vector<std::thread> m_workers;
  std::queue<std::function<void()>> m_tasks;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop;
};

// ============================================================================
// AWAITABLE UTILITIES
// ============================================================================

// Yield control back to scheduler
struct YieldAwaiter {
  bool await_ready() noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) noexcept { TaskSystem::Get().schedule(h); }

  void await_resume() noexcept {}
};

inline YieldAwaiter yield() {
  return {};
}

// Simple sleep awaiter (schedules resume after delay)
class SleepAwaiter {
public:
  explicit SleepAwaiter(std::chrono::milliseconds duration) : m_duration(duration) {}

  bool await_ready() noexcept { return m_duration.count() <= 0; }

  void await_suspend(std::coroutine_handle<> h) {
    // In a real implementation, this would use a timer queue
    // For simplicity, we just spawn a thread
    std::thread([h, dur = m_duration] {
      std::this_thread::sleep_for(dur);
      TaskSystem::Get().schedule(h);
    }).detach();
  }

  void await_resume() noexcept {}

private:
  std::chrono::milliseconds m_duration;
};

inline SleepAwaiter sleep_for(std::chrono::milliseconds duration) {
  return SleepAwaiter(duration);
}

} // namespace cpp26
