#pragma once

#include <api/http_server.hpp>
#include <events/events.hpp>
#include <events/reflectors.hpp>
#include <moonlight/control.hpp>
#include <state/data-structures.hpp>

namespace wolf::api {

using namespace wolf::core;

void start_server(std::string_view runtime_dir, immer::box<state::AppState> app_state);

struct PendingPairClient {
  std::string pair_secret;
  rfl::Description<"The IP of the remote Moonlight client", std::string> client_ip;
};

struct PairRequest {
  std::string pair_secret;
  rfl::Description<"The PIN created by the remote Moonlight client", std::string> pin;
};

struct UnpairClientRequest {
  rfl::Description<"The client ID to unpair", std::string> client_id;
};

struct GenericSuccessResponse {
  bool success = true;
};

struct GenericErrorResponse {
  bool success = false;
  std::string error;
};

struct PendingPairRequestsResponse {
  bool success = true;
  std::vector<PendingPairClient> requests;
};

struct PairedClient {
  std::string client_id;
  std::string app_state_folder;
  config::ClientSettings settings = {};
};

struct PairedClientsResponse {
  bool success = true;
  std::vector<PairedClient> clients;
};

struct PartialClientSettings {
  std::optional<uint> run_uid;
  std::optional<uint> run_gid;
  std::optional<std::vector<wolf::config::ControllerType>> controllers_override;
  std::optional<float> mouse_acceleration;
  std::optional<float> v_scroll_acceleration;
  std::optional<float> h_scroll_acceleration;
};

struct UpdateClientSettingsRequest {
  rfl::Description<"The client ID to identify the client (derived from certificate)", std::string> client_id;
  rfl::Description<"New app state folder path (optional)", std::optional<std::string>> app_state_folder;
  rfl::Description<"Client settings to update (only specified fields will be updated)",
                   std::optional<PartialClientSettings>>
      settings;
};

struct AppListResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::App>::ReflType> apps;
};

struct AppDeleteRequest {
  std::string id;
};

struct ProfileListResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::Profile>::ReflType> profiles;
};

struct ProfileRemoveRequest {
  std::string id;
};

struct StreamSessionCreated {
  bool success = true;
  std::string session_id;
};

struct StreamSessionListResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::StreamSession>::ReflType> sessions;
};

struct StreamSessionStartRequest {
  std::string session_id;

  events::VideoSession video_session;
  events::AudioSession audio_session;
};

struct StreamSessionPauseRequest {
  std::string session_id;
};

struct StreamSessionStopRequest {
  std::string session_id;
};

struct StreamSessionHandleInputRequest {
  std::string session_id;
  rfl::Description<"A HEX encoded Moonlight input packet, for the full format see: "
                   "games-on-whales.github.io/wolf/stable/protocols/input-data.html",
                   std::string>
      input_packet_hex;
};

/**
 * Request to set PipeWire ScreenCast node ID for a lobby.
 * Container calls this after creating a ScreenCast session.
 */
struct SetPipeWireNodeIdRequest {
  std::string lobby_id;
  unsigned int node_id;
};

struct CreateLobbyRequest {
  rfl::Description<"The profile that originally created the lobby", std::string> profile_id;
  std::string name;
  std::optional<std::string> icon_png_path;
  bool multi_user = true;
  rfl::Description<"If present, the pin that is required to join the lobby."
                   "If this is not set, then the lobby is open to everyone",
                   std::optional<std::vector<short>>>
      pin;
  bool stop_when_everyone_leaves = true;

  events::VideoSettings video_settings;
  events::AudioSettings audio_settings;

  rfl::Description<"Client settings to update (only specified fields will be updated)",
                   std::optional<PartialClientSettings>>
      client_settings;

  std::string runner_state_folder;
  events::RunnerTypes runner;
};

struct LobbiesResponse {
  bool success = true;
  std::vector<rfl::Reflector<events::Lobby>::ReflType> lobbies;
};

struct LobbyCreateResponse {
  bool success = true;
  std::string lobby_id;
};

struct RunnerStartRequest {
  bool stop_stream_when_over;
  events::RunnerTypes runner;
  std::string session_id;
};

struct GetIconRequest {
  std::string icon_png_path;
};

struct GetIconResponse {
  bool success = true;
  std::string icon_base64;
};

struct DockerPullImageRequest {
  std::string image_name;
};

struct DockerPullImageResponse {
  bool success = true;
};

struct AppMemoryUsage {
  std::string app_id;
  std::string app_name;
  std::string resolution;
  int64_t client_count;
  int64_t memory_bytes;
};

struct LobbyMemoryUsage {
  std::string lobby_id;
  std::string lobby_name;
  std::string resolution;
  int64_t client_count;
  int64_t memory_bytes;
};

struct ClientConnectionInfo {
  size_t session_id; // Keep as size_t - Moonlight requires string serialization for session IDs
  std::string client_ip;
  std::string resolution;
  std::optional<std::string> lobby_id;
  std::optional<std::string> app_id;
  int64_t memory_bytes;
};

struct GPUStats {
  bool available = false;
  std::string gpu_name;
  int encoder_session_count = 0;
  double encoder_average_fps = 0.0;
  int encoder_average_latency_us = 0;
  int encoder_utilization_percent = 0;
  int gpu_utilization_percent = 0;
  int memory_utilization_percent = 0;
  int memory_used_mb = 0;
  int memory_total_mb = 0;
  int temperature_celsius = 0;
  int query_duration_ms = 0; // Track how long nvidia-smi took
  std::string error;
};

struct GStreamerPipelineStats {
  int producer_pipelines = 0; // Video + audio producers (2 per lobby)
  int consumer_pipelines = 0; // Video + audio consumers (2 per session)
  int total_pipelines = 0;    // Sum of producers + consumers
};

struct ThreadHealthInfo {
  int32_t tid;
  std::string name;
  std::string details;  // Pipeline description or other info
  int64_t seconds_since_heartbeat;
  int64_t seconds_alive;
  int64_t heartbeat_count;  // Changed from uint64_t to avoid JSON string serialization
  bool is_stuck;  // >30s since heartbeat

  // HTTP request tracking (for HTTP/HTTPS server threads)
  std::string current_request_path;
  int64_t request_duration_seconds;
  bool has_active_request;

  // Kernel stack trace (where thread is blocked/executing)
  std::string stack_trace;
};

struct SystemHealthResponse {
  bool success = true;
  int64_t process_uptime_seconds;  // How long Wolf has been running
  std::vector<ThreadHealthInfo> threads;
  int32_t stuck_thread_count;
  int32_t total_thread_count;
  bool can_create_new_pipelines;  // Tests if GStreamer type lock is available (real deadlock check)
  std::string overall_status;  // "healthy", "degraded", "critical"
};

struct SystemMemoryResponse {
  bool success = true;
  int64_t process_rss_bytes;
  int64_t gstreamer_buffer_bytes;
  int64_t total_memory_bytes;
  std::vector<AppMemoryUsage> apps;
  std::vector<LobbyMemoryUsage> lobbies;
  std::vector<ClientConnectionInfo> clients;
  std::optional<GPUStats> gpu_stats;                          // GPU encoder metrics via nvidia-smi
  std::optional<GStreamerPipelineStats> gstreamer_pipelines; // Actual pipeline count from state
};

// Keyboard state observability for debugging stuck modifier keys
struct KeyboardModifierState {
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool meta = false;
};

// One layer of keyboard state (Wolf's view, inputtino's view, or evdev/kernel view)
struct KeyboardLayerState {
  std::vector<int32_t> pressed_keys;          // Key codes (Moonlight VK codes for wolf/inputtino, Linux KEY_* for evdev)
  std::vector<std::string> pressed_key_names; // Human-readable names
  KeyboardModifierState modifier_state;
};

struct SessionKeyboardState {
  std::string session_id;
  int64_t timestamp_ms;
  std::string device_name;
  std::string device_node;  // e.g., /dev/input/event15

  // Three layers of keyboard state for debugging:
  // 1. Wolf's view - what Moonlight events Wolf has received and tracked
  KeyboardLayerState wolf_state;
  // 2. Inputtino's view - what inputtino's internal cur_press_keys vector contains
  KeyboardLayerState inputtino_state;
  // 3. Evdev/kernel view - what the kernel thinks is pressed on the virtual device
  KeyboardLayerState evdev_state;

  // Mismatch detection - true if any layer disagrees (indicates a bug)
  bool has_mismatch = false;
  std::string mismatch_description;

  // Legacy fields for backwards compatibility
  std::vector<int32_t> pressed_keys;          // Same as wolf_state.pressed_keys
  std::vector<std::string> pressed_key_names; // Same as wolf_state.pressed_key_names
  KeyboardModifierState modifier_state;       // Same as wolf_state.modifier_state
};

struct KeyboardStateRequest {
  std::optional<std::string> session_id;  // Optional: filter by session
};

struct KeyboardStateResponse {
  bool success = true;
  std::vector<SessionKeyboardState> sessions;
};

struct KeyboardResetRequest {
  std::string session_id;  // Required: which session to reset
};

struct KeyboardResetResponse {
  bool success = true;
  std::vector<std::string> released_keys;
  std::string message;
};

/**
 * Request to pre-configure a pending session for immediate lobby attachment.
 * This allows Helix to set up lobby attachment before the Moonlight client connects.
 * Security: Only accessible via Unix socket (Helix API container).
 */
struct ConfigurePendingSessionRequest {
  rfl::Description<"Moonlight client unique ID for matching (e.g., helix-agent-{sessionId})",
                   std::string> client_unique_id;
  rfl::Description<"Lobby ID to attach to immediately when session starts",
                   std::string> immediate_lobby_id;
};

struct ConfigurePendingSessionResponse {
  bool success = true;
  std::string message;
};

struct UnixSocket {
  boost::asio::local::stream_protocol::socket socket;
  bool is_alive = true;
};

class UnixSocketServer {
public:
  UnixSocketServer(boost::asio::io_context &io_context,
                   const std::string &socket_path,
                   immer::box<state::AppState> app_state);

  UnixSocketServer(const UnixSocketServer &) = default;

  void broadcast_event(const std::string &event_type, const std::string &event_json);

private:
  void endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_Lobbies(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyCreate(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyJoin(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyLeave(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbyStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_LobbySetPipeWireNodeId(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_RunnerStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_SystemMemory(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_SystemHealth(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_KeyboardState(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_KeyboardReset(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void endpoint_StreamSessionConfigure(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);

  void sse_broadcast(const std::string &payload);
  void sse_keepalive(const boost::system::error_code &e);

  void send_http(std::shared_ptr<UnixSocket> socket, int status_code, std::string_view body);
  void send_http(std::shared_ptr<UnixSocket> socket,
                 int status_code,
                 const std::vector<std::string_view> &http_headers,
                 std::string_view body);
  void send_data(std::shared_ptr<UnixSocket> socket, std::string_view data);

  void handle_request(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket);
  void start_connection(std::shared_ptr<UnixSocket> socket);
  void start_accept();

  void cleanup_sockets();
  void close(UnixSocket &socket);

  struct UnixSocketState {
    UnixSocketState(boost::asio::io_context &io_context, immer::box<state::AppState> app_state, std::string socket_path)
        : io_context(io_context), app_state(app_state),
          acceptor(io_context, boost::asio::local::stream_protocol::endpoint(socket_path)),
          http(HTTPServer<std::shared_ptr<UnixSocket>>{}), sse_keepalive_timer(boost::asio::steady_timer{io_context}) {}

    boost::asio::io_context &io_context;
    immer::box<state::AppState> app_state;
    boost::asio::local::stream_protocol::acceptor acceptor;
    std::vector<std::shared_ptr<UnixSocket>> sockets;
    HTTPServer<std::shared_ptr<UnixSocket>> http;
    boost::asio::steady_timer sse_keepalive_timer;
  };

  std::shared_ptr<UnixSocketState> state_;
};

} // namespace wolf::api