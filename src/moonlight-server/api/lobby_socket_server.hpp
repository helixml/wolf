#pragma once

#include <boost/asio.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <memory>
#include <string>
#include <thread>

namespace wolf::api {

/**
 * Per-lobby Unix socket server for multi-tenant isolation.
 *
 * Each lobby gets its own socket that only exposes lobby-specific endpoints:
 * - POST /set-pipewire-node-id - Set PipeWire ScreenCast node ID
 * - GET /status - Get lobby status
 *
 * This prevents containers from interfering with other lobbies via the API.
 */
class LobbySocketServer {
public:
  LobbySocketServer(const std::string &socket_path,
                    const std::string &lobby_id,
                    std::shared_ptr<wolf::core::events::EventBusType> event_bus);

  ~LobbySocketServer();

  // Non-copyable
  LobbySocketServer(const LobbySocketServer &) = delete;
  LobbySocketServer &operator=(const LobbySocketServer &) = delete;

  // Movable
  LobbySocketServer(LobbySocketServer &&) = default;
  LobbySocketServer &operator=(LobbySocketServer &&) = default;

  void start();
  void stop();

  const std::string &socket_path() const { return socket_path_; }

private:
  void run_server();
  void handle_connection(std::shared_ptr<boost::asio::local::stream_protocol::socket> socket);
  void handle_request(const std::string &request, std::shared_ptr<boost::asio::local::stream_protocol::socket> socket);

  void send_response(std::shared_ptr<boost::asio::local::stream_protocol::socket> socket,
                     int status_code,
                     const std::string &body);

  std::string socket_path_;
  std::string lobby_id_;
  std::shared_ptr<wolf::core::events::EventBusType> event_bus_;

  std::unique_ptr<boost::asio::io_context> io_context_;
  std::unique_ptr<boost::asio::local::stream_protocol::acceptor> acceptor_;
  std::unique_ptr<std::thread> server_thread_;
  std::atomic<bool> running_{false};
};

/**
 * Start a lobby socket server in a background thread.
 * Returns the server instance for lifecycle management.
 */
std::shared_ptr<LobbySocketServer> start_lobby_socket_server(
    const std::string &socket_path,
    const std::string &lobby_id,
    std::shared_ptr<wolf::core::events::EventBusType> event_bus);

} // namespace wolf::api
