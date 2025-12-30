#include <immer/vector_transient.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <state/data-structures.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

/**
 * @brief Removes the StreamSession from the input Lobby and switches everything to the original session
 *
 * @param skip_producer_switch If true, skip switching the interpipesrc back to the session's test pattern.
 *        This should be true when the session is being canceled, because the test pattern producer
 *        is already destroyed - switching to it would corrupt the interpipe state.
 *
 * @note Leaving a lobby may have side effects,
 * like terminating the lobby if it becomes empty or triggering additional events.
 */
void leave_lobby(const std::shared_ptr<events::EventBusType> &ev_bus,
                 const events::Lobby &lobby,
                 const events::StreamSession &session,
                 bool skip_producer_switch = false) {
  logs::log(logs::info, "[LOBBY] Session {} leaving lobby {} (skip_producer_switch={})",
            session.session_id, lobby.id, skip_producer_switch);
  // Remove the current session from the lobby list
  lobby.connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
    return connected_sessions | //
           ranges::views::filter([session](const immer::box<std::string> &session_id) {
             return *session_id != std::to_string(session.session_id);
           }) | //
           ranges::to<immer::vector<immer::box<std::string>>>();
  });

  // Switch over mouse and keyboard to use the original session's input method
  bool use_pipewire_mode = lobby.video_settings.video_source_mode == "pipewire";

  if (use_pipewire_mode) {
    // PipeWire mode: Create new inputtino devices for the session's test pattern
    logs::log(logs::debug, "[LOBBY] Creating inputtino devices for session {} leaving PipeWire lobby", session.session_id);

    auto mouse = input::Mouse::create();
    if (mouse) {
      session.mouse->emplace(input::Mouse(std::move(*mouse)));
    }

    auto keyboard = input::Keyboard::create();
    if (keyboard) {
      session.keyboard->emplace(input::Keyboard(std::move(*keyboard)));
    }

    auto touch = input::TouchScreen::create();
    if (touch) {
      session.touch_screen->emplace(input::TouchScreen(std::move(*touch)));
    }
  } else {
    // Wayland mode: Switch back to session's Wayland compositor
    auto wl_state = session.wayland_display->load();
    session.mouse->emplace(virtual_display::WaylandMouse(wl_state));
    session.keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
    session.touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));
  }

  // Switch over all joypads present in the lobby back into the original session
  events::JoypadList joypads = session.joypads->load();
  for (auto [_joypad_nr, joypad] : joypads) {
    // Plug them into original session
    events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session.session_id)};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *joypad);
    ev_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
    // Unplug them from the current lobby
    ev_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
        events::UnplugDeviceEvent{.session_id = lobby.id,
                                  .udev_events = plug_ev.udev_events,
                                  .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});
  }
  // TODO: hotplug pen_tablet and touch_screen

  // Switch audio/video gstreamer stream producers back to the session's test pattern
  // UNLESS skip_producer_switch is true (session is being canceled, test pattern already destroyed)
  if (!skip_producer_switch) {
    ev_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
        events::SwitchStreamProducerEvents{.session_id = session.session_id,
                                           .interpipe_src_id = std::to_string(session.session_id)}});
  } else {
    logs::log(logs::info, "[LOBBY] Skipping producer switch for session {} (session being canceled)",
              session.session_id);
  }

  if (lobby.stop_when_everyone_leaves && lobby.connected_sessions->load()->size() == 0) {
    // Nobody left in the lobby, and it's set to stop when everyone leaves
    ev_bus->fire_event(immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby.id}});
  }
}

immer::vector<immer::box<events::EventBusHandlers>>
setup_lobbies_handlers(const immer::box<state::AppState> &app_state,
                       const std::string &runtime_dir,
                       const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  // On create lobby event
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::CreateLobbyEvent>>(
      [=](const immer::box<events::CreateLobbyEvent> &lobby_settings) {
        logs::log(logs::info, "[LOBBY] Creating new lobby");
        auto ev_bus = app_state->event_bus;

        // Compute runner state folder path for PipeWire socket sharing
        auto runner_state_path = (std::filesystem::path(app_state->host->local_base_state_folder) /
                                  lobby_settings->runner_state_folder).string();

        auto lobby = std::make_shared<events::Lobby>(
            events::Lobby{.id = lobby_settings->id,
                          .name = lobby_settings->name,
                          .started_by_profile_id = lobby_settings->profile_id,
                          .icon_png_path = lobby_settings->icon_png_path,
                          .multi_user = lobby_settings->multi_user,
                          .runner_state_folder_path = runner_state_path,
                          .pin = lobby_settings->pin,
                          .stop_when_everyone_leaves = lobby_settings->stop_when_everyone_leaves,
                          .runner = lobby_settings->runner,
                          .video_settings = lobby_settings->video_settings});
        app_state->lobbies->update(
            [lobby](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(*lobby); });

        bool use_pipewire_mode = lobby_settings->video_settings.video_source_mode == "pipewire";

        if (use_pipewire_mode) {
          // PipeWire mode: Start runner FIRST, wait for container to report node ID
          // Video producer will be started when SetPipeWireNodeIdEvent is received
          logs::log(logs::info, "[LOBBY] Using PipeWire video source mode (GNOME 49+)");
          logs::log(logs::debug, "[LOBBY] Starting runner first, video producer will start when node ID is reported");

          auto full_path = std::filesystem::path(app_state->host->local_base_state_folder) /
                           lobby_settings->runner_state_folder;
          std::filesystem::create_directories(full_path);

          std::thread([=]() {
            try {
              start_runner(lobby->runner,
                           lobby->plugged_devices_queue,
                           immer::box<RunnerArgs>{RunnerArgs{
                               .session_id = lobby->id,
                               .video_settings = lobby_settings->video_settings,
                               .wayland_display = nullptr, // No Wayland display for PipeWire mode
                               .audio_server = audio_server,
                               .audio_sink = lobby->audio_sink->load(),
                               .host = app_state->host,
                               .app_local_state_folder = full_path.string(),
                               .app_host_state_folder = std::filesystem::path(app_state->host->host_base_state_folder) /
                                                        lobby_settings->runner_state_folder,
                               .xdg_runtime_dir = runtime_dir,
                               .client_settings = lobby_settings->client_settings}});
              // Runner process ended, stop the lobby
              ev_bus->fire_event<immer::box<events::StopLobbyEvent>>(
                  immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
            } catch (const std::exception &e) {
              logs::log(logs::error, "[LOBBY] Runner thread exception: {}", e.what());
            } catch (...) {
              logs::log(logs::error, "[LOBBY] Runner thread unknown exception");
            }
          }).detach();

          // Signal setup complete - video producer will be started when node ID is reported
          lobby_settings->on_setup_over.get()->set_value(true);

        } else {
          // Wayland mode: Start video producer first (nested compositor for Sway/KDE)
          logs::log(logs::debug, "[LOBBY] Create wayland compositor");

          std::shared_ptr<boost::promise<streaming::WaylandDisplayReady>> on_ready =
              std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();

          std::thread([lobby, lobby_settings, ev_bus, on_ready, gst_context = app_state->gst_context]() {
            try {
              streaming::start_video_producer(lobby->id,
                                              lobby_settings->video_settings.video_producer_buffer_caps,
                                              lobby_settings->video_settings.wayland_render_node,
                                              {.width = lobby_settings->video_settings.width,
                                               .height = lobby_settings->video_settings.height,
                                               .refreshRate = lobby_settings->video_settings.refresh_rate},
                                              gst_context,
                                              on_ready,
                                              ev_bus);
            } catch (const std::exception &e) {
              logs::log(logs::error, "[LOBBY] Video producer thread exception: {}", e.what());
            } catch (...) {
              logs::log(logs::error, "[LOBBY] Video producer thread unknown exception");
            }
          }).detach();

          auto w_display_ready = on_ready->get_future().then(
              [lobby, runtime_dir, ev_bus, audio_server, lobby_settings, host = app_state->host](auto fut) {
                streaming::WaylandDisplayReady ready = fut.get();

                auto wl_state =
                    virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
                // Set the wayland display
                lobby->wayland_display->store(wl_state);

                { // Start runner
                  logs::log(logs::debug, "[LOBBY] Start runner");
                  auto full_path = std::filesystem::path(host->local_base_state_folder) /
                                   lobby_settings->runner_state_folder;
                  logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
                  std::filesystem::create_directories(full_path);

                  std::thread([=]() {
                    try {
                      start_runner(lobby->runner,
                                   lobby->plugged_devices_queue,
                                   immer::box<RunnerArgs>{RunnerArgs{
                                       .session_id = lobby->id,
                                       .video_settings = lobby_settings->video_settings,
                                       .wayland_display = lobby->wayland_display->load(),
                                       .audio_server = audio_server,
                                       .audio_sink = lobby->audio_sink->load(),
                                       .host = host,
                                       .app_local_state_folder = full_path.string(),
                                       .app_host_state_folder = std::filesystem::path(host->host_base_state_folder) /
                                                                lobby_settings->runner_state_folder,
                                       .xdg_runtime_dir = runtime_dir,
                                       .client_settings = lobby_settings->client_settings}});
                      // Runner process ended, stop the lobby
                      lobby->wayland_display->store(nullptr);

                      ev_bus->fire_event<immer::box<events::StopLobbyEvent>>(
                          immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
                    } catch (const std::exception &e) {
                      logs::log(logs::error, "[LOBBY] Runner thread exception: {}", e.what());
                    } catch (...) {
                      logs::log(logs::error, "[LOBBY] Runner thread unknown exception");
                    }
                  }).detach();
                }

                lobby_settings->on_setup_over.get()->set_value(true);
              });
        }

        { // Create audio virtual sink
          logs::log(logs::debug, "[LOBBY] Create audio virtual sink");
          auto pulse_sink_name = fmt::format("virtual_sink_{}", lobby->id);
          if (audio_server && audio_server->server) {
            auto channel_count = lobby_settings->audio_settings.channel_count;
            auto v_device = audio::create_virtual_sink(
                audio_server->server,
                audio::AudioDevice{.sink_name = pulse_sink_name, .mode = state::get_audio_mode(channel_count, true)});

            lobby->audio_sink->store(v_device);

            // Start Gstreamer producer pipeline
            std::thread([lobby, audio_server = audio_server->server, ev_bus, channel_count]() {
              try {
                auto sink_name = fmt::format("virtual_sink_{}.monitor", lobby->id);
                streaming::start_audio_producer(lobby->id,
                                                ev_bus,
                                                channel_count,
                                                sink_name,
                                                audio::get_server_name(audio_server));
              } catch (const std::exception &e) {
                logs::log(logs::error, "[LOBBY] Audio producer thread exception: {}", e.what());
              } catch (...) {
                logs::log(logs::error, "[LOBBY] Audio producer thread unknown exception");
              }
            }).detach();
          }
        }
      }));

  // When a Moonlight client joins a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::JoinLobbyEvent>>(
      [=](const immer::box<events::JoinLobbyEvent> &join_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), join_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), join_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to join lobby: lobby {} or session {} not found",
                    join_lobby_event->lobby_id,
                    join_lobby_event->moonlight_session_id);
          join_lobby_event->error_message.get()->set_value("Lobby or session not found");
          return;
        }
        logs::log(logs::info, "[LOBBY] Session {} joining lobby {}", session->session_id, lobby->id);

        // DEFENSIVE CHECK 1: Verify session isn't already in a different lobby
        // This prevents GStreamer pipeline conflicts from switching between lobbies
        for (const auto &existing_lobby : *lobbies) {
          auto connected_sessions = existing_lobby.connected_sessions->load();
          for (const auto &connected_session_id : *connected_sessions) {
            if (std::to_string(session->session_id) == *connected_session_id) {
              if (existing_lobby.id == lobby->id) {
                // Already in target lobby - this is fine (idempotent join)
                logs::log(logs::info,
                          "[LOBBY] Session {} already in lobby {} - returning success (idempotent)",
                          session->session_id,
                          lobby->id);
                join_lobby_event->error_message.get()->set_value("");
                return;
              } else {
                // In different lobby - reject the join to prevent pipeline conflicts
                logs::log(logs::error,
                          "[LOBBY] DEFENSIVE CHECK FAILED: Session {} already in lobby {}, cannot join lobby {}",
                          session->session_id,
                          existing_lobby.id,
                          lobby->id);
                join_lobby_event->error_message.get()->set_value(
                    "Session already in different lobby - leave first before joining another");
                return;
              }
            }
          }
        }

        if (!lobby->multi_user && lobby->connected_sessions->load()->size() >= 1) {
          logs::log(logs::error, "[LOBBY] Lobby {} is full", lobby->id);
          join_lobby_event->error_message.get()->set_value("Lobby is full");
          return;
        }

        // Update the lobby with the new session
        lobby->connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
          return connected_sessions.push_back({std::to_string(session->session_id)});
        });

        // switch mouse and keyboard in session to use the lobby's input method
        bool use_pipewire_mode = lobby->video_settings.video_source_mode == "pipewire";

        if (use_pipewire_mode) {
          // PipeWire mode: Use inputtino (kernel evdev) devices instead of Wayland
          // These are passed to the container via fake-udev
          logs::log(logs::debug, "[LOBBY] Creating inputtino devices for PipeWire mode lobby {}", lobby->id);

          auto mouse = input::Mouse::create();
          if (!mouse) {
            logs::log(logs::error, "[LOBBY] Failed to create mouse: {}", mouse.getErrorMessage());
          } else {
            auto mouse_ptr = input::Mouse(std::move(*mouse));
            // Plug device into the lobby's container
            lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = mouse_ptr.get_udev_events(),
                                        .udev_hw_db_entries = mouse_ptr.get_udev_hw_db_entries()}));
            session->mouse->emplace(std::move(mouse_ptr));
          }

          auto keyboard = input::Keyboard::create();
          if (!keyboard) {
            logs::log(logs::error, "[LOBBY] Failed to create keyboard: {}", keyboard.getErrorMessage());
          } else {
            auto keyboard_ptr = input::Keyboard(std::move(*keyboard));
            lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = keyboard_ptr.get_udev_events(),
                                        .udev_hw_db_entries = keyboard_ptr.get_udev_hw_db_entries()}));
            session->keyboard->emplace(std::move(keyboard_ptr));
          }

          auto touch = input::TouchScreen::create();
          if (!touch) {
            logs::log(logs::error, "[LOBBY] Failed to create touch screen: {}", touch.getErrorMessage());
          } else {
            auto touch_ptr = input::TouchScreen(std::move(*touch));
            lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = touch_ptr.get_udev_events(),
                                        .udev_hw_db_entries = touch_ptr.get_udev_hw_db_entries()}));
            session->touch_screen->emplace(std::move(touch_ptr));
          }
        } else {
          // Wayland mode: Use the lobby's Wayland compositor for input
          auto wl_state = lobby->wayland_display->load();
          session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
          session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
          session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));
        }

        // Switch over all joypads present in the session into the lobby
        events::JoypadList joypads = session->joypads->load();
        for (auto [_joypad_nr, joypad] : joypads) {
          events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session->session_id)};
          std::visit(
              [&plug_ev](auto &pad) {
                plug_ev.udev_events = pad.get_udev_events();
                plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
              },
              *joypad);
          app_state->event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
          // Unplug it from the current session
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
              events::UnplugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = plug_ev.udev_events,
                                        .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});

          // Add it to the current lobby devices queue
          lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{plug_ev});
        }
        // TODO: hotplug pen_tablet

        // Switch audio/video gstreamer stream producers
        app_state->event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
            events::SwitchStreamProducerEvents{.session_id = session->session_id, .interpipe_src_id = lobby->id}});
        join_lobby_event->error_message.get()->set_value("");
      }));

  // When a Moonlight session leaves the lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [=](const immer::box<events::LeaveLobbyEvent> &leave_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), leave_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), leave_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to leave lobby: lobby {} or session {} not found",
                    leave_lobby_event->lobby_id,
                    leave_lobby_event->moonlight_session_id);
        } else {
          leave_lobby(app_state->event_bus, lobby.value(), session.value(), leave_lobby_event->skip_producer_switch);
        }
      }));

  // Stopping a lobby will trigger leave for all the connected sessions
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
      [=](const immer::box<events::StopLobbyEvent> &stop_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), stop_lobby_event->lobby_id);

        if (!lobby) {
          logs::log(logs::warning, "[LOBBY] lobby {} not found", stop_lobby_event->lobby_id);
          return;
        }
        logs::log(logs::info, "[LOBBY] stopping lobby {}", stop_lobby_event->lobby_id);

        immer::vector<immer::box<std::string>> sessions = lobby->connected_sessions->load();
        for (auto &session_id : sessions) {
          app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
              events::LeaveLobbyEvent{.lobby_id = lobby->id, .moonlight_session_id = std::stoul(*session_id)}});
        }

        // Finally, remove the lobby from the app_state
        app_state->lobbies->update([stop_lobby_event](const immer::vector<events::Lobby> &lobbies) {
          return lobbies | //
                 ranges::views::filter([stop_lobby_event](const events::Lobby &lobby) {
                   return lobby.id != stop_lobby_event->lobby_id;
                 }) | //
                 ranges::to<immer::vector<events::Lobby>>();
        });
      }));

  // On a PlugDeviceEvent, we have to add the device to the lobby queue so that the runner will pick it up
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [=](const immer::box<events::PlugDeviceEvent> &plug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_by_connected_session(lobbies, plug_device_event->session_id)) {
          logs::log(logs::info,
                    "[LOBBY] adding device to session {} in lobby {}",
                    plug_device_event->session_id,
                    lobby->id);

          lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{
              events::PlugDeviceEvent{.session_id = lobby->id,
                                      .udev_events = plug_device_event->udev_events,
                                      .udev_hw_db_entries = plug_device_event->udev_hw_db_entries}});
        }
      }));

  // When a device is unplugged from a Moonlight session, we have to re-fire the event on our lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [=](const immer::box<events::UnplugDeviceEvent> &unplug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_by_connected_session(lobbies, unplug_device_event->session_id)) {
          logs::log(logs::debug, "[LOBBY] Unplug device for session {}", unplug_device_event->session_id);
          app_state->event_bus->fire_event(
              events::UnplugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = unplug_device_event->udev_events,
                                        .udev_hw_db_entries = unplug_device_event->udev_hw_db_entries});
        }
      }));

  auto on_moonlight_session_over = [app_state](std::size_t moonlight_session_id) {
    immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
    if (auto lobby = state::get_lobby_by_connected_session(lobbies, std::to_string(moonlight_session_id))) {
      logs::log(logs::info, "[LOBBY] Moonlight stream {} over, leaving lobby {}", moonlight_session_id, lobby->id);
      // Fire the LeaveLobbyEvent so that it can also be picked up by WolfUI via SSE
      // skip_producer_switch = true because the streaming pipeline is stopping anyway.
      // There's no point switching interpipesrc to a different source in a stopping pipeline.
      // When the session resumes, a NEW streaming pipeline is created with fresh listen-to.
      app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
          events::LeaveLobbyEvent{.lobby_id = lobby->id,
                                  .moonlight_session_id = moonlight_session_id,
                                  .skip_producer_switch = true}});
    }
  };

  // When a Moonlight client Pauses a session, we get the user out of a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
      [=](const immer::box<events::PauseStreamEvent> &pause_stream_event) {
        on_moonlight_session_over(pause_stream_event->session_id);
      }));

  // When a Moonlight client Stops a session, we get the user out of a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [=](const immer::box<events::StopStreamEvent> &stop_stream_event) {
        on_moonlight_session_over(stop_stream_event->session_id);
      }));

  // When a container reports its PipeWire ScreenCast node ID, start the pipewiresrc video producer
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::SetPipeWireNodeIdEvent>>(
      [=](const immer::box<events::SetPipeWireNodeIdEvent> &node_id_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), node_id_event->lobby_id);

        if (!lobby) {
          logs::log(logs::error, "[LOBBY] SetPipeWireNodeIdEvent: lobby {} not found", node_id_event->lobby_id);
          return;
        }

        if (lobby->video_settings.video_source_mode != "pipewire") {
          logs::log(logs::warning, "[LOBBY] SetPipeWireNodeIdEvent: lobby {} not in pipewire mode, ignoring",
                    node_id_event->lobby_id);
          return;
        }

        // Store the node ID
        lobby->pipewire_node_id->store(node_id_event->node_id);
        logs::log(logs::info, "[LOBBY] PipeWire node ID {} received for lobby {}, starting pipewiresrc video producer",
                  node_id_event->node_id, lobby->id);

        // Start the pipewiresrc video producer
        auto ev_bus = app_state->event_bus;
        auto gst_context = app_state->gst_context;
        auto video_settings = lobby->video_settings;

        std::thread([lobby_id = lobby->id, node_id = node_id_event->node_id, video_settings, ev_bus, gst_context,
                     pipewire_socket_path = lobby->runner_state_folder_path]() {
          try {
            // Create a promise that we won't use (pipewiresrc doesn't need wayland display setup)
            std::shared_ptr<boost::promise<streaming::WaylandDisplayReady>> on_ready =
                std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();

            streaming::start_pipewire_video_producer(lobby_id,
                                                     node_id,
                                                     pipewire_socket_path,
                                                     video_settings.video_producer_buffer_caps,
                                                     video_settings.wayland_render_node,
                                                     {.width = video_settings.width,
                                                      .height = video_settings.height,
                                                      .refreshRate = video_settings.refresh_rate},
                                                     gst_context,
                                                     on_ready,
                                                     ev_bus);
          } catch (const std::exception &e) {
            logs::log(logs::error, "[LOBBY] PipeWire video producer thread exception: {}", e.what());
          } catch (...) {
            logs::log(logs::error, "[LOBBY] PipeWire video producer thread unknown exception");
          }
        }).detach();
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions