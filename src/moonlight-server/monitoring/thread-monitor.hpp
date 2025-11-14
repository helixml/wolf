#pragma once

#include <atomic>
#include <chrono>
#include <helpers/logger.hpp>
#include <mutex>
#include <sys/syscall.h>
#include <unordered_map>
#include <unistd.h>

namespace wolf::monitoring {

struct ThreadInfo {
  pid_t tid;
  std::string name;
  std::string pipeline_desc;  // For GStreamer threads
  std::atomic<std::chrono::steady_clock::time_point> last_heartbeat;
  std::chrono::steady_clock::time_point created_at;
  std::atomic<uint64_t> heartbeat_count{0};
};

/**
 * Global thread monitor for detecting stuck/dead threads
 * Tracks heartbeats from critical threads (HTTPS, HTTP, RTSP, GStreamer pipelines)
 */
class ThreadMonitor {
private:
  static ThreadMonitor instance_;
  std::mutex mutex_;
  std::unordered_map<pid_t, std::shared_ptr<ThreadInfo>> threads_;

  ThreadMonitor() = default;

public:
  static ThreadMonitor& get() { return instance_; }

  // Register a thread for monitoring
  void register_thread(const std::string& name, const std::string& pipeline_desc = "") {
    pid_t tid = syscall(SYS_gettid);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    auto info = std::make_shared<ThreadInfo>();
    info->tid = tid;
    info->name = name;
    info->pipeline_desc = pipeline_desc;
    info->last_heartbeat.store(now);
    info->created_at = now;
    info->heartbeat_count = 0;

    threads_[tid] = info;
    logs::log(logs::info, "[THREAD_MONITOR] Registered thread: TID={} name={}", tid, name);
  }

  // Update heartbeat for calling thread
  void heartbeat() {
    pid_t tid = syscall(SYS_gettid);

    std::lock_guard lock(mutex_);
    auto it = threads_.find(tid);
    if (it != threads_.end()) {
      it->second->last_heartbeat.store(std::chrono::steady_clock::now());
      it->second->heartbeat_count++;
    }
  }

  // Unregister thread (call before thread exits)
  void unregister_thread() {
    pid_t tid = syscall(SYS_gettid);

    std::lock_guard lock(mutex_);
    auto it = threads_.find(tid);
    if (it != threads_.end()) {
      logs::log(logs::info, "[THREAD_MONITOR] Unregistered thread: TID={} name={} (lived {}s, {} heartbeats)",
                tid,
                it->second->name,
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - it->second->created_at).count(),
                it->second->heartbeat_count.load());
      threads_.erase(it);
    }
  }

  // Get all threads and their status
  struct ThreadStatus {
    pid_t tid;
    std::string name;
    std::string pipeline_desc;
    int64_t seconds_since_heartbeat;
    int64_t seconds_alive;
    uint64_t heartbeat_count;
    bool is_stuck;  // >30s since last heartbeat
  };

  std::vector<ThreadStatus> get_all_threads() {
    std::vector<ThreadStatus> result;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    for (const auto& [tid, info] : threads_) {
      auto last_hb = info->last_heartbeat.load();
      auto since_heartbeat = std::chrono::duration_cast<std::chrono::seconds>(now - last_hb).count();
      auto alive = std::chrono::duration_cast<std::chrono::seconds>(now - info->created_at).count();

      result.push_back(ThreadStatus{
        .tid = tid,
        .name = info->name,
        .pipeline_desc = info->pipeline_desc.substr(0, 80),
        .seconds_since_heartbeat = since_heartbeat,
        .seconds_alive = alive,
        .heartbeat_count = info->heartbeat_count.load(),
        .is_stuck = since_heartbeat > 30
      });
    }

    return result;
  }

  // Check for stuck threads (>30s since heartbeat)
  std::vector<pid_t> get_stuck_threads() {
    std::vector<pid_t> stuck;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    for (const auto& [tid, info] : threads_) {
      auto last_hb = info->last_heartbeat.load();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_hb).count();
      if (elapsed > 30) {
        stuck.push_back(tid);
      }
    }

    return stuck;
  }
};

// Initialize static instance
inline ThreadMonitor ThreadMonitor::instance_;

/**
 * RAII helper for thread lifecycle
 * Automatically registers thread on construction, unregisters on destruction
 */
class ScopedThreadMonitor {
private:
  std::string name_;

public:
  ScopedThreadMonitor(const std::string& name, const std::string& details = "")
      : name_(name) {
    ThreadMonitor::get().register_thread(name, details);
  }

  ~ScopedThreadMonitor() {
    ThreadMonitor::get().unregister_thread();
  }

  void heartbeat() {
    ThreadMonitor::get().heartbeat();
  }
};

} // namespace wolf::monitoring
