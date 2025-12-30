#include <api/lobby_socket_server.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <filesystem>
#include <rfl/json.hpp>
#include <sstream>

namespace wolf::api {

using namespace wolf::core;
using boost::asio::local::stream_protocol;

// Simple request/response structures for the lobby API
// NOTE: Unique name to avoid ODR conflicts with api.hpp's SetPipeWireNodeIdRequest
// which requires lobby_id. This local struct only needs node_id since the lobby
// context is implicit (per-lobby socket).
struct LobbySetNodeIdRequest {
  unsigned int node_id;
};

struct SetInputSocketRequest {
  std::string input_socket;
};

struct LobbyStatusResponse {
  std::string lobby_id;
  std::string status = "running";
};

struct ErrorResponse {
  std::string error;
};

struct SuccessResponse {
  bool success = true;
};

LobbySocketServer::LobbySocketServer(const std::string &socket_path,
                                     const std::string &lobby_id,
                                     std::shared_ptr<events::EventBusType> event_bus)
    : socket_path_(socket_path), lobby_id_(lobby_id), event_bus_(event_bus) {}

LobbySocketServer::~LobbySocketServer() {
  stop();
}

void LobbySocketServer::start() {
  if (running_) {
    return;
  }

  // Remove existing socket file
  std::filesystem::remove(socket_path_);

  io_context_ = std::make_unique<boost::asio::io_context>();
  acceptor_ = std::make_unique<stream_protocol::acceptor>(
      *io_context_, stream_protocol::endpoint(socket_path_));

  // Make socket world-accessible so container can connect
  std::filesystem::permissions(socket_path_,
                               std::filesystem::perms::owner_all |
                               std::filesystem::perms::group_all |
                               std::filesystem::perms::others_all);

  running_ = true;
  server_thread_ = std::make_unique<std::thread>([this]() { run_server(); });

  logs::log(logs::info, "[LOBBY_SOCKET] Started per-lobby socket for {} at {}",
            lobby_id_, socket_path_);
}

void LobbySocketServer::stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  if (io_context_) {
    io_context_->stop();
  }

  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }

  // Clean up socket file
  std::filesystem::remove(socket_path_);

  logs::log(logs::info, "[LOBBY_SOCKET] Stopped per-lobby socket for {} at {}",
            lobby_id_, socket_path_);
}

void LobbySocketServer::run_server() {
  while (running_) {
    try {
      auto socket = std::make_shared<stream_protocol::socket>(*io_context_);
      acceptor_->accept(*socket);

      if (running_) {
        handle_connection(socket);
      }
    } catch (const boost::system::system_error &e) {
      if (running_) {
        logs::log(logs::warning, "[LOBBY_SOCKET] Accept error: {}", e.what());
      }
    }
  }
}

void LobbySocketServer::handle_connection(std::shared_ptr<stream_protocol::socket> socket) {
  try {
    boost::asio::streambuf buffer;
    boost::asio::read_until(*socket, buffer, "\r\n\r\n");

    std::istream stream(&buffer);
    std::string request;
    std::getline(stream, request);

    // Read remaining headers
    std::string header;
    size_t content_length = 0;
    while (std::getline(stream, header) && header != "\r") {
      if (header.find("Content-Length:") == 0) {
        content_length = std::stoul(header.substr(16));
      }
    }

    // Read body if present
    std::string body;
    if (content_length > 0) {
      // Read any remaining buffered data
      if (buffer.size() > 0) {
        std::istreambuf_iterator<char> eos;
        body = std::string(std::istreambuf_iterator<char>(stream), eos);
      }

      // Read more if needed
      while (body.size() < content_length) {
        char buf[1024];
        size_t to_read = std::min(sizeof(buf), content_length - body.size());
        size_t n = socket->read_some(boost::asio::buffer(buf, to_read));
        body.append(buf, n);
      }
    }

    handle_request(request + "\n" + body, socket);

  } catch (const std::exception &e) {
    logs::log(logs::warning, "[LOBBY_SOCKET] Connection error: {}", e.what());
  }
}

void LobbySocketServer::handle_request(const std::string &request,
                                       std::shared_ptr<stream_protocol::socket> socket) {
  // Parse HTTP request line
  std::istringstream iss(request);
  std::string method, path, version;
  iss >> method >> path >> version;

  // Extract body (everything after first newline)
  std::string body;
  auto newline_pos = request.find('\n');
  if (newline_pos != std::string::npos) {
    body = request.substr(newline_pos + 1);
  }

  logs::log(logs::debug, "[LOBBY_SOCKET] {} {} for lobby {}", method, path, lobby_id_);

  // Route requests
  if (method == "GET" && path == "/status") {
    // Return lobby status
    LobbyStatusResponse resp{.lobby_id = lobby_id_, .status = "running"};
    send_response(socket, 200, rfl::json::write(resp));

  } else if (method == "POST" && path == "/set-pipewire-node-id") {
    // Set PipeWire node ID
    auto parsed = rfl::json::read<LobbySetNodeIdRequest>(body);
    if (!parsed) {
      send_response(socket, 400, rfl::json::write(ErrorResponse{.error = "Invalid JSON: " + parsed.error().what()}));
      return;
    }

    logs::log(logs::info, "[LOBBY_SOCKET] Setting PipeWire node ID {} for lobby {}",
              parsed->node_id, lobby_id_);

    // Fire event to trigger pipewiresrc video producer startup
    event_bus_->fire_event(immer::box<events::SetPipeWireNodeIdEvent>(
        events::SetPipeWireNodeIdEvent{.lobby_id = lobby_id_, .node_id = parsed->node_id}));

    send_response(socket, 200, rfl::json::write(SuccessResponse{}));

  } else if (method == "POST" && path == "/set-input-socket") {
    // Set input socket path for RemoteDesktop input forwarding
    auto parsed = rfl::json::read<SetInputSocketRequest>(body);
    if (!parsed) {
      send_response(socket, 400, rfl::json::write(ErrorResponse{.error = "Invalid JSON: " + parsed.error().what()}));
      return;
    }

    logs::log(logs::info, "[LOBBY_SOCKET] Setting input socket {} for lobby {}",
              parsed->input_socket, lobby_id_);

    // Fire event to connect to the input socket
    event_bus_->fire_event(immer::box<events::SetInputSocketEvent>(
        events::SetInputSocketEvent{.lobby_id = lobby_id_, .input_socket_path = parsed->input_socket}));

    send_response(socket, 200, rfl::json::write(SuccessResponse{}));

  } else {
    send_response(socket, 404, rfl::json::write(ErrorResponse{.error = "Not found"}));
  }
}

void LobbySocketServer::send_response(std::shared_ptr<stream_protocol::socket> socket,
                                      int status_code,
                                      const std::string &body) {
  std::string status_text;
  switch (status_code) {
    case 200: status_text = "OK"; break;
    case 400: status_text = "Bad Request"; break;
    case 404: status_text = "Not Found"; break;
    case 500: status_text = "Internal Server Error"; break;
    default: status_text = "Unknown"; break;
  }

  std::ostringstream response;
  response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
  response << "Content-Type: application/json\r\n";
  response << "Content-Length: " << body.size() << "\r\n";
  response << "Connection: close\r\n";
  response << "\r\n";
  response << body;

  try {
    boost::asio::write(*socket, boost::asio::buffer(response.str()));
  } catch (const std::exception &e) {
    logs::log(logs::warning, "[LOBBY_SOCKET] Failed to send response: {}", e.what());
  }
}

std::shared_ptr<LobbySocketServer> start_lobby_socket_server(
    const std::string &socket_path,
    const std::string &lobby_id,
    std::shared_ptr<events::EventBusType> event_bus) {

  auto server = std::make_shared<LobbySocketServer>(socket_path, lobby_id, event_bus);
  server->start();
  return server;
}

} // namespace wolf::api
