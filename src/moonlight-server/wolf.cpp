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

    auto container = docker_api.create(
        docker::Container{
            .id = "",
            .name = "WolfPulseAudio",
            .image = utils::get_env("WOLF_PULSE_IMAGE", "ghcr.io/games-on-whales/pulseaudio:master"),
            .status = docker::CREATED,
            .ports = {},
            .mounts = {docker::MountPoint{.source = host_runtime_dir, .destination = "/tmp/pulse/", .mode = "rw"}},
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
 * @brief Deadlock watchdog - monitors system health and dumps debug info on critical status
 *
 * Runs in separate thread, polls ThreadMonitor every 30s.
 * If system is critical (stuck threads) for >60s:
 * 1. Fork child process (timeout protection - if debug gathering deadlocks, child dies after 60s)
 * 2. Write thread dump, generate core dump (timestamped files in /var/wolf-debug-dumps/)
 * 3. Exit main process for Docker restart
 */
/**
 * @brief Test if new GStreamer pipelines can be created
 *
 * The real failure mode for production deadlock is: global GLib type lock held
 * → gst_element_factory_make() blocks → can't create new sessions
 *
 * This test detects the ACTUAL problem (new sessions won't work) instead of
 * arbitrary thread percentage. Production deadlocked with only 5/14 threads stuck
 * (35% < 50% threshold), but new sessions couldn't start.
 *
 * @return true if pipeline creation works, false if deadlocked
 */
bool can_create_pipelines() {
  // Fork child to test creation (timeout protection - if type lock held, child hangs)
  pid_t child = fork();

  if (child == 0) {
    // CHILD PROCESS: Try to create simple element (requires global type lock)
    alarm(5);  // Kill child if it hangs >5s

    // gst_element_factory_make acquires global GLib type lock
    // If lock is held by crashed thread, this will block forever
    GstElement* test = gst_element_factory_make("fakesrc", nullptr);
    if (test) {
      gst_object_unref(test);
      _exit(0);  // Success - type lock available
    }
    _exit(1);  // Failed to create (shouldn't happen for fakesrc)
  }

  // PARENT PROCESS: Wait for child with timeout
  int status;
  struct timespec timeout = {.tv_sec = 6, .tv_nsec = 0};  // 6s max (5s alarm + 1s grace)
  siginfo_t info;

  int result = waitid(P_PID, child, &info, WEXITED | WNOHANG);
  if (result == 0 && info.si_pid == 0) {
    // Child still running after initial check - wait with timeout
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(6)) {
      result = waitid(P_PID, child, &info, WEXITED | WNOHANG);
      if (result == 0 && info.si_pid != 0) {
        // Child exited
        if (info.si_code == CLD_EXITED && info.si_status == 0) {
          return true;  // Pipeline creation works!
        }
        return false;  // Child crashed or returned error
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Child timed out - kill it
    kill(child, SIGKILL);
    waitpid(child, &status, 0);  // Clean up zombie
    return false;  // Type lock is held - new sessions WON'T work
  }

  // Child exited immediately
  waitpid(child, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void start_watchdog() {
  std::thread([]() {
    using namespace std::chrono;

    const seconds CHECK_INTERVAL{30};
    const seconds CRITICAL_THRESHOLD{60};
    std::optional<steady_clock::time_point> critical_since;

    logs::log(logs::info, "[WATCHDOG] Started monitoring system health");

    while (true) {
      std::this_thread::sleep_for(CHECK_INTERVAL);

      // Test if new pipelines can be created (real failure condition)
      // Production deadlocked with only 35% threads stuck, but new sessions couldn't start
      bool pipelines_work = can_create_pipelines();

      // Also check stuck thread count for context
      auto thread_statuses = wolf::monitoring::ThreadMonitor::get().get_all_threads();
      int stuck_count = 0;
      for (const auto& status : thread_statuses) {
        if (status.is_stuck) {
          stuck_count++;
        }
      }

      // CRITICAL if: pipeline creation fails (type lock held)
      // Thread count is just for logging context
      bool is_critical = !pipelines_work;

      if (is_critical) {
        if (!critical_since) {
          critical_since = steady_clock::now();
          logs::log(logs::error,
                    "[WATCHDOG] System entered CRITICAL state: pipeline creation FAILED (type lock held) - {}/{} threads stuck",
                    stuck_count, thread_statuses.size());
        }

        auto critical_duration = steady_clock::now() - *critical_since;
        if (critical_duration >= CRITICAL_THRESHOLD) {
          logs::log(logs::fatal,
                    "[WATCHDOG] System CRITICAL for {}s - gathering debug info and exiting",
                    duration_cast<seconds>(critical_duration).count());

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
              thread_dump << fmt::format("Wolf Deadlock Debug Dump - {}\n", timestamp);
              thread_dump << fmt::format("Critical for: {}s\n", duration_cast<seconds>(critical_duration).count());
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
            logs::log(logs::fatal, "[WATCHDOG] Exiting for container restart");
            exit(1);

          } else {
            logs::log(logs::error, "[WATCHDOG] fork() failed: {}", strerror(errno));
            exit(1);
          }
        }
      } else {
        // System healthy - pipeline creation works
        if (critical_since) {
          logs::log(logs::info, "[WATCHDOG] System recovered - pipeline creation works again");
          critical_since = std::nullopt;
        }

        if (stuck_count > 0) {
          logs::log(logs::warning,
                    "[WATCHDOG] System degraded: {}/{} threads stuck, BUT pipeline creation works (new sessions OK)",
                    stuck_count, thread_statuses.size());
        }
      }
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
 * Keeps last 48 hours of dumps + rotates old ones.
 * Core dumps are ~8GB each: 48 × 8GB = ~400GB disk space required.
 */
void start_periodic_dumps() {
  std::thread([]() {
    using namespace std::chrono;

    const hours DUMP_INTERVAL{1};  // Dump every hour
    const int MAX_HOURLY_DUMPS = 48;  // Keep 48 hours of dumps

    logs::log(logs::info, "[PERIODIC_DUMP] Started hourly core dump thread");

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

            // Rotate old hourly dumps (keep last MAX_HOURLY_DUMPS)
            std::vector<std::filesystem::path> hourly_dumps;
            for (const auto& entry : std::filesystem::directory_iterator(debug_dir)) {
              std::string filename = entry.path().filename().string();
              // Match hourly-* core dumps (not critical dumps)
              if (filename.starts_with("hourly-") && (filename.find(".core.") != std::string::npos || filename.ends_with(fmt::format(".{}", getppid())))) {
                hourly_dumps.push_back(entry.path());
              }
            }

            // Sort by timestamp (filename is hourly-{timestamp}.{pid})
            std::sort(hourly_dumps.begin(), hourly_dumps.end());

            // Remove oldest dumps if we have more than MAX_HOURLY_DUMPS
            if (hourly_dumps.size() > MAX_HOURLY_DUMPS) {
              int to_remove = hourly_dumps.size() - MAX_HOURLY_DUMPS;
              for (int i = 0; i < to_remove; i++) {
                logs::log(logs::info, "[PERIODIC_DUMP] Rotating out old dump: {}", hourly_dumps[i].string());
                std::filesystem::remove(hourly_dumps[i]);
              }
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
    wolf::monitoring::ScopedThreadMonitor thread_monitor("HTTP-Server");
    // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
    HttpServer server = HttpServer();
    HTTPServers::startServer(&server, local_state, state::get_port(state::HTTP_PORT));
  });

  // HTTPS APIs
  std::thread([local_state, p_key_file, p_cert_file]() {
    wolf::monitoring::ScopedThreadMonitor thread_monitor("HTTPS-Server");
    // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
    HttpsServer server = HttpsServer(p_cert_file, p_key_file);
    HTTPServers::startServer(&server, local_state, state::get_port(state::HTTPS_PORT));
  }).detach();

  // RTSP
  std::thread([sessions = local_state->running_sessions]() {
    wolf::monitoring::ScopedThreadMonitor thread_monitor("RTSP-Server");
    // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
    rtsp::run_server(state::get_port(state::RTSP_SETUP_PORT), sessions);
  }).detach();

  // Control
  std::thread([sessions = local_state->running_sessions, ev_bus = local_state->event_bus]() {
    wolf::monitoring::ScopedThreadMonitor thread_monitor("Control-Server");
    // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
    control::run_control(state::get_port(state::CONTROL_PORT), sessions, ev_bus);
  }).detach();

  // RTP
  rtp::start_rtp_ping(state::get_port(state::VIDEO_PING_PORT),
                      state::get_port(state::AUDIO_PING_PORT),
                      local_state->event_bus);
  // Wolf API server (Unix socket)
  std::thread([local_state, runtime_dir]() {
    wolf::monitoring::ScopedThreadMonitor thread_monitor("UnixSocket-API");
    // TODO: Add Boost ASIO steady_timer for heartbeat in io_context event loop
    wolf::api::start_server(runtime_dir, local_state);
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