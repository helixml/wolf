#pragma once

#include "state/config.hpp"
#include "state/data-structures.hpp"

#include <events/events.hpp>
#include <rfl.hpp>
#include <state/serialised_config.hpp>

namespace rfl {

using namespace wolf::core;
using namespace wolf::config;

template <> struct Reflector<events::PairSignal> {
  struct ReflType {
    std::string client_ip;
    std::string host_ip;
  };

  static events::PairSignal to(const ReflType &v) noexcept {
    return {.client_ip = v.client_ip,
            .host_ip = v.host_ip,
            .user_pin = std::make_shared<boost::promise<std::string>>()};
  }

  static ReflType from(const events::PairSignal &v) {
    return {.client_ip = v.client_ip, .host_ip = v.host_ip};
  }
};

template <> struct Reflector<events::Runner> {
  using ReflType = events::RunnerTypes;
  static ReflType from(const events::Runner &v) {
    return v.serialize();
  }

  static std::shared_ptr<events::Runner> to(const ReflType &v, const std::shared_ptr<events::EventBusType> &ev_bus) {
    return state::get_runner(v, ev_bus);
  }
};

template <> struct Reflector<events::App> {
  struct ReflType {
    const std::string title;
    const std::string id;
    const bool support_hdr;
    std::optional<std::string> icon_png_path;

    std::optional<std::string> video_producer_buffer_caps;
    std::optional<std::string> h264_gst_pipeline;
    std::optional<std::string> hevc_gst_pipeline;
    std::optional<std::string> av1_gst_pipeline;

    std::optional<std::string> render_node;

    std::optional<std::string> opus_gst_pipeline;
    std::optional<bool> start_virtual_compositor;
    std::optional<bool> start_audio_server;
    Reflector<events::Runner>::ReflType runner;
  };

  static ReflType from(const events::App &v) {
    return {.title = v.base.title,
            .id = v.base.id,
            .support_hdr = v.base.support_hdr,
            .icon_png_path = v.base.icon_png_path,
            .video_producer_buffer_caps = v.video_producer_buffer_caps,
            .h264_gst_pipeline = v.h264_gst_pipeline,
            .hevc_gst_pipeline = v.hevc_gst_pipeline,
            .av1_gst_pipeline = v.av1_gst_pipeline,
            .render_node = v.render_node,
            .opus_gst_pipeline = v.opus_gst_pipeline,
            .start_virtual_compositor = v.start_virtual_compositor,
            .start_audio_server = v.start_audio_server,
            .runner = v.runner->serialize()};
  }

  static events::App to(const ReflType &app, const std::shared_ptr<events::EventBusType> &ev_bus) {
    auto runner = Reflector<events::Runner>::to(app.runner, ev_bus);
    return events::App{
        .base = {.title = app.title, .id = app.id, .support_hdr = app.support_hdr, .icon_png_path = app.icon_png_path},
        .video_producer_buffer_caps = app.video_producer_buffer_caps.value_or(""),
        .h264_gst_pipeline = app.h264_gst_pipeline.value_or(""),
        .hevc_gst_pipeline = app.hevc_gst_pipeline.value_or(""),
        .av1_gst_pipeline = app.av1_gst_pipeline.value_or(""),
        .render_node = app.render_node.value_or(""),
        .opus_gst_pipeline = app.opus_gst_pipeline.value_or(""),
        .start_virtual_compositor = app.start_virtual_compositor.value_or(true),
        .start_audio_server = app.start_audio_server.value_or(true),
        .runner = runner,
    };
  }
};

template <> struct Reflector<events::Profile> {
  struct ReflType {
    std::string name;
    std::string id;
    std::string icon_png_path;
    std::optional<std::vector<short>> pin;
    std::vector<Reflector<events::App>::ReflType> apps;
  };

  static ReflType from(const events::Profile &v) {
    immer::vector<immer::box<events::App>> apps_boxed = v.apps->load();
    std::vector<Reflector<events::App>::ReflType> apps;
    for (const auto &app : apps_boxed) {
      apps.push_back(Reflector<events::App>::from(*app));
    }
    return {.name = v.name, .id = v.id, .icon_png_path = v.icon_png_path, .pin = v.pin, .apps = apps};
  }

  static events::Profile to(const ReflType &v, const std::shared_ptr<events::EventBusType> &ev_bus) {
    auto parsed_apps = v.apps | //
                       ranges::views::transform([ev_bus](const auto &app) {
                         return immer::box<events::App>(Reflector<events::App>::to(app, ev_bus));
                       }) |
                       ranges::to<immer::vector<immer::box<events::App>>>();

    return {.id = v.id,
            .name = v.name,
            .icon_png_path = v.icon_png_path,
            .pin = v.pin,
            .apps = std::make_shared<immer::atom<immer::vector<immer::box<events::App>>>>(parsed_apps)};
  }
};

template <> struct Reflector<events::StartRunner> {
  struct ReflType {
    bool stop_stream_when_over;
    events::RunnerTypes runner;
    std::string session_id;
  };

  static ReflType from(const events::StartRunner &v) {
    return {.stop_stream_when_over = v.stop_stream_when_over,
            .runner = v.runner->serialize(),
            .session_id = std::to_string(v.stream_session->session_id)};
  }
};

template <> struct Reflector<events::StreamSession> {
  struct ReflType {
    std::string app_id;
    std::string client_id;
    std::string client_ip;
    std::string client_unique_id;  // Moonlight uniqueid for secure session matching

    // gcm encryption keys
    std::string aes_key;
    std::string aes_iv;
    std::string rtsp_fake_ip;

    int video_width;
    int video_height;
    int video_refresh_rate;

    int audio_channel_count;

    wolf::config::ClientSettings client_settings;

    // Observability: seconds since last ENET packet received
    // Used by session timeout monitor to detect stale sessions
    int idle_seconds;
  };

  static ReflType from(const events::StreamSession &v) {
    // Compute idle_seconds from last_activity timestamp
    int idle_seconds = 0;
    if (v.last_activity) {
      auto now = std::chrono::steady_clock::now();
      auto last = v.last_activity->load();
      idle_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
    }

    return {.app_id = v.app->base.id,
            .client_id = std::to_string(v.session_id),
            .client_ip = v.ip,
            .client_unique_id = v.client_unique_id,  // Expose uniqueid for secure auto-join
            .aes_key = v.aes_key,
            .aes_iv = v.aes_iv,
            .rtsp_fake_ip = v.rtsp_fake_ip,
            .video_width = v.display_mode.width,
            .video_height = v.display_mode.height,
            .video_refresh_rate = v.display_mode.refreshRate,
            .audio_channel_count = v.audio_channel_count,
            .client_settings = v.client_settings,
            .idle_seconds = idle_seconds};
  }
};

template <> struct Reflector<events::Lobby> {
  struct ReflType {
    std::string id;
    std::string name;
    std::optional<std::string> icon_png_path;
    bool multi_user;
    std::string started_by_profile_id;
    bool pin_required;
    bool stop_when_everyone_leaves;
    Reflector<events::Runner>::ReflType runner;
    std::vector<std::string> connected_sessions;
  };

  static ReflType from(const events::Lobby &v) {
    immer::vector<immer::box<std::string>> connected_sessions = v.connected_sessions->load();
    return {.id = v.id,
            .name = v.name,
            .icon_png_path = v.icon_png_path,
            .multi_user = v.multi_user,
            .started_by_profile_id = v.started_by_profile_id,
            .pin_required = v.pin.has_value(),
            .stop_when_everyone_leaves = v.stop_when_everyone_leaves,
            .runner = v.runner->serialize(),
            .connected_sessions = connected_sessions |
                                  ranges::views::transform([](const immer::box<std::string> &v) { return *v; }) |
                                  ranges::to_vector};
  }
};

/**
 * Unsigned long is used by Moonlight as the session ID but JSON doesn't support unsigned integers.
 */
template <> struct Reflector<std::size_t> {
  using ReflType = std::string;
  static ReflType from(const std::size_t &v) {
    return std::to_string(v);
  }
  static std::size_t to(const ReflType &v) {
    return std::stoull(v);
  }
};

} // namespace rfl