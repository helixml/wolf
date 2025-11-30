#include <api/api.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <events/reflectors.hpp>
#include <filesystem>
#include <helpers/utils.hpp>
#include <memory>
#include <monitoring/thread-monitor.hpp>
#include <rfl/json.hpp>
#include <thread>

namespace wolf::api {

using namespace wolf::core;

void start_server(std::string_view runtime_dir, immer::box<state::AppState> app_state) {
  auto default_socket_path = std::filesystem::path(runtime_dir) / "wolf.sock";
  auto socket_path = utils::get_env("WOLF_SOCKET_PATH", default_socket_path.c_str());
  logs::log(logs::info, "Starting Wolf API server on {}", socket_path);

  ::unlink(socket_path);
  boost::asio::io_context io_context;
  UnixSocketServer server(io_context, socket_path, app_state);
  auto server_ptr = std::make_shared<UnixSocketServer>(server);

  auto global_ev_handler = app_state->event_bus->register_global_handler([server_ptr](events::EventsVariant ev) {
    std::visit(
        [server_ptr](auto &&arg) {
          const auto event_type = rfl::type_name_t<std::decay_t<decltype(*arg)>>().str();
          server_ptr->broadcast_event(event_type, rfl::json::write(*arg));
        },
        ev);
  });

  // Add heartbeat timer
  auto heartbeat_timer = std::make_shared<boost::asio::steady_timer>(io_context);
  auto heartbeat_callback = std::make_shared<std::function<void(const boost::system::error_code&)>>();
  *heartbeat_callback = [heartbeat_timer, heartbeat_callback](const boost::system::error_code& ec) {
    if (!ec) {
      wolf::monitoring::ThreadMonitor::get().heartbeat();
      heartbeat_timer->expires_after(std::chrono::seconds(1));
      heartbeat_timer->async_wait(*heartbeat_callback);
    }
  };
  heartbeat_timer->expires_after(std::chrono::seconds(1));
  heartbeat_timer->async_wait(*heartbeat_callback);

  // Run the io_context in a loop, restarting on exceptions
  // This ensures the socket stays alive even if handlers throw exceptions
  // (e.g., PulseAudio callback throwing "promise already satisfied")
  int consecutive_errors = 0;
  const int MAX_CONSECUTIVE_ERRORS = 10;

  while (true) {
    try {
      // Reset io_context if it was stopped by an exception
      if (io_context.stopped()) {
        io_context.restart();
      }

      io_context.run();

      // If run() returns normally (no work left), we're done
      logs::log(logs::info, "[API] io_context.run() completed normally");
      break;

    } catch (const std::exception& e) {
      consecutive_errors++;
      logs::log(logs::error, "[API] Exception in io_context: {} (error {}/{})",
                e.what(), consecutive_errors, MAX_CONSECUTIVE_ERRORS);

      if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        logs::log(logs::fatal, "[API] Too many consecutive errors, socket server stopping");
        throw;  // Re-throw to let process crash and restart
      }

      // Brief pause before restarting
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    } catch (...) {
      consecutive_errors++;
      logs::log(logs::error, "[API] Unknown exception in io_context (error {}/{})",
                consecutive_errors, MAX_CONSECUTIVE_ERRORS);

      if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        logs::log(logs::fatal, "[API] Too many consecutive errors, socket server stopping");
        throw;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

} // namespace wolf::api