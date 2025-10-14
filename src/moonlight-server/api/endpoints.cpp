#include <api/api.hpp>
#include <control/input_handler.hpp>
#include <core/docker.hpp>
#include <rtp/udp-ping.hpp>
#include <state/config.hpp>
#include <state/sessions.hpp>
#include <state/utils.hpp>
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
    if (app_with_defaults.h264_gst_pipeline.empty()) {
      app_with_defaults.h264_gst_pipeline = defaults.h264_gst_pipeline;
    }
    if (app_with_defaults.hevc_gst_pipeline.empty()) {
      app_with_defaults.hevc_gst_pipeline = defaults.hevc_gst_pipeline;
    }
    if (app_with_defaults.av1_gst_pipeline.empty()) {
      app_with_defaults.av1_gst_pipeline = defaults.av1_gst_pipeline;
    }
    if (app_with_defaults.opus_gst_pipeline.empty()) {
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
        ss.aes_iv);
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
      control::INPUT_PKT *input_pkt = reinterpret_cast<control::INPUT_PKT *>(pkt_parsed.data());
      control::handle_input(session.value(), {}, input_pkt);

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

  send_http(socket, 200, rfl::json::write(res));
}

} // namespace wolf::api