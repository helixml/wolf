#include <api/api.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <control/control.hpp>
#include <core/docker.hpp>
#include <core/gstreamer.hpp>
#include <csignal>
#include <exceptions/exceptions.h>
#include <filesystem>
#include <fstream>
#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <introspect/introspect.hpp>
#include <mdns_cpp/logger.hpp>
#include <mdns_cpp/mdns.hpp>
#include <memory>
#include <monitoring/thread-monitor.hpp>
#include <rest/rest.hpp>
#include <rtsp/net.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <streaming/streaming.hpp>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ba = boost::asio;
namespace fs = std::filesystem;

using namespace std::string_literals;
using namespace std::chrono_literals;
using namespace wolf::core;

/**
 * @brief Will try to load the config file and fallback to defaults
 */
auto load_config(std::string_view config_file,
                 const std::shared_ptr<events::EventBusType> &ev_bus,
                 state::SessionsAtoms running_sessions) {
  logs::log(logs::info, "Reading config file from: {}", config_file);
  return state::load_or_default(config_file.data(), ev_bus, running_sessions);
}

state::Host get_host_config(std::string_view pkey_filename, std::string_view cert_filename) {
  x509::x509_ptr server_cert;
  x509::pkey_ptr server_pkey;
  if (x509::cert_exists(pkey_filename, cert_filename)) {
    logs::log(logs::debug, "Loading server certificates from disk: {} {}", cert_filename, pkey_filename);
    server_cert = x509::cert_from_file(cert_filename);
    server_pkey = x509::pkey_from_file(pkey_filename);
  } else {
    logs::log(logs::info, "x509 certificates not present, generating: {} {}", cert_filename, pkey_filename);
    server_pkey = x509::generate_key();
    server_cert = x509::generate_x509(server_pkey);
    x509::write_to_disk(server_pkey, pkey_filename, server_cert, cert_filename);
  }

  std::optional<std::string> internal_ip = std::nullopt;
  if (auto override_ip = utils::get_env("WOLF_INTERNAL_IP")) {
    internal_ip = override_ip;
  }
  std::optional<std::string> mac_address = std::nullopt;
  if (auto override_mac = utils::get_env("WOLF_INTERNAL_MAC")) {
    mac_address = override_mac;
  }

  std::string local_base_state_folder = utils::get_env("HOST_APPS_STATE_FOLDER", "/etc/wolf");
  std::string host_base_state_folder = local_base_state_folder;
  std::string host_xdg_runtime_dir = utils::get_env("XDG_RUNTIME_DIR", "/tmp/sockets");

  docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
  if (auto container = introspect::get_current_container(docker_api)) {
    host_base_state_folder =
        introspect::get_host_path_for(*container, local_base_state_folder).value_or(local_base_state_folder);
    host_xdg_runtime_dir =
        introspect::get_host_path_for(*container, host_xdg_runtime_dir).value_or(host_xdg_runtime_dir);
  } else {
    logs::log(logs::warning,
              "Unable to get the container that is running Wolf, automatic mounts matching is disabled.");
  }

  return {state::DISPLAY_CONFIGURATIONS,
          state::AUDIO_CONFIGURATIONS,
          server_cert,
          server_pkey,
          internal_ip,
          mac_address,
          host_base_state_folder,
          local_base_state_folder,
          host_xdg_runtime_dir};
}

/**
 * @brief Local state initialization
 */
auto initialize(std::string_view config_file, std::string_view pkey_filename, std::string_view cert_filename) {
  auto event_bus = std::make_shared<events::EventBusType>();
  auto running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto config = load_config(config_file, event_bus, running_sessions);

  auto host = get_host_config(pkey_filename, cert_filename);
  auto state = state::AppState{
      .config = config,
      .host = host,
      .pairing_cache = std::make_shared<immer::atom<immer::map<std::string, state::PairCache>>>(),
      .pairing_atom = std::make_shared<immer::atom<immer::map<std::string, immer::box<events::PairSignal>>>>(),
      .event_bus = event_bus,
      .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
      .running_sessions = running_sessions};
  return immer::box<state::AppState>(state);
}

/**
 * We first try to connect to a running PulseAudio server
 * if that fails, we run our own PulseAudio container and connect to it
 * if that fails, we can't return an AudioServer, hence the optional!
 */
std::optional<sessions::AudioServer> setup_audio_server(const std::string &host_runtime_dir,
                                                        const std::string &runtime_dir) {
  auto audio_server = audio::connect();
  if (audio::connected(audio_server)) {
    return {{.server = audio_server}};
  } else {
    logs::log(logs::info, "Starting PulseAudio docker container");
    docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
    auto pulse_socket = fmt::format("{}/pulse-socket", runtime_dir);

    /* Cleanup old leftovers, Pulse will fail to start otherwise */
    try {
      std::filesystem::remove(pulse_socket);
      std::filesystem::remove_all(fmt::format("{}/pulse", runtime_dir));
    } catch (const std::filesystem::filesystem_error &e) {
      logs::log(logs::warning, "Failed to remove old PulseAudio socket: {}", e.what());
    }

    /* Mount low-memory PulseAudio config from Wolf container
     * Config file is at /opt/wolf-defaults/pulse-lowmem.conf in Wolf image
     * This disables shared memory to save ~64MB per session */
    auto pulse_config_path = utils::get_env("WOLF_PULSE_LOWMEM_CONFIG", "/opt/wolf-defaults/pulse-lowmem.conf");
    std::vector<docker::MountPoint> mounts = {
        docker::MountPoint{.source = host_runtime_dir, .destination = "/tmp/pulse/", .mode = "rw"},
        docker::MountPoint{.source = pulse_config_path, .destination = "/etc/pulse/daemon.conf.d/99-wolf-low-memory.conf", .mode = "ro"}};

    auto container = docker_api.create(
        docker::Container{
            .id = "",
            .name = "WolfPulseAudio",
            .image = utils::get_env("WOLF_PULSE_IMAGE", "ghcr.io/games-on-whales/pulseaudio:master"),
            .status = docker::CREATED,
            .ports = {},
            .mounts = mounts,
            .env = {"XDG_RUNTIME_DIR=/tmp/pulse/", "UNAME=retro", "UID=1000", "GID=1000"}},
        // The following is needed when using podman (or any container that uses SELINUX). This way we can access the
        // socket that is created by PulseAudio from other containers (including this one).
        R"({
                  "HostConfig" : {
                    "SecurityOpt" : ["label=disable"]
                  }
            })");
    if (container && docker_api.start_by_id(container.value().id)) {
      auto ms = std::stoi(utils::get_env("WOLF_PULSE_CONTAINER_TIMEOUT_MS", "2000"));
      std::this_thread::sleep_for(std::chrono::milliseconds(ms)); // TODO: Better way of knowing when ready?
      return {{.server = audio::connect(fmt::format("{}/pulse-socket", runtime_dir)), .container = container}};
    }
  }

  logs::log(logs::warning, "Failed to connect to any PulseAudio server, audio will not be available!");

  return {};
}

/**
 * @brief Test if Wolf's Unix socket API is responding
 *
 * Attempts to connect to the socket and make a simple HTTP request.
 * Returns false if socket is dead (connection refused, timeout, etc.)
 * This catches cases where an unhandled exception kills the io_context
 * but the watchdog thread keeps running.
 */
bool test_socket_health(const std::string& socket_path) {
  try {
    boost::asio::io_context io_ctx;

    // Set a 5-second timeout for the entire operation
    boost::asio::local::stream_protocol::socket socket(io_ctx);
    boost::asio::local::stream_protocol::endpoint endpoint(socket_path);

    // Try to connect
    boost::system::error_code ec;
    socket.connect(endpoint, ec);
    if (ec) {
      logs::log(logs::debug, "[WATCHDOG] Socket connect failed: {}", ec.message());
      return false;
    }

    // Send a simple HTTP request
    std::string request = "GET /api/v1/system/health HTTP/1.0\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request), ec);
    if (ec) {
      logs::log(logs::debug, "[WATCHDOG] Socket write failed: {}", ec.message());
      return false;
    }

    // Read response (just need to get some data back)
    std::array<char, 256> buf;
    std::size_t len = socket.read_some(boost::asio::buffer(buf), ec);
    if (ec && ec != boost::asio::error::eof) {
      logs::log(logs::debug, "[WATCHDOG] Socket read failed: {}", ec.message());
      return false;
    }

    // Check for HTTP 200 response
    std::string response(buf.data(), len);
    return response.find("HTTP/1.0 200") != std::string::npos ||
           response.find("HTTP/1.1 200") != std::string::npos;

  } catch (const std::exception& e) {
    logs::log(logs::debug, "[WATCHDOG] Socket health check exception: {}", e.what());
    return false;
  }
}

/**
 * @brief Fail-fast watchdog - exits immediately on any stuck thread
 *
 * Philosophy: Stuck threads indicate deadlock, resource exhaustion, or corruption.
 * They won't self-heal. Limping along in degraded state just delays the inevitable
 * and makes debugging harder. Exit immediately and let Docker restart us cleanly.
 *
 * Runs in separate thread, polls ThreadMonitor every 30s.
 * Exits immediately if:
 * - ANY thread is stuck (hasn't sent heartbeat in 30s)
 * - Pipeline creation fails (GStreamer type lock held)
 * - API socket is dead (io_context crashed)
 *
 * In degraded state, new session creation also fails with:
 *   "Lobby setup timed out" - Wolf API returns 500, can't create wayland compositor or audio sink
 * This is evidence that the stuck threads are blocking shared resources needed for new sessions.
 *
 * Root cause investigation: Race condition suspected when client reconnects while previous
 * session is being cleaned up. Moonlight-web now has cleaning_up flag to reject such requests.
 *
 * Before exiting:
 * 1. Fork child process (timeout protection - if debug gathering deadlocks, child dies after 60s)
 * 2. Write thread dump, generate core dump (timestamped files in /var/wolf-debug-dumps/)
 * 3. Exit main process for Docker restart
 */
void start_watchdog() {
  std::thread([]() {
    try {
      using namespace std::chrono;

      const seconds CHECK_INTERVAL{30};

      // Get socket path from environment
    auto default_socket = std::filesystem::path(utils::get_env("XDG_RUNTIME_DIR", "/var/run/wolf")) / "wolf.sock";
    auto socket_path = utils::get_env("WOLF_SOCKET_PATH", default_socket.c_str());

    logs::log(logs::info, "[WATCHDOG] Started monitoring system health (socket: {})", socket_path);

    while (true) {
      std::this_thread::sleep_for(CHECK_INTERVAL);

      // Test if new pipelines can be created (real failure condition)
      // Production deadlocked with only 35% threads stuck, but new sessions couldn't start
      bool pipelines_work = wolf::monitoring::ThreadMonitor::can_create_new_pipelines();

      // Test if the Unix socket is responding (catches io_context death)
      bool socket_alive = test_socket_health(socket_path);

      // Also check stuck thread count for context
      auto thread_statuses = wolf::monitoring::ThreadMonitor::get().get_all_threads();
      int stuck_count = 0;
      for (const auto& status : thread_statuses) {
        if (status.is_stuck) {
          stuck_count++;
        }
      }

      // FAIL FAST: Any stuck thread means something is fundamentally broken.
      // Stuck threads won't self-heal - they indicate deadlock, resource exhaustion, or corruption.
      // Exit immediately and let Docker restart us cleanly.
      // Also exit if socket is dead (API unreachable) or pipeline creation fails.
      bool should_exit = stuck_count > 0 || !pipelines_work || !socket_alive;

      if (should_exit) {
        std::string reason;
        if (stuck_count > 0) {
          reason = fmt::format("{} thread(s) stuck - fail fast", stuck_count);
        } else if (!socket_alive && !pipelines_work) {
          reason = "socket DEAD + pipeline creation FAILED";
        } else if (!socket_alive) {
          reason = "socket DEAD (API unreachable)";
        } else {
          reason = "pipeline creation FAILED (type lock held)";
        }

        logs::log(logs::fatal,
                  "[WATCHDOG] {} - {}/{} threads total - gathering debug info and exiting immediately",
                  reason, stuck_count, thread_statuses.size());

        // Fork child process for debug gathering (timeout protection)
        pid_t child_pid = fork();

        if (child_pid == 0) {
          // CHILD PROCESS: Gather debug info with 60s timeout
          alarm(60); // Kill child if debug gathering hangs

          try {
            auto now = system_clock::now();
            auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
            std::string debug_dir = "/var/wolf-debug-dumps";
            std::string prefix = fmt::format("{}/{}", debug_dir, timestamp);

            // Create debug dumps directory
            std::filesystem::create_directories(debug_dir);

            // 1. Write thread dump
            std::ofstream thread_dump(prefix + "-threads.txt");
            thread_dump << fmt::format("Wolf Fail-Fast Debug Dump - {}\n", timestamp);
            thread_dump << fmt::format("Reason: {}\n", reason);
            thread_dump << fmt::format("Stuck threads: {}/{}\n\n", stuck_count, thread_statuses.size());

            for (const auto& status : thread_statuses) {
              thread_dump << fmt::format("TID {}: {} ({})\n", status.tid, status.name, status.pipeline_desc);
              thread_dump << fmt::format("  Last heartbeat: {}s ago\n", status.seconds_since_heartbeat);
              thread_dump << fmt::format("  Alive: {}s, Heartbeats: {}\n", status.seconds_alive, status.heartbeat_count);
              thread_dump << fmt::format("  Status: {}\n\n", status.is_stuck ? "STUCK" : "healthy");
            }
            thread_dump.close();

            // 2. Generate core dump using gcore
            std::string gcore_cmd = fmt::format("gcore -o {} {}", prefix, getppid());
            logs::log(logs::info, "[WATCHDOG] Generating core dump: {}", gcore_cmd);
            int gcore_result = system(gcore_cmd.c_str());
            if (gcore_result != 0) {
              logs::log(logs::warning, "[WATCHDOG] gcore failed with code {}", gcore_result);
            }

            // 3. Copy recent logs (last 1000 lines)
            // hostname gives us container ID, use docker inspect to get the name
            std::string logs_cmd = fmt::format(
                "CONTAINER_NAME=$(docker inspect --format='{{{{.Name}}}}' $(hostname) 2>/dev/null | sed 's/^\\/\\/*//' || echo 'wolf'); "
                "docker logs --tail 1000 $CONTAINER_NAME > {}-logs.txt 2>&1 || "
                "echo 'Failed to capture logs' > {}-logs.txt",
                prefix, prefix);
            system(logs_cmd.c_str());

            logs::log(logs::info, "[WATCHDOG] Debug dumps written to: {}-*", prefix);
            _exit(0); // Exit child cleanly

          } catch (const std::exception& e) {
            logs::log(logs::error, "[WATCHDOG] Debug gathering failed: {}", e.what());
            _exit(1);
          }
        } else if (child_pid > 0) {
          // PARENT PROCESS: Wait for child (max 70s = 60s alarm + 10s grace)
          int status;
          pid_t result = waitpid(child_pid, &status, 0);

          if (result == -1) {
            logs::log(logs::error, "[WATCHDOG] waitpid failed: {}", strerror(errno));
          } else if (WIFEXITED(status)) {
            logs::log(logs::info, "[WATCHDOG] Debug gathering completed with exit code {}", WEXITSTATUS(status));
          } else if (WIFSIGNALED(status)) {
            logs::log(logs::warning, "[WATCHDOG] Debug gathering killed by signal {}", WTERMSIG(status));
          }

          // Exit main process for Docker restart
          // CRITICAL: Use _exit() not exit() - exit() runs destructors/atexit handlers
          // which may deadlock if threads are stuck holding locks
          // Don't fflush() - it can block if I/O subsystem is stuck
          logs::log(logs::fatal, "[WATCHDOG] Exiting for container restart");
          _exit(1);

        } else {
          logs::log(logs::error, "[WATCHDOG] fork() failed: {}", strerror(errno));
          _exit(1);
        }
      }
      // System healthy - no action needed
    }
    } catch (const std::exception &e) {
      logs::log(logs::error, "Watchdog thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "Watchdog thread unknown exception");
    }
  }).detach();
}

/**
 * @brief Periodic core dump - dumps core every hour for post-mortem debugging
 *
 * If Wolf deadlocks but doesn't hit critical threshold (e.g., 5/14 = 35% < 50%),
 * or if restarted before watchdog triggers, we lose all debugging data.
 * Hourly dumps ensure we always have recent state to analyze.
 *
 * Uses gcore which pauses process briefly (~3s) to get consistent snapshot.
 * May cause brief stream glitches during dump, but process keeps running.
 *
 * Keeps last N dumps + enforces size quota.
 * Core dumps are ~8GB each. Configure via env vars:
 *   WOLF_MAX_DUMPS (default: 6) - max number of dumps
 *   WOLF_MAX_DUMPS_GB (default: 20) - max total size in GB
 */
void start_periodic_dumps() {
  std::thread([]() {
    try {
      using namespace std::chrono;

    const hours DUMP_INTERVAL{1};  // Dump every hour

    // Configurable limits via env vars
    int max_dumps = 6;  // Default: 6 dumps
    if (const char* env = std::getenv("WOLF_MAX_DUMPS")) {
      max_dumps = std::max(1, std::atoi(env));
    }

    uint64_t max_size_bytes = 20ULL * 1024 * 1024 * 1024;  // Default: 20GB
    if (const char* env = std::getenv("WOLF_MAX_DUMPS_GB")) {
      max_size_bytes = std::max(uint64_t{1}, static_cast<uint64_t>(std::atoi(env))) * 1024 * 1024 * 1024;
    }

    logs::log(logs::info, "[PERIODIC_DUMP] Started (max {} dumps, {}GB quota)", max_dumps, max_size_bytes / (1024*1024*1024));

    // First dump after 5 minutes to verify gcore works (don't wait a full hour)
    const minutes INITIAL_DELAY{5};
    logs::log(logs::info, "[PERIODIC_DUMP] First dump in 5 minutes, then hourly");
    std::this_thread::sleep_for(INITIAL_DELAY);

    while (true) {

      logs::log(logs::info, "[PERIODIC_DUMP] Starting hourly core dump");

      // Fork child process for dump (timeout protection - if gcore hangs, child dies after 5min)
      pid_t child_pid = fork();

      if (child_pid == 0) {
        // CHILD PROCESS: Generate core dump with 5min timeout
        alarm(300);  // Kill child if gcore hangs

        try {
          auto now = system_clock::now();
          auto timestamp = duration_cast<seconds>(now.time_since_epoch()).count();
          std::string debug_dir = "/var/wolf-debug-dumps";
          std::string prefix = fmt::format("{}/hourly-{}", debug_dir, timestamp);

          std::filesystem::create_directories(debug_dir);

          // Generate core dump using gcore (dumps WITHOUT stopping process)
          std::string gcore_cmd = fmt::format("gcore -o {} {} 2>&1", prefix, getppid());
          logs::log(logs::info, "[PERIODIC_DUMP] Running: {}", gcore_cmd);
          int gcore_result = system(gcore_cmd.c_str());

          if (gcore_result == 0) {
            logs::log(logs::info, "[PERIODIC_DUMP] Core dump saved: {}.{}", prefix, getppid());

            // Rotate old hourly dumps - match ALL hourly-* files regardless of
            // PID suffix (old dumps from previous Wolf runs must be cleaned too)
            std::vector<std::filesystem::path> hourly_dumps;
            for (const auto& entry : std::filesystem::directory_iterator(debug_dir)) {
              std::string filename = entry.path().filename().string();
              // Match all hourly-* files (from any Wolf process)
              if (filename.starts_with("hourly-")) {
                hourly_dumps.push_back(entry.path());
              }
            }

            // Sort by timestamp (filename is hourly-{timestamp}.{pid})
            std::sort(hourly_dumps.begin(), hourly_dumps.end());

            // Remove oldest dumps if we have more than max_dumps
            while (hourly_dumps.size() > static_cast<size_t>(max_dumps)) {
              logs::log(logs::info, "[PERIODIC_DUMP] Count limit: removing {}", hourly_dumps[0].string());
              std::filesystem::remove(hourly_dumps[0]);
              hourly_dumps.erase(hourly_dumps.begin());
            }

            // Enforce size quota - delete oldest until under limit
            auto calc_total_size = [&]() {
              uint64_t total = 0;
              for (const auto& p : hourly_dumps) {
                try { total += std::filesystem::file_size(p); } catch (...) {}
              }
              return total;
            };

            while (!hourly_dumps.empty() && calc_total_size() > max_size_bytes) {
              logs::log(logs::info, "[PERIODIC_DUMP] Size quota: removing {}", hourly_dumps[0].string());
              std::filesystem::remove(hourly_dumps[0]);
              hourly_dumps.erase(hourly_dumps.begin());
            }
          } else {
            logs::log(logs::warning, "[PERIODIC_DUMP] gcore failed with code {}", gcore_result);
          }

          _exit(0);  // Exit child cleanly

        } catch (const std::exception& e) {
          logs::log(logs::error, "[PERIODIC_DUMP] Dump failed: {}", e.what());
          _exit(1);
        }
      } else if (child_pid > 0) {
        // PARENT PROCESS: Wait for child (max 310s = 5min alarm + 10s grace)
        int status;
        pid_t result = waitpid(child_pid, &status, 0);

        if (result == -1) {
          logs::log(logs::error, "[PERIODIC_DUMP] waitpid failed: {}", strerror(errno));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
          logs::log(logs::info, "[PERIODIC_DUMP] Hourly dump completed successfully");
        }
      } else {
        logs::log(logs::error, "[PERIODIC_DUMP] fork() failed: {}", strerror(errno));
      }

      // Sleep until next dump (1 hour)
      std::this_thread::sleep_for(DUMP_INTERVAL);
    }
    } catch (const std::exception &e) {
      logs::log(logs::error, "Periodic dump thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "Periodic dump thread unknown exception");
    }
  }).detach();
}

/**
 * @brief Monitors sessions for inactivity and stops orphaned sessions.
 *
 * This prevents memory leaks from orphaned sessions when clients disconnect abruptly
 * (browser crash, network failure, WebSocket disconnect without proper cleanup).
 *
 * Sessions that haven't received any ENET packets for SESSION_TIMEOUT seconds will be stopped,
 * which triggers cleanup of GStreamer pipelines, interpipesrc consumers, and session records.
 *
 * NOTE: We fire StopStreamEvent (not PauseStreamEvent) because:
 * - PauseStreamEvent only quits the pipeline, leaving the session in running_sessions
 * - StopStreamEvent removes the session from running_sessions, properly cleaning up resources
 * - Orphaned sessions (no client activity for 60s) should be fully stopped, not just paused
 *
 * @param app_state Box containing AppState with running_sessions and event_bus
 */
void start_session_timeout_monitor(immer::box<state::AppState> app_state) {
  std::thread([app_state]() {
    try {
      using namespace std::chrono;

      const seconds CHECK_INTERVAL{10};      // Check every 10 seconds
    const seconds SESSION_TIMEOUT{60};     // Sessions idle >60s are considered orphaned

    logs::log(logs::info, "[SESSION_TIMEOUT] Started session timeout monitor (timeout={}s, check_interval={}s)",
              SESSION_TIMEOUT.count(), CHECK_INTERVAL.count());

    while (true) {
      std::this_thread::sleep_for(CHECK_INTERVAL);

      auto now = steady_clock::now();
      auto sessions = app_state->running_sessions->load();

      for (const auto& session : *sessions) {
        if (!session.last_activity) {
          continue;  // Skip sessions without activity tracking (shouldn't happen)
        }

        auto last_activity = session.last_activity->load();
        auto idle_duration = duration_cast<seconds>(now - last_activity);

        if (idle_duration > SESSION_TIMEOUT) {
          logs::log(logs::warning,
                    "[SESSION_TIMEOUT] Session {} idle for {}s (>{}s), firing StopStreamEvent to clean up orphaned session",
                    session.session_id, idle_duration.count(), SESSION_TIMEOUT.count());

          // Fire StopStreamEvent to fully stop and remove the session
          // (PauseStreamEvent only pauses the pipeline but leaves session in running_sessions,
          //  causing orphaned interpipesrc consumers to accumulate)
          try {
            app_state->event_bus->fire_event(
                immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session.session_id}));
          } catch (const std::exception &e) {
            logs::log(logs::error, "[SESSION_TIMEOUT] Exception firing StopStreamEvent: {}", e.what());
          } catch (...) {
            logs::log(logs::error, "[SESSION_TIMEOUT] Unknown exception firing StopStreamEvent");
          }
        }
      }
    }
    } catch (const std::exception &e) {
      logs::log(logs::error, "Session timeout monitor thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "Session timeout monitor thread unknown exception");
    }
  }).detach();
}

/**
 * @brief here's where the magic starts
 */
void run() {
  streaming::init(); // Need to initialise gstreamer once
  control::init();   // Need to initialise enet once
  docker::init();    // Need to initialise libcurl once
  gst_video_context::init();

  auto runtime_dir = utils::get_env("XDG_RUNTIME_DIR", "/tmp/sockets");
  logs::log(logs::debug, "XDG_RUNTIME_DIR={}", runtime_dir);

  auto config_file = utils::get_env("WOLF_CFG_FILE", "config.toml");
  auto p_key_file = utils::get_env("WOLF_PRIVATE_KEY_FILE", "key.pem");
  auto p_cert_file = utils::get_env("WOLF_PRIVATE_CERT_FILE", "cert.pem");
  auto local_state = initialize(config_file, p_key_file, p_cert_file);

  // HTTP APIs
  auto http_thread = std::thread([local_state]() {
    try {
      wolf::monitoring::ScopedThreadMonitor thread_monitor("HTTP-Server");
      // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
      HttpServer server = HttpServer();
      HTTPServers::startServer(&server, local_state, state::get_port(state::HTTP_PORT));
    } catch (const std::exception &e) {
      logs::log(logs::error, "HTTP server thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "HTTP server thread unknown exception");
    }
  });

  // HTTPS APIs
  std::thread([local_state, p_key_file, p_cert_file]() {
    try {
      wolf::monitoring::ScopedThreadMonitor thread_monitor("HTTPS-Server");
      // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
      HttpsServer server = HttpsServer(p_cert_file, p_key_file);
      HTTPServers::startServer(&server, local_state, state::get_port(state::HTTPS_PORT));
    } catch (const std::exception &e) {
      logs::log(logs::error, "HTTPS server thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "HTTPS server thread unknown exception");
    }
  }).detach();

  // RTSP
  std::thread([sessions = local_state->running_sessions]() {
    try {
      wolf::monitoring::ScopedThreadMonitor thread_monitor("RTSP-Server");
      // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
      rtsp::run_server(state::get_port(state::RTSP_SETUP_PORT), sessions);
    } catch (const std::exception &e) {
      logs::log(logs::error, "RTSP server thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "RTSP server thread unknown exception");
    }
  }).detach();

  // Control
  std::thread([sessions = local_state->running_sessions, ev_bus = local_state->event_bus]() {
    try {
      wolf::monitoring::ScopedThreadMonitor thread_monitor("Control-Server");
      // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
      control::run_control(state::get_port(state::CONTROL_PORT), sessions, ev_bus);
    } catch (const std::exception &e) {
      logs::log(logs::error, "Control server thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "Control server thread unknown exception");
    }
  }).detach();

  // RTP
  rtp::start_rtp_ping(state::get_port(state::VIDEO_PING_PORT),
                      state::get_port(state::AUDIO_PING_PORT),
                      local_state->event_bus);
  // Wolf API server (Unix socket)
  std::thread([local_state, runtime_dir]() {
    try {
      wolf::monitoring::ScopedThreadMonitor thread_monitor("UnixSocket-API");
      // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
      wolf::api::start_server(runtime_dir, local_state);
    } catch (const std::exception &e) {
      logs::log(logs::error, "Unix socket API server thread exception: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "Unix socket API server thread unknown exception");
    }
  }).detach();

  // mDNS
  std::thread([hostname = local_state->config->hostname]() {
    logs::log(logs::info, "Starting mDNS service");
    try {
      mdns_cpp::Logger::setLoggerSink([](const std::string &msg) {
        // msg here will include a /n at the end, so we remove it
        logs::log(logs::trace, "mDNS: {}", msg.substr(0, msg.size() - 1));
      });
      mdns_cpp::mDNS mdns;
      mdns.setServiceName("_nvstream._tcp.local.");
      mdns.setServiceHostname(hostname);
      mdns.setServicePort(state::HTTP_PORT);
      mdns.startService(false);
    } catch (const std::exception &e) {
      logs::log(logs::error, "mDNS error: {}", e.what());
    } catch (...) {
      logs::log(logs::error, "mDNS unknown exception");
    }
  }).detach();

  auto audio_server = setup_audio_server(local_state->host->host_xdg_runtime_dir, runtime_dir);
  // Setup event handlers for Moonlight related events (Start/Stop stream, hotplug, etc)
  auto moonlight_sess_handlers = sessions::setup_moonlight_handlers(local_state, runtime_dir, audio_server);
  // Setup event handlers for player Lobbies
  auto lobbies_handlers = sessions::setup_lobbies_handlers(local_state, runtime_dir, audio_server);

  // Start watchdog thread for deadlock detection
  start_watchdog();

  // Start periodic core dumps (every hour, keeps last 3)
  start_periodic_dumps();

  // Start session timeout monitor (cleans up orphaned sessions after 60s of inactivity)
  start_session_timeout_monitor(local_state);

  // Monitor main thread - parked on http_thread.join()
  // Add simple heartbeat to prove main thread is alive
  wolf::monitoring::ScopedThreadMonitor main_thread_monitor("Main-Thread");
  pid_t main_tid = syscall(SYS_gettid);

  std::atomic<bool> stop_main_heartbeat{false};
  std::thread main_heartbeat_thread([&stop_main_heartbeat, main_tid]() {
    while (!stop_main_heartbeat) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      wolf::monitoring::ThreadMonitor::get().heartbeat_for_tid(main_tid);
    }
  });

  http_thread.join(); // Let's park the main thread over here

  stop_main_heartbeat = true;
  if (main_heartbeat_thread.joinable()) {
    main_heartbeat_thread.join();
  }

  // If we reach here, HTTP thread died unexpectedly
  logs::log(logs::fatal, "[MAIN] HTTP thread died - Wolf is shutting down");
  exit(1);
}

int main(int argc, char *argv[]) try {
  logs::init(logs::parse_level(utils::get_env("WOLF_LOG_LEVEL", "INFO")));
  // Exception and termination handling
  std::signal(SIGINT, shutdown_handler);
  std::signal(SIGTERM, shutdown_handler);
  std::signal(SIGQUIT, shutdown_handler);
  std::signal(SIGSEGV, shutdown_handler);
  std::signal(SIGABRT, shutdown_handler);
  std::set_terminate(on_terminate);
  check_exceptions();

  run(); // Main loop
} catch (...) {
  on_terminate();
}