#include <api/api.hpp>
#include <control/input_handler.hpp>
#include <control/keyboard_state.hpp>
#include <core/docker.hpp>
#include <monitoring/thread-monitor.hpp>
#include <rtp/udp-ping.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>
#include <chrono>
#include <fstream>
#include <sstream>

namespace wolf::api {

void UnixSocketServer::endpoint_Events(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  // curl -N --unix-socket /tmp/wolf.sock http://localhost/api/v1/events
  state_->sockets.push_back(socket);
  send_http(socket,
            200,
            {{"Content-Type: text/event-stream"}, {"Connection: keep-alive"}, {"Cache-Control: no-cache"}},
            ""); // Inform clients this is going to be SS
}

void UnixSocketServer::endpoint_PendingPairRequest(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto requests = std::vector<PendingPairClient>();
  for (auto [secret, pair_request] : *(state_->app_state)->pairing_atom->load()) {
    requests.push_back({.pair_secret = secret, .client_ip = pair_request->client_ip});
  }
  send_http(socket, 200, rfl::json::write(PendingPairRequestsResponse{.requests = requests}));
}

void UnixSocketServer::endpoint_Pair(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<PairRequest>(req.body);
  if (event) {
    if (auto pair_request = state_->app_state->pairing_atom->load()->find(event.value().pair_secret)) {
      pair_request->get().user_pin->set_value(event.value().pin.value()); // Resolve the promise
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid pair secret: {}", event.value().pair_secret);
      auto res = GenericErrorResponse{.error = "Invalid pair secret"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_PairedClients(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = PairedClientsResponse{.success = true};
  auto clients = state_->app_state->config->paired_clients->load();
  for (const config::PairedClient &client : clients.get()) {
    res.clients.push_back(PairedClient{.client_id = std::to_string(state::get_client_id(client)),
                                       .app_state_folder = client.app_state_folder,
                                       .settings = client.settings});
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_UnpairClient(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  try {
    auto payload_result = rfl::json::read<UnpairClientRequest>(req.body);
    if (!payload_result) {
      auto res = GenericErrorResponse{.error = "Invalid request format"};
      send_http(socket, 400, rfl::json::write(res));
      return;
    }

    const auto &payload = payload_result.value(); // Unwrap the Result
    auto client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
    if (!client) {
      auto res = GenericErrorResponse{.error = "Client not found"};
      send_http(socket, 404, rfl::json::write(res));
      return;
    }

    state::unpair(this->state_->app_state->config, *client);

    auto res = GenericSuccessResponse{.success = true};
    send_http(socket, 200, rfl::json::write(res));
  } catch (const std::exception &e) {
    auto res = GenericErrorResponse{.error = e.what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Apps(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = AppListResponse{.success = true};
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (!moonlight_profile) {
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Moonlight profile not found"}));
    return;
  }
  immer::vector<immer::box<events::App>> app_list = moonlight_profile.value()->apps->load();
  for (const immer::box<events::App> &app : app_list) {
    res.apps.push_back(rfl::Reflector<events::App>::from(app));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<rfl::Reflector<events::App>::ReflType>(req.body);
  if (app) {
    // Compute pipeline defaults if not provided in request
    // This ensures API apps get correct pipelines based on GPU vendor and WOLF_USE_ZERO_COPY
    auto defaults = state::compute_pipeline_defaults(this->state_->app_state->config->config_source);

    // Apply defaults to empty pipeline fields
    auto app_with_defaults = app.value();
    if (!app_with_defaults.video_producer_buffer_caps || app_with_defaults.video_producer_buffer_caps->empty()) {
      app_with_defaults.video_producer_buffer_caps = defaults.video_producer_buffer_caps;
    }
    if (!app_with_defaults.h264_gst_pipeline || app_with_defaults.h264_gst_pipeline->empty()) {
      app_with_defaults.h264_gst_pipeline = defaults.h264_gst_pipeline;
    }
    if (!app_with_defaults.hevc_gst_pipeline || app_with_defaults.hevc_gst_pipeline->empty()) {
      app_with_defaults.hevc_gst_pipeline = defaults.hevc_gst_pipeline;
    }
    if (!app_with_defaults.av1_gst_pipeline || app_with_defaults.av1_gst_pipeline->empty()) {
      app_with_defaults.av1_gst_pipeline = defaults.av1_gst_pipeline;
    }
    if (!app_with_defaults.opus_gst_pipeline || app_with_defaults.opus_gst_pipeline->empty()) {
      app_with_defaults.opus_gst_pipeline = defaults.opus_gst_pipeline;
    }

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app_with_defaults, this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app_with_defaults, this](auto &apps) {
                  return apps.push_back(rfl::Reflector<events::App>::to(app_with_defaults, this->state_->app_state->event_bus));
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveApp(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto app = rfl::json::read<AppDeleteRequest>(req.body);
  if (app) {
    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles | //
            ranges::views::transform([app = app.value(), this](const immer::box<events::Profile> &profile) {
              if (profile->id == events::MOONLIGHT_PROFILE_ID) {
                profile->apps->update([app, this](auto &apps) {
                  return apps | //
                         ranges::views::filter(
                             [&app](const immer::box<events::App> &a) { return a->base.id != app.id; }) | //
                         ranges::to<immer::vector<immer::box<events::App>>>();
                });
              }
              return profile;
            }) |
            ranges::to<state::ProfilesList>());

    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, app.error().what());
    auto res = GenericErrorResponse{.error = app.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_Profiles(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profiles = state_->app_state->config->profiles->load().get();
  auto res = ProfileListResponse{.success = true,
                                 .profiles = profiles | //
                                             ranges::views::filter([](const immer::box<events::Profile> &p) {
                                               return p->id != events::MOONLIGHT_PROFILE_ID;
                                             }) |                                                              //
                                             ranges::views::transform(rfl::Reflector<events::Profile>::from) | //
                                             ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_AddProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<rfl::Reflector<events::Profile>::ReflType>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(
        state_->app_state->config,
        profiles.push_back(rfl::Reflector<events::Profile>::to(p, this->state_->app_state->event_bus)));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_RemoveProfile(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto profile_req = rfl::json::read<ProfileRemoveRequest>(req.body);
  if (profile_req) {
    auto p = profile_req.value();

    auto profiles = state_->app_state->config->profiles->load().get();
    state::update_profiles(state_->app_state->config,
                           profiles | //
                               ranges::views::remove_if([&p](const immer::box<events::Profile> &profile) {
                                 return profile.get().id == p.id;
                               }) | //
                               ranges::to<state::ProfilesList>());
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, profile_req.error().what());
    auto res = GenericErrorResponse{.error = profile_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessions(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = StreamSessionListResponse{.success = true};
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto &session : sessions.get()) {
    res.sessions.push_back(rfl::Reflector<events::StreamSession>::from(session));
  }
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_StreamSessionAdd(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<rfl::Reflector<events::StreamSession>::ReflType>(req.body);
  if (session) {
    auto ss = session.value();
    auto app = state::get_moonlight_app_by_id(this->state_->app_state->config, ss.app_id);
    if (!app) {
      logs::log(logs::warning, "[API] Invalid app_id: {}", ss.app_id);
      auto res = GenericErrorResponse{.error = "Invalid app_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto client = state::get_client_by_id(this->state_->app_state->config, ss.client_id);
    if (!client) {
      logs::log(logs::warning, "[API] Invalid client_id: {}", ss.client_id);
      auto res = GenericErrorResponse{.error = "Invalid client_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto new_session = state::create_stream_session( //
        state_->app_state,
        app.value(),
        client.value(),
        moonlight::DisplayMode{.width = ss.video_width,
                               .height = ss.video_height,
                               .refreshRate = ss.video_refresh_rate,
                               .hevc_supported = state_->app_state->config->support_hevc,
                               .av1_supported = state_->app_state->config->support_av1},
        ss.audio_channel_count,
        ss.aes_key,
        ss.aes_iv,
        "");  // client_unique_id defaults to empty for Unix socket API (only HTTPS endpoints populate this)
    new_session->ip = ss.client_ip;
    new_session->rtsp_fake_ip = ss.rtsp_fake_ip;

    state_->app_state->running_sessions->update(
        [new_session](const immer::vector<events::StreamSession> &ses_v) { return ses_v.push_back(*new_session); });
    state_->app_state->event_bus->fire_event(immer::box<events::StreamSession>(*new_session));

    auto res = StreamSessionCreated{.success = true, .session_id = std::to_string(new_session->session_id)};
    send_http(socket, 200, rfl::json::write(res));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStart(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto start_req = rfl::json::read<StreamSessionStartRequest>(req.body);
  if (start_req) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(start_req.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto video_session = start_req.value().video_session;
      video_session.session_id = session_id; // Can't be JSON encoded
      if (video_session.render_node.empty()) {
        video_session.render_node = session->app->render_node;
      }
      state_->app_state->event_bus->fire_event(immer::box<events::VideoSession>(video_session));

      auto audio_session = start_req.value().audio_session;
      audio_session.session_id = session_id; // Can't be JSON encoded
      state_->app_state->event_bus->fire_event(immer::box<events::AudioSession>(audio_session));

      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, start_req.error().what());
    auto res = GenericErrorResponse{.error = start_req.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionPause(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionPauseRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::PauseStreamEvent>(events::PauseStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionStop(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto session = rfl::json::read<StreamSessionStopRequest>(req.body);
  if (session) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(session.value().session_id);
    if (state::get_session_by_id(sessions.get(), session_id)) {
      this->state_->app_state->event_bus->fire_event(
          immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = session_id}));
      auto res = GenericSuccessResponse{.success = true};
      send_http(socket, 200, rfl::json::write(res));
      return;
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", session.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, session.error().what());
    auto res = GenericErrorResponse{.error = session.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_StreamSessionHandleInput(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_request = rfl::json::read<StreamSessionHandleInputRequest>(req.body);
  if (input_request) {
    auto sessions = state_->app_state->running_sessions->load();
    auto session_id = std::stoul(input_request.value().session_id);
    if (auto session = state::get_session_by_id(sessions.get(), session_id)) {
      auto hex_pkt = input_request.value().input_packet_hex.get();
      auto pkt_parsed = crypto::hex_to_str(hex_pkt);
      moonlight::control::pkts::INPUT_PKT *input_pkt = reinterpret_cast<moonlight::control::pkts::INPUT_PKT *>(pkt_parsed.data());
      ::control::handle_input(session.value(), {}, input_pkt);

      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{.success = true}));
    } else {
      logs::log(logs::warning, "[API] Invalid session_id: {}", input_request.value().session_id);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Invalid session_id"}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, input_request.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = input_request.error().what()}));
  }
}

void UnixSocketServer::endpoint_Lobbies(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  auto res = LobbiesResponse{.lobbies = lobbies | //
                                        ranges::views::transform([](const events::Lobby &lobby) {
                                          return rfl::Reflector<events::Lobby>::from(lobby);
                                        }) | //
                                        ranges::to_vector};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_LobbyCreate(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<CreateLobbyRequest>(req.body);
  if (event) {
    auto default_client_settings = state::ClientSettings{};
    auto client_settings = event.value().client_settings.value().value_or(PartialClientSettings{});
    auto lobby_id = state::gen_uuid();
    auto create_lobby_ev = events::CreateLobbyEvent{
        .id = lobby_id,
        .profile_id = event.value().profile_id.get(),
        .name = event.value().name,
        .icon_png_path = event.value().icon_png_path,
        .pin = event.value().pin.get(),
        .multi_user = event.value().multi_user,
        .stop_when_everyone_leaves = event.value().stop_when_everyone_leaves,
        .video_settings = event.value().video_settings,
        .audio_settings = event.value().audio_settings,
        .client_settings =
            state::ClientSettings{
                .run_uid = client_settings.run_uid.value_or(default_client_settings.run_uid),
                .run_gid = client_settings.run_gid.value_or(default_client_settings.run_gid),
                .controllers_override =
                    client_settings.controllers_override.value_or(default_client_settings.controllers_override),
                .mouse_acceleration =
                    client_settings.mouse_acceleration.value_or(default_client_settings.mouse_acceleration),
                .v_scroll_acceleration =
                    client_settings.v_scroll_acceleration.value_or(default_client_settings.v_scroll_acceleration),
                .h_scroll_acceleration =
                    client_settings.h_scroll_acceleration.value_or(default_client_settings.h_scroll_acceleration)},
        .runner_state_folder = event.value().runner_state_folder,
        .runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus)};
    // Fire the event
    state_->app_state->event_bus->fire_event(immer::box<events::CreateLobbyEvent>(create_lobby_ev));

    auto setup_over_future = create_lobby_ev.on_setup_over.get()->get_future();
    auto result = setup_over_future.wait_for(std::chrono::seconds(10));
    if (result == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby setup timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby setup timed out"}));
    } else {
      auto res = LobbyCreateResponse{.lobby_id = lobby_id};
      send_http(socket, 200, rfl::json::write(res));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

std::optional<std::string /* Error message */> check_lobby_pin(const immer::vector<events::Lobby> &lobbies,
                                                               std::string_view lobby_id,
                                                               const std::optional<std::vector<short>> &pin) {
  auto lobby = state::get_lobby_by_id(lobbies, lobby_id);
  if (!lobby) {
    return "Invalid lobby ID";
  }
  if (lobby->pin != pin) {
    return "Invalid PIN";
  }
  return std::nullopt;
}

void UnixSocketServer::endpoint_LobbyJoin(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::JoinLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    auto lobby_ev = event.value();
    lobby_ev.error_message = std::make_shared<std::promise<std::string>>();
    state_->app_state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>(lobby_ev));

    auto error_message_fut = lobby_ev.error_message.get()->get_future();
    auto future_status = error_message_fut.wait_for(std::chrono::seconds(2));
    if (future_status == std::future_status::timeout) {
      logs::log(logs::warning, "[API] Lobby join timed out");
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Lobby join timed out"}));
    } else if (auto error_message = error_message_fut.get(); !error_message.empty()) {
      logs::log(logs::warning, "[API] Lobby join failed: {}", error_message);
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = utils::to_string(error_message)}));
    } else {
      send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
    }
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyLeave(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::LeaveLobbyEvent>(req.body);
  if (event) {
    state_->app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_LobbyStop(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<events::StopLobbyEvent>(req.body);
  if (event) {
    auto lobbies = this->state_->app_state->lobbies->load();
    if (auto err = check_lobby_pin(lobbies.get(), event->lobby_id, event->pin)) {
      send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = err.value()}));
      return;
    }
    state_->app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>(event.value()));
    send_http(socket, 200, rfl::json::write(GenericSuccessResponse{}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = event.error().what()}));
  }
}

void UnixSocketServer::endpoint_RunnerStart(const wolf::api::HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto event = rfl::json::read<RunnerStartRequest>(req.body);
  if (event) {
    auto session = state::get_session_by_id(this->state_->app_state->running_sessions->load(),
                                            std::stoul(event.value().session_id));
    if (!session) {
      logs::log(logs::warning, "[API] Invalid session_id: {}", event.value().session_id);
      auto res = GenericErrorResponse{.error = "Invalid session_id"};
      send_http(socket, 500, rfl::json::write(res));
      return;
    }

    auto runner = state::get_runner(event.value().runner, this->state_->app_state->event_bus);
    state_->app_state->event_bus->fire_event(immer::box<events::StartRunner>(
        events::StartRunner{.stop_stream_when_over = event.value().stop_stream_when_over,
                            .runner = runner,
                            .stream_session = std::make_shared<events::StreamSession>(*session)}));
  } else {
    logs::log(logs::warning, "[API] Invalid event: {} - {}", req.body, event.error().what());
    auto res = GenericErrorResponse{.error = event.error().what()};
    send_http(socket, 500, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_UpdateClientSettings(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto payload_result = rfl::json::read<UpdateClientSettingsRequest>(req.body);
  if (!payload_result) {
    auto res = GenericErrorResponse{.error = "Invalid request format"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  const auto &payload = payload_result.value();
  auto current_client = state::get_client_by_id(this->state_->app_state->config, payload.client_id.value());
  if (!current_client) {
    auto res = GenericErrorResponse{.error = "Client not found"};
    send_http(socket, 404, rfl::json::write(res));
    return;
  }

  // Edit only the settings that are being passed in the payload
  auto current_settings = current_client->settings;
  auto new_settings = payload.settings.get().value_or(PartialClientSettings{});
  auto merged_client = config::PairedClient{
      .client_cert = current_client->client_cert, // Immutable, changing this would mean a new client
      .app_state_folder = payload.app_state_folder.get().value_or(current_client->app_state_folder),
      .settings = config::ClientSettings{
          .run_uid = new_settings.run_gid.value_or(current_settings.run_uid),
          .run_gid = new_settings.run_gid.value_or(current_settings.run_gid),
          .controllers_override = new_settings.controllers_override.value_or(current_settings.controllers_override),
          .mouse_acceleration = new_settings.mouse_acceleration.value_or(current_settings.mouse_acceleration),
          .v_scroll_acceleration = new_settings.v_scroll_acceleration.value_or(current_settings.v_scroll_acceleration),
          .h_scroll_acceleration = new_settings.h_scroll_acceleration.value_or(current_settings.h_scroll_acceleration),
      }};

  update_client_settings(this->state_->app_state->config, std::stoull(payload.client_id.value()), merged_client);

  auto res = GenericSuccessResponse{.success = true};
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_GetIcon(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto icon_path = utils::split(req.query_string, '=');
  if (icon_path.size() != 2 || icon_path[0] != "icon_path") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'icon_path' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }
  // TODO: implement coroutines for CURL
  std::thread([this, socket, icon_path = utils::to_string(icon_path[1])]() {
    if (auto icon = utils::get_icon(this->state_->app_state->host->local_base_state_folder, icon_path)) {
      send_http(socket,
                200,
                {"Content-Length: " + std::to_string(icon->size()), "Content-Type: image/png"},
                icon.value());
    } else {
      auto res = GenericErrorResponse{.error = "Icon not found"};
      send_http(socket, 404, rfl::json::write(res));
    }
  }).detach();
}

void UnixSocketServer::endpoint_DockerInspectImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto image_name = utils::split(req.query_string, '=');
  if (image_name.size() != 2 || image_name[0] != "image_name") {
    auto res = GenericErrorResponse{.error = "Invalid request format, expects 'image_name' as a query parameter"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
  if (auto response = docker_api.inspect_image(image_name[1])) {
    send_http(socket, 200, response.value());
  } else {
    auto res = GenericErrorResponse{.error = "Image not found"};
    send_http(socket, 404, rfl::json::write(res));
  }
}

void UnixSocketServer::endpoint_DockerPullImage(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto input_payload = rfl::json::read<DockerPullImageRequest>(req.body);
  if (input_payload) {
    // TODO: implement coroutines for CURL
    std::thread([this, socket, image = input_payload.value().image_name]() {
      docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
      bool first_send = true;
      broadcast_event("DockerPullImageStartEvent",
                      rfl::json::write(events::DockerPullImageStartEvent{.image_name = image}));
      if (docker_api.pull_image(image,
                                {},
                                [this, &first_send, socket](const docker::DockerAPI::DockerProgressEvent &progress_ev) {
                                  if (first_send) {
                                    send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
                                    first_send = false;
                                  }
                                  auto serialized_ev = rfl::json::write(progress_ev) + "\r\n";
                                  send_data(socket, serialized_ev);
                                })) {
        if (first_send) {
          send_data(socket, "HTTP/1.0 200 OK\r\n\r\n");
        }
        auto final_result = rfl::json::write(GenericSuccessResponse{.success = true});
        send_data(socket, final_result + "\r\n");
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = true}));
      } else {
        send_http(socket, 500, rfl::json::write(GenericErrorResponse{.error = "Failed to pull image"}));
        broadcast_event("DockerPullImageEndEvent",
                        rfl::json::write(events::DockerPullImageEndEvent{.image_name = image, .success = false}));
      }
    }).detach();
  }
}

// Cache for GPU stats to prevent spamming nvidia-smi/rocm-smi
// GPU tools can be slow (50-200ms), so we cache for 2 seconds
static std::optional<GPUStats> cached_gpu_stats;
static std::chrono::steady_clock::time_point last_gpu_query_time;
static const std::chrono::seconds GPU_CACHE_DURATION{2};

// GPU vendor detection (cached)
enum class GPUVendor { Unknown, NVIDIA, AMD };
static GPUVendor detected_gpu_vendor = GPUVendor::Unknown;
static bool gpu_vendor_detected = false;

GPUVendor detectGPUVendor() {
  if (gpu_vendor_detected) {
    return detected_gpu_vendor;
  }

  // Check for nvidia-smi
  if (std::system("which nvidia-smi > /dev/null 2>&1") == 0) {
    detected_gpu_vendor = GPUVendor::NVIDIA;
    gpu_vendor_detected = true;
    logs::log(logs::info, "[GPU] Detected NVIDIA GPU (nvidia-smi available)");
    return detected_gpu_vendor;
  }

  // Check for rocm-smi (AMD)
  if (std::system("which rocm-smi > /dev/null 2>&1") == 0) {
    detected_gpu_vendor = GPUVendor::AMD;
    gpu_vendor_detected = true;
    logs::log(logs::info, "[GPU] Detected AMD GPU (rocm-smi available)");
    return detected_gpu_vendor;
  }

  detected_gpu_vendor = GPUVendor::Unknown;
  gpu_vendor_detected = true;
  logs::log(logs::warning, "[GPU] No GPU monitoring tool found (neither nvidia-smi nor rocm-smi)");
  return detected_gpu_vendor;
}

// Helper to execute command and get output
std::pair<std::string, int> execCommand(const std::string& cmd) {
  std::array<char, 512> buffer{};
  std::string result;

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return {"", -1};
  }

  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }

  int return_code = pclose(pipe);
  return {result, return_code};
}

GPUStats queryNVIDIAStats() {
  GPUStats stats{};
  auto query_start = std::chrono::steady_clock::now();

  // Execute nvidia-smi to query GPU metrics
  std::string cmd = "nvidia-smi --query-gpu=name,encoder.stats.sessionCount,encoder.stats.averageFps,"
                    "encoder.stats.averageLatency,utilization.encoder,utilization.gpu,utilization.memory,"
                    "memory.used,memory.total,temperature.gpu --format=csv,noheader,nounits";

  auto [result, return_code] = execCommand(cmd);

  auto query_end = std::chrono::steady_clock::now();
  stats.query_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(query_end - query_start).count();

  if (return_code != 0) {
    stats.error = fmt::format("nvidia-smi exited with code {}", return_code);
    stats.available = false;
    return stats;
  }

  // Parse CSV output
  std::istringstream iss(result);
  std::string field;
  std::vector<std::string> fields;

  while (std::getline(iss, field, ',')) {
    field.erase(0, field.find_first_not_of(" \t\r\n"));
    field.erase(field.find_last_not_of(" \t\r\n") + 1);
    fields.push_back(field);
  }

  if (fields.size() != 10) {
    stats.error = fmt::format("Unexpected nvidia-smi output: expected 10 fields, got {}", fields.size());
    stats.available = false;
    return stats;
  }

  stats.gpu_name = fields[0];
  stats.encoder_session_count = std::stoi(fields[1]);
  stats.encoder_average_fps = std::stod(fields[2]);
  stats.encoder_average_latency_us = std::stoi(fields[3]);
  stats.encoder_utilization_percent = std::stoi(fields[4]);
  stats.gpu_utilization_percent = std::stoi(fields[5]);
  stats.memory_utilization_percent = std::stoi(fields[6]);
  stats.memory_used_mb = std::stoi(fields[7]);
  stats.memory_total_mb = std::stoi(fields[8]);
  stats.temperature_celsius = std::stoi(fields[9]);
  stats.available = true;

  logs::log(logs::debug, "[GPU] nvidia-smi query took {}ms: {} NVENC sessions active",
            stats.query_duration_ms, stats.encoder_session_count);

  return stats;
}

GPUStats queryAMDStats() {
  GPUStats stats{};
  auto query_start = std::chrono::steady_clock::now();

  // Get GPU name from rocm-smi -a --json (reliable for GPU name)
  auto [info_result, info_code] = execCommand("rocm-smi -a --json 2>/dev/null");
  if (info_code == 0 && !info_result.empty()) {
    // Parse JSON to extract "Device Name" field
    // Format: {"card0": {"Device Name": "AMD Radeon Pro V710 MxGPU", ...}}
    size_t name_pos = info_result.find("\"Device Name\":");
    if (name_pos != std::string::npos) {
      size_t quote_start = info_result.find('"', name_pos + 14);
      if (quote_start != std::string::npos) {
        size_t quote_end = info_result.find('"', quote_start + 1);
        if (quote_end != std::string::npos) {
          stats.gpu_name = info_result.substr(quote_start + 1, quote_end - quote_start - 1);
        }
      }
    }
  }

  // Get VRAM info from rocm-smi --showmeminfo vram --json
  // Format: {"card0": {"VRAM Total Memory (B)": "9126805504", "VRAM Total Used Memory (B)": "238620672"}}
  auto [mem_result, mem_code] = execCommand("rocm-smi --showmeminfo vram --json 2>/dev/null");
  if (mem_code == 0 && !mem_result.empty()) {
    // Parse VRAM Total
    size_t total_pos = mem_result.find("\"VRAM Total Memory (B)\":");
    if (total_pos != std::string::npos) {
      size_t quote_start = mem_result.find('"', total_pos + 24);
      if (quote_start != std::string::npos) {
        size_t quote_end = mem_result.find('"', quote_start + 1);
        if (quote_end != std::string::npos) {
          std::string total_str = mem_result.substr(quote_start + 1, quote_end - quote_start - 1);
          try {
            int64_t total_bytes = std::stoll(total_str);
            stats.memory_total_mb = static_cast<int>(total_bytes / (1024 * 1024));
          } catch (...) {}
        }
      }
    }

    // Parse VRAM Used
    size_t used_pos = mem_result.find("\"VRAM Total Used Memory (B)\":");
    if (used_pos != std::string::npos) {
      size_t quote_start = mem_result.find('"', used_pos + 29);
      if (quote_start != std::string::npos) {
        size_t quote_end = mem_result.find('"', quote_start + 1);
        if (quote_end != std::string::npos) {
          std::string used_str = mem_result.substr(quote_start + 1, quote_end - quote_start - 1);
          try {
            int64_t used_bytes = std::stoll(used_str);
            stats.memory_used_mb = static_cast<int>(used_bytes / (1024 * 1024));
          } catch (...) {}
        }
      }
    }

    // Calculate memory utilization
    if (stats.memory_total_mb > 0) {
      stats.memory_utilization_percent = (stats.memory_used_mb * 100) / stats.memory_total_mb;
    }
  }

  auto query_end = std::chrono::steady_clock::now();
  stats.query_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(query_end - query_start).count();

  // AMD doesn't expose encoder stats like NVIDIA, so set defaults
  stats.encoder_session_count = 0;
  stats.encoder_average_fps = 0.0;
  stats.encoder_average_latency_us = 0;
  stats.encoder_utilization_percent = 0;
  stats.gpu_utilization_percent = 0;  // Could be obtained from amd-smi metric but often N/A
  stats.temperature_celsius = 0;  // Could be obtained from rocm-smi -t

  // Mark as available if we got at least the GPU name or memory info
  stats.available = !stats.gpu_name.empty() || stats.memory_total_mb > 0;

  if (stats.available) {
    logs::log(logs::debug, "[GPU] rocm-smi query took {}ms: {} ({} MB / {} MB used)",
              stats.query_duration_ms, stats.gpu_name, stats.memory_used_mb, stats.memory_total_mb);
  } else {
    stats.error = "Failed to query AMD GPU stats via rocm-smi";
  }

  return stats;
}

GPUStats queryGPUStats() {
  auto now = std::chrono::steady_clock::now();

  // Return cached stats if less than 2 seconds old
  if (cached_gpu_stats.has_value() &&
      (now - last_gpu_query_time) < GPU_CACHE_DURATION) {
    return *cached_gpu_stats;
  }

  GPUStats stats{};
  GPUVendor vendor = detectGPUVendor();

  switch (vendor) {
    case GPUVendor::NVIDIA:
      stats = queryNVIDIAStats();
      break;
    case GPUVendor::AMD:
      stats = queryAMDStats();
      break;
    default:
      stats.available = false;
      stats.error = "No supported GPU monitoring tool found";
      break;
  }

  // Cache the result
  cached_gpu_stats = stats;
  last_gpu_query_time = now;

  return stats;
}

void UnixSocketServer::endpoint_SystemMemory(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = SystemMemoryResponse{};

  // Read process RSS memory from /proc/self/status
  std::ifstream status_file("/proc/self/status");
  std::string line;
  size_t rss_kb = 0;

  while (std::getline(status_file, line)) {
    if (line.find("VmRSS:") == 0) {
      // Extract RSS value in kB
      std::istringstream iss(line);
      std::string label;
      iss >> label >> rss_kb; // Format: "VmRSS:     123456 kB"
      break;
    }
  }

  res.process_rss_bytes = rss_kb * 1024; // Convert kB to bytes

  // Get apps from moonlight profile for backwards compatibility with stable-moonlight-web frontend
  auto moonlight_profile = state::get_moonlight_profile(state_->app_state->config);
  if (moonlight_profile) {
    immer::vector<immer::box<events::App>> apps = moonlight_profile.value()->apps->load();
    for (const immer::box<events::App> &app_box : apps) {
      const events::App &app = *app_box;
      // Apps don't have streaming sessions directly in lobbies mode, so set client_count to 0
      res.apps.push_back(AppMemoryUsage{
        .app_id = app.base.id,
        .app_name = app.base.title,
        .resolution = "N/A", // Apps don't stream directly in lobbies mode
        .client_count = 0,
        .memory_bytes = 0
      });
    }
  }

  // Get lobbies and calculate per-lobby memory breakdown
  immer::vector<events::Lobby> lobbies = state_->app_state->lobbies->load();
  size_t total_lobby_memory = 0;

  for (const events::Lobby &lobby : lobbies) {
    auto connected_sessions = lobby.connected_sessions->load();
    size_t client_count = connected_sessions.get().size();

    // Estimate memory per lobby based on resolution and client count
    // Formula: base overhead + (width * height * bytes_per_pixel * buffer_count) + (client_count * transcoding_overhead)
    // Rough estimates:
    // - Base overhead per lobby: ~50 MB (wayland display, audio sink, runner state)
    // - Video buffer: width * height * 4 bytes (RGBA) * 3 buffers
    // - Per-client transcoding: ~20 MB per client (encoder state, RTP buffers)

    size_t base_overhead = 50 * 1024 * 1024; // 50 MB

    // Get actual video settings from lobby
    int width = lobby.video_settings.width;
    int height = lobby.video_settings.height;
    int fps = lobby.video_settings.refresh_rate;

    size_t video_buffers = width * height * 4 * 3; // RGBA * 3 buffers
    size_t client_overhead = client_count * 20 * 1024 * 1024; // 20 MB per client

    size_t lobby_memory = base_overhead + video_buffers + client_overhead;
    total_lobby_memory += lobby_memory;

    std::string resolution_str = std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(fps);

    res.lobbies.push_back(LobbyMemoryUsage{
      .lobby_id = lobby.id,
      .lobby_name = lobby.name,
      .resolution = resolution_str,
      .client_count = client_count,
      .memory_bytes = lobby_memory
    });
  }

  // Iterate over ALL client connections (StreamSessions) for leak detection
  immer::vector<events::StreamSession> sessions = state_->app_state->running_sessions->load();
  for (const events::StreamSession &session : sessions) {
    // Estimate memory per client connection
    // - Base client overhead: ~10 MB (session state, buffers)
    // - Video encoder state: ~15 MB
    // - Audio encoder state: ~5 MB
    // - Per-client total: ~30 MB
    size_t client_memory = 30 * 1024 * 1024;

    // Check if this client is connected to a lobby
    std::optional<std::string> lobby_id;
    auto lobby = state::get_lobby_by_connected_session(lobbies, std::to_string(session.session_id));
    if (lobby) {
      lobby_id = lobby->id;
    }

    // Get client resolution from display_mode
    std::string client_resolution = std::to_string(session.display_mode.width) + "x" +
                                   std::to_string(session.display_mode.height) + "@" +
                                   std::to_string(session.display_mode.refreshRate);

    // For compatibility: provide both lobby_id (wolf-ui) and app_id (stable-moonlight-web)
    std::optional<std::string> app_id;
    if (session.app) {
      app_id = session.app->base.id;
    }

    res.clients.push_back(ClientConnectionInfo{
      .session_id = session.session_id,
      .client_ip = session.ip,
      .resolution = client_resolution,
      .lobby_id = lobby_id,
      .app_id = app_id,
      .memory_bytes = client_memory
    });
  }

  // GStreamer buffer estimate (rough approximation)
  // This includes interpipe buffers, encoder buffers, RTP buffers
  res.gstreamer_buffer_bytes = total_lobby_memory / 2; // Rough estimate: ~50% of lobby memory is GStreamer buffers

  res.total_memory_bytes = res.process_rss_bytes;

  // Count actual GStreamer pipelines from state (not estimated)
  // Each lobby has 2 pipelines: video producer + audio producer
  // Each session has 2 pipelines: video consumer + audio consumer
  GStreamerPipelineStats pipeline_stats{};
  pipeline_stats.producer_pipelines = lobbies.size() * 2;
  pipeline_stats.consumer_pipelines = sessions.size() * 2;
  pipeline_stats.total_pipelines = pipeline_stats.producer_pipelines + pipeline_stats.consumer_pipelines;
  res.gstreamer_pipelines = pipeline_stats;

  // Query GPU stats via nvidia-smi (with caching to prevent spam)
  res.gpu_stats = queryGPUStats();

  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_SystemHealth(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = SystemHealthResponse{};

  // Calculate process uptime
  static auto process_start_time = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  res.process_uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - process_start_time).count();

  // Get thread health from monitor
  auto thread_statuses = wolf::monitoring::ThreadMonitor::get().get_all_threads();

  for (const auto& status : thread_statuses) {
    res.threads.push_back(ThreadHealthInfo{
      .tid = status.tid,
      .name = status.name,
      .details = status.pipeline_desc,
      .seconds_since_heartbeat = status.seconds_since_heartbeat,
      .seconds_alive = status.seconds_alive,
      .heartbeat_count = status.heartbeat_count,
      .is_stuck = status.is_stuck,
      .current_request_path = status.current_request_path,
      .request_duration_seconds = status.request_duration_seconds,
      .has_active_request = status.has_active_request,
      .stack_trace = status.stack_trace
    });
  }

  res.total_thread_count = res.threads.size();
  res.stuck_thread_count = 0;
  for (const auto& t : res.threads) {
    if (t.is_stuck) {
      res.stuck_thread_count++;
    }
  }

  // Test if new pipelines can be created (real deadlock detection)
  // Production deadlocked with only 35% threads stuck, but new sessions couldn't start
  // This test detects the ACTUAL failure: global GLib type lock held
  res.can_create_new_pipelines = wolf::monitoring::ThreadMonitor::can_create_new_pipelines();

  // Determine overall status based on pipeline creation test (not thread percentage)
  // If pipeline creation fails → CRITICAL (new sessions won't work)
  // Thread stuck count is just context
  if (!res.can_create_new_pipelines) {
    res.overall_status = "critical";  // Type lock held - new sessions blocked
  } else if (res.stuck_thread_count == 0) {
    res.overall_status = "healthy";
  } else {
    res.overall_status = "degraded";  // Some threads stuck, but new sessions OK
  }

  logs::log(logs::debug, "[HEALTH] Status={} threads={} stuck={} pipelines={}",
            res.overall_status, res.total_thread_count, res.stuck_thread_count,
            res.can_create_new_pipelines ? "OK" : "BLOCKED");

  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_KeyboardState(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto res = KeyboardStateResponse{};

  // Get keyboard state from our tracker
  auto& tracker = wolf::control::KeyboardStateTracker::get();
  auto sessions = tracker.get_all_sessions();

  for (const auto& session : sessions) {
    SessionKeyboardState state;
    state.session_id = std::to_string(session.session_id);
    state.timestamp_ms = session.last_update_ms;

    // Convert pressed keys to vectors
    for (short key : session.pressed_keys) {
      state.pressed_keys.push_back(static_cast<int32_t>(key));
      state.pressed_key_names.push_back(wolf::control::moonlight_key_to_name(key));
    }

    // Determine modifier state
    state.modifier_state.shift = wolf::control::is_shift_pressed(session.pressed_keys);
    state.modifier_state.ctrl = wolf::control::is_ctrl_pressed(session.pressed_keys);
    state.modifier_state.alt = wolf::control::is_alt_pressed(session.pressed_keys);
    state.modifier_state.meta = wolf::control::is_meta_pressed(session.pressed_keys);

    state.device_name = session.device_name.empty() ? "Wolf Virtual Keyboard" : session.device_name;

    res.sessions.push_back(state);
  }

  logs::log(logs::debug, "[KEYBOARD] Returning state for {} sessions", res.sessions.size());
  send_http(socket, 200, rfl::json::write(res));
}

void UnixSocketServer::endpoint_KeyboardReset(const HTTPRequest &req, std::shared_ptr<UnixSocket> socket) {
  auto req_data = rfl::json::read<KeyboardResetRequest>(req.body);
  if (!req_data) {
    auto res = GenericErrorResponse{.error = "Invalid JSON: " + std::string(req_data.error().what())};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  auto session_id_str = req_data.value().session_id;
  std::size_t session_id;
  try {
    session_id = std::stoull(session_id_str);
  } catch (...) {
    auto res = GenericErrorResponse{.error = "Invalid session_id format"};
    send_http(socket, 400, rfl::json::write(res));
    return;
  }

  // Get keyboard state and reset it
  auto& tracker = wolf::control::KeyboardStateTracker::get();
  auto released_keys = tracker.reset_session(session_id);

  // Also release keys on the inputtino keyboard
  auto sessions = state_->app_state->running_sessions->load();
  for (const auto& session : sessions.get()) {
    if (session.session_id == session_id && session.keyboard->has_value()) {
      for (short key : released_keys) {
        std::visit([key](auto &keyboard) { keyboard.release(key); }, session.keyboard->value());
      }
      logs::log(logs::info, "[KEYBOARD] Reset keyboard state for session {}, released {} keys",
                session_id, released_keys.size());
      break;
    }
  }

  KeyboardResetResponse res;
  res.success = true;
  for (short key : released_keys) {
    res.released_keys.push_back(wolf::control::moonlight_key_to_name(key));
  }
  res.message = "Released " + std::to_string(released_keys.size()) + " stuck keys";

  send_http(socket, 200, rfl::json::write(res));
}

} // namespace wolf::api