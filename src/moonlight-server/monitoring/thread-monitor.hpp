#pragma once

#include <atomic>
#include <chrono>
#include <fstream>
#include <helpers/logger.hpp>
#include <mutex>
#include <signal.h>
#include <sstream>
#include <sys/syscall.h>
#include <sys/wait.h>
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

  // HTTP request tracking
  std::string current_request_path;
  std::chrono::steady_clock::time_point request_start_time;
  bool has_active_request{false};
};

/**
 * Global thread monitor for detecting stuck/dead threads
 * Tracks heartbeats from critical threads (HTTPS, HTTP, RTSP, GStreamer pipelines)
 */
class ThreadMonitor {
private:
  friend class ScopedThreadMonitor;  // Allow ScopedThreadMonitor to access private members
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
    heartbeat_for_tid(tid);
  }

  // Update heartbeat for specific thread (by TID)
  void heartbeat_for_tid(pid_t tid) {
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

    // HTTP request tracking
    std::string current_request_path;
    int64_t request_duration_seconds;
    bool has_active_request;

    // Kernel stack trace (where thread is blocked/executing)
    std::string stack_trace;
  };

  std::vector<ThreadStatus> get_all_threads() {
    std::vector<ThreadStatus> result;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    for (const auto& [tid, info] : threads_) {
      auto last_hb = info->last_heartbeat.load();
      auto since_heartbeat = std::chrono::duration_cast<std::chrono::seconds>(now - last_hb).count();
      auto alive = std::chrono::duration_cast<std::chrono::seconds>(now - info->created_at).count();

      int64_t request_duration = 0;
      if (info->has_active_request) {
        request_duration = std::chrono::duration_cast<std::chrono::seconds>(now - info->request_start_time).count();
      }

      // A thread is only "stuck" if it WAS heartbeating and then stopped
      // Threads that never heartbeat (heartbeat_count = 0) are not considered stuck
      bool is_stuck = (info->heartbeat_count.load() > 0) && (since_heartbeat > 30);

      // Read current syscall from /proc (shows what kernel call thread is blocked in)
      // Format: "<syscall_nr> <arg1> <arg2> ... <sp> <pc>"
      // Common syscalls: 1=write, 7=poll, 14=rt_sigtimedwait, 202=futex, 232=epoll_wait
      std::string stack_trace;  // Reuse field name for syscall info
      std::string syscall_path = "/proc/self/task/" + std::to_string(tid) + "/syscall";
      std::ifstream syscall_file(syscall_path);
      if (syscall_file.is_open()) {
        std::string syscall_line;
        std::getline(syscall_file, syscall_line);
        if (!syscall_line.empty()) {
          // Extract syscall number and format nicely
          std::istringstream iss(syscall_line);
          long syscall_nr;
          if (iss >> syscall_nr) {
            // Map common syscalls to names for readability
            static const std::unordered_map<long, std::string> syscall_names = {
              {0, "read"}, {1, "write"}, {7, "poll"}, {14, "rt_sigtimedwait"},
              {202, "futex"}, {232, "epoll_wait"}, {271, "ppoll"}
            };
            auto it = syscall_names.find(syscall_nr);
            if (it != syscall_names.end()) {
              stack_trace = it->second + " (" + std::to_string(syscall_nr) + ")";
            } else {
              stack_trace = "syscall " + std::to_string(syscall_nr);
            }
          }
        }
      }

      result.push_back(ThreadStatus{
        .tid = tid,
        .name = info->name,
        .pipeline_desc = info->pipeline_desc.substr(0, 80),
        .seconds_since_heartbeat = since_heartbeat,
        .seconds_alive = alive,
        .heartbeat_count = info->heartbeat_count.load(),
        .is_stuck = is_stuck,
        .current_request_path = info->current_request_path,
        .request_duration_seconds = request_duration,
        .has_active_request = info->has_active_request,
        .stack_trace = stack_trace
      });
    }

    return result;
  }

  // Start tracking an HTTP request
  void start_request(const std::string& path) {
    pid_t tid = syscall(SYS_gettid);
    std::lock_guard lock(mutex_);
    auto it = threads_.find(tid);
    if (it != threads_.end()) {
      it->second->current_request_path = path;
      it->second->request_start_time = std::chrono::steady_clock::now();
      it->second->has_active_request = true;
    }
  }

  // End tracking an HTTP request
  void end_request() {
    pid_t tid = syscall(SYS_gettid);
    std::lock_guard lock(mutex_);
    auto it = threads_.find(tid);
    if (it != threads_.end()) {
      it->second->has_active_request = false;
      it->second->current_request_path = "";
    }
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

  /**
   * Test if new GStreamer pipelines can be created (deadlock detection)
   *
   * Production deadlock: Global GLib type lock held by crashed thread
   * → gst_element_factory_make() blocks → new sessions can't start
   *
   * This test detects the ACTUAL failure condition, not arbitrary thread percentages.
   * Uses fork + timeout to avoid hanging the health check itself.
   *
   * @return true if pipeline creation works, false if deadlocked
   */
  static bool can_create_new_pipelines() {
    // Fork child to test creation (timeout protection)
    pid_t child = fork();

    if (child == 0) {
      // CHILD PROCESS: Try to create element (requires global GLib type lock)
      alarm(5);  // Kill child if it hangs >5s

      GstElement* test = gst_element_factory_make("fakesrc", nullptr);
      if (test) {
        gst_object_unref(test);
        _exit(0);  // Success - type lock available
      }
      _exit(1);  // Failed to create (shouldn't happen for fakesrc)
    }

    // PARENT PROCESS: Wait for child with timeout
    int status;
    auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(6);  // 6s max (5s alarm + 1s grace)

    while (std::chrono::steady_clock::now() - start < timeout) {
      pid_t result = waitpid(child, &status, WNOHANG);
      if (result == child) {
        // Child exited
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      } else if (result == -1) {
        // Error
        return false;
      }
      // Child still running, sleep briefly
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Child timed out - kill it and return false
    kill(child, SIGKILL);
    waitpid(child, &status, 0);  // Clean up zombie
    return false;  // Type lock is held - new sessions WON'T work
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
