#include "platforms/hw.hpp"

#include <control/control.hpp>
#include <gst-video-context.hpp>
#include <gstreamer-1.0/gst/app/gstappsink.h>
#include <gstreamer-1.0/gst/app/gstappsrc.h>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <memory>
#include <streaming/streaming.hpp>

namespace streaming {

using namespace wolf::core::gstreamer;
using namespace wolf::core;

struct GstBusData {
  std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready;
  gst_element_ptr wayland_plugin;
};

gboolean structure_each(GQuark field_id, const GValue *value, gpointer user_data) {
  auto field_str = std::string(g_quark_to_string(field_id));
  if (!G_VALUE_HOLDS_STRING(value)) {
    logs::log(logs::warning, "Wayland source message: {} = {}", field_str, "not a string");
    return FALSE;
  }
  auto value_str = g_value_get_string(value);
  logs::log(logs::debug, "Wayland source message: {} = {}", field_str, value_str);

  if (field_str == "WAYLAND_DISPLAY") {
    logs::log(logs::info, "Wayland display ready, listening on: {}", value_str);
    auto bus_data = static_cast<GstBusData *>(user_data);
    bus_data->on_ready->set_value(
        WaylandDisplayReady{.wayland_socket_name = value_str, .wayland_plugin = bus_data->wayland_plugin});
  }

  return TRUE;
}

static void application_message_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto structure = gst_message_get_structure(msg);
  if (gst_structure_has_name(structure, "wayland.src")) {
    gst_structure_foreach(structure, structure_each, data);
  }
}

struct NeedContextData {
  const std::string device_path;
  std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> gst_context;
};

static void need_context_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto ctx_data = static_cast<NeedContextData *>(data);
  if (auto gst_context = ctx_data->gst_context->load().get()) {
    logs::log(logs::debug, "Context already set, passing it to the pipeline.");
    gst_video_context::set_context(gst_context, msg);
  } else if (auto video_context = gst_video_context::need_context_for_device(ctx_data->device_path, msg)) {
    ctx_data->gst_context->store(video_context);
  }
}

static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    need_context_handler(bus, msg, data);
  }
  return GST_BUS_PASS;
}

std::pair<std::string, std::string> get_color_params(immer::box<events::VideoSession> video_session) {
  std::string color_range = (video_session->color_range == events::ColorRange::JPEG) ? "jpeg" : "mpeg2";
  std::string color_space;
  switch (video_session->color_space) {
  case events::ColorSpace::BT601:
    color_space = "bt601";
    break;
  case events::ColorSpace::BT709:
    color_space = "bt709";
    break;
  case events::ColorSpace::BT2020:
    color_space = "bt2020";
    break;
  }
  return std::make_pair(color_range, color_space);
}

void start_video_producer(const std::string &session_id,
                          const std::string &buffer_format,
                          const std::string &render_node,
                          const wolf::core::virtual_display::DisplayMode &display_mode,
                          std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                          std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready,
                          std::shared_ptr<events::EventBusType> event_bus) {
  auto pipeline = fmt::format("waylanddisplaysrc name=wolf_wayland_source render_node={render_node} ! "
                              "{buffer_format}, width={width}, height={height}, framerate={fps}/1 ! \n"    //
                              "interpipesink sync=true async=false name={session_id}_video max-buffers=5", //
                              fmt::arg("buffer_format", buffer_format),
                              fmt::arg("render_node", render_node),
                              fmt::arg("session_id", session_id),
                              fmt::arg("width", display_mode.width),
                              fmt::arg("height", display_mode.height),
                              fmt::arg("fps", display_mode.refreshRate));
  logs::log(logs::debug, "[GSTREAMER] Starting video producer: {}", pipeline);
  auto bus_data_ptr =
      std::make_shared<GstBusData>(GstBusData{.on_ready = std::move(on_ready), .wayland_plugin = nullptr});
  std::shared_ptr<NeedContextData> ctx_data_ptr =
      std::make_shared<NeedContextData>(NeedContextData{.device_path = render_node, .gst_context = video_context});
  run_pipeline(pipeline, [=](auto pipeline, auto loop) {
    logs::log(logs::debug, "Setting up waylanddisplaysrc");

    auto wayland_plugin_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_wayland_source");
    auto wayland_plugin_ptr = gst_element_ptr(wayland_plugin_el, ::gst_object_unref);
    bus_data_ptr->wayland_plugin.swap(wayland_plugin_ptr);

    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    g_signal_connect(bus, "message::application", G_CALLBACK(application_message_handler), bus_data_ptr.get());
    gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);
    gst_object_unref(bus);

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (std::to_string(ev->session_id) == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, loop](const immer::box<events::StopLobbyEvent> &ev) {
          if (ev->lobby_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler), std::move(stop_lobby_handler)};
  });
}

void start_audio_producer(const std::string &session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name) {
  std::string channel_mask;
  switch (channel_count) {
  case 2:
    channel_mask = "0x3";
    break;
  case 6:
    channel_mask = "0x3f";
    break;
  case 8:
    channel_mask = "0xc3f";
    break;
  default:
    channel_mask = "";
  }

  auto pipeline = fmt::format("pulsesrc device=\"{sink_name}\" server=\"{server_name}\" ! "                           //
                              "audio/x-raw, channels={channels}, channel-mask=(bitmask){channel_mask}, rate=48000 ! " //
                              "queue leaky=downstream max-size-buffers=3 ! "                                          //
                              "interpipesink name=\"{session_id}_audio\" sync=true async=false max-buffers=3",
                              fmt::arg("session_id", session_id),
                              fmt::arg("channels", channel_count),
                              fmt::arg("channel_mask", channel_mask),
                              fmt::arg("sink_name", sink_name),
                              fmt::arg("server_name", server_name));
  logs::log(logs::debug, "[GSTREAMER] Starting audio producer: {}", pipeline);

  run_pipeline(pipeline, [=](auto pipeline, auto loop) {
    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (std::to_string(ev->session_id) == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping audio producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, loop](const immer::box<events::StopLobbyEvent> &ev) {
          if (ev->lobby_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping audio producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler), std::move(stop_lobby_handler)};
  });
}

void start_test_pattern_producer(const std::string &session_id,
                                 const std::string &source_pipeline,
                                 const std::string &buffer_caps,
                                 const std::string &render_node,
                                 const wolf::core::virtual_display::DisplayMode &display_mode,
                                 std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                                 std::shared_ptr<events::EventBusType> event_bus) {
  // Format the source pipeline with display mode parameters
  auto formatted_source = fmt::format(fmt::runtime(source_pipeline),
                                      fmt::arg("width", display_mode.width),
                                      fmt::arg("height", display_mode.height),
                                      fmt::arg("fps", display_mode.refreshRate));

  // Build GPU upload element based on buffer_caps to ensure consistent memory format
  // This prevents buffer pool corruption when interpipesrc switches between test pattern and lobby
  //
  // CRITICAL: Output caps MUST EXACTLY MATCH waylanddisplaysrc's format:
  //   waylanddisplaysrc ! {buffer_caps}, width={width}, height={height}, framerate={fps}/1 ! interpipesink
  //
  // Previous fix had explicit format=NV12 and missing framerate, causing caps negotiation
  // differences that led to black screen on second session.
  std::string gpu_upload;
  if (buffer_caps.find("CUDAMemory") != std::string::npos) {
    // NVIDIA: upload to CUDA memory, use EXACT same caps format as waylanddisplaysrc
    gpu_upload = fmt::format("cudaupload ! "
                             "{}, width={}, height={}, framerate={}/1",
                             buffer_caps, display_mode.width, display_mode.height, display_mode.refreshRate);
    logs::log(logs::info, "[GSTREAMER] Test pattern using CUDA memory upload (matching waylanddisplaysrc)");
  } else if (buffer_caps.find("DMABuf") != std::string::npos) {
    // AMD/Intel: use VA-API postprocessor, output DMABuf with EXACT same caps format as waylanddisplaysrc
    gpu_upload = fmt::format("vapostproc ! "
                             "{}, width={}, height={}, framerate={}/1",
                             buffer_caps, display_mode.width, display_mode.height, display_mode.refreshRate);
    logs::log(logs::info, "[GSTREAMER] Test pattern using DMABuf memory upload (matching waylanddisplaysrc)");
  } else if (buffer_caps.find("VAMemory") != std::string::npos) {
    // Fallback for explicit VAMemory caps (rare) - use same format as waylanddisplaysrc
    gpu_upload = fmt::format("vapostproc ! "
                             "{}, width={}, height={}, framerate={}/1",
                             buffer_caps, display_mode.width, display_mode.height, display_mode.refreshRate);
    logs::log(logs::info, "[GSTREAMER] Test pattern using VAMemory upload (matching waylanddisplaysrc)");
  } else {
    // Fallback: no GPU upload (CPU memory) - may cause issues with lobby switching
    gpu_upload = fmt::format("video/x-raw, format=NV12, width={}, height={}, framerate={}/1",
                             display_mode.width, display_mode.height, display_mode.refreshRate);
    logs::log(logs::warning, "[GSTREAMER] Test pattern using CPU memory (no GPU upload) - "
                             "lobby switching may cause black screen");
  }

  auto pipeline = fmt::format("{source} ! {gpu_upload} ! "
                              "interpipesink sync=true async=false name={session_id}_video max-buffers=5",
                              fmt::arg("source", formatted_source),
                              fmt::arg("gpu_upload", gpu_upload),
                              fmt::arg("session_id", session_id));
  logs::log(logs::debug, "[GSTREAMER] Starting test pattern producer: {}", pipeline);

  // CRITICAL: Set up CUDA context sharing so cudaupload uses the same CUDA context as waylanddisplaysrc.
  // Without this, nvh264enc receives buffers from a different CUDA context when interpipesrc switches
  // from test pattern to lobby, causing NV_ENC_ERR_RESOURCE_REGISTER_FAILED (0x17).
  std::shared_ptr<NeedContextData> ctx_data_ptr =
      std::make_shared<NeedContextData>(NeedContextData{.device_path = render_node, .gst_context = video_context});

  run_pipeline(pipeline, [=](auto pipeline, auto loop) {
    // Set up CUDA context handler on bus - this ensures cudaupload shares the same CUDA context
    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);
    gst_object_unref(bus);

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (std::to_string(ev->session_id) == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping test pattern producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, loop](const immer::box<events::StopLobbyEvent> &ev) {
          if (ev->lobby_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping test pattern producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    // NOTE: Don't stop on PauseStreamEvent - the test pattern producer must remain alive
    // so the session can still switch to a lobby. Pause != stop, and killing the producer
    // on pause causes black screens when a second session connects before the first joins a lobby.
    // The test pattern producer should only stop on StopStreamEvent (session fully terminated).

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler), std::move(stop_lobby_handler)};
  });
}

void start_test_audio_producer(const std::string &session_id,
                               const std::string &source_pipeline,
                               int channel_count,
                               std::shared_ptr<events::EventBusType> event_bus) {
  // Calculate channel mask based on channel count (same logic as start_audio_producer)
  uint64_t channel_mask;
  switch (channel_count) {
    case 2:   channel_mask = 0x3;       break;  // Front Left + Front Right
    case 6:   channel_mask = 0x3F;      break;  // 5.1
    case 8:   channel_mask = 0x63F;     break;  // 7.1
    default:  channel_mask = 0x3;       break;  // Fallback to stereo
  }

  auto pipeline = fmt::format("{source} ! "
                              "audio/x-raw, channels={channels}, channel-mask=(bitmask){channel_mask}, rate=48000 ! "
                              "queue leaky=downstream max-size-buffers=3 ! "
                              "interpipesink name=\"{session_id}_audio\" sync=true async=false max-buffers=3",
                              fmt::arg("source", source_pipeline),
                              fmt::arg("channels", channel_count),
                              fmt::arg("channel_mask", channel_mask),
                              fmt::arg("session_id", session_id));
  logs::log(logs::debug, "[GSTREAMER] Starting test audio producer: {}", pipeline);

  run_pipeline(pipeline, [=](auto pipeline, auto loop) {
    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (std::to_string(ev->session_id) == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping test audio producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, loop](const immer::box<events::StopLobbyEvent> &ev) {
          if (ev->lobby_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping test audio producer: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    // NOTE: Don't stop on PauseStreamEvent - see comment in start_test_pattern_producer

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler), std::move(stop_lobby_handler)};
  });
}

namespace custom_sink {

struct UDPSink {
  std::shared_ptr<udp::socket> socket;
  std::shared_ptr<udp::endpoint> client_endpoint;
};

static GstFlowReturn
send_buffer(std::shared_ptr<GstBuffer> buffer, std::shared_ptr<GstSample> sample, UDPSink *udp_sink) {
  GstMapInfo map;
  if (gst_buffer_map(buffer.get(), &map, GST_MAP_READ)) {
    std::shared_ptr<GstMapInfo> map_ptr = std::make_shared<GstMapInfo>(map);
    if (!udp_sink->socket->is_open()) {
      logs::log(logs::warning, "UDP Socket is not open");
      udp_sink->socket->open(udp::v4());
    }
    udp_sink->socket->async_send_to(
        boost::asio::buffer(map.data, map.size),
        *udp_sink->client_endpoint,
        [buffer, sample, map_ptr](const boost::system::error_code &error, std::size_t bytes_sent) {
          if (error) {
            logs::log(logs::error, "Error sending UDP packet: {}", error.message());
          }
          gst_buffer_unmap(buffer.get(), map_ptr.get());
        });
    return GST_FLOW_OK;
  } else {
    logs::log(logs::error, "Failed to map buffer");
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  std::shared_ptr<GstSample> sample(gst_app_sink_pull_sample(appsink), gst_sample_unref);
  if (!sample) {
    logs::log(logs::warning, "Custom sink: failed to create sample");
    return GST_FLOW_ERROR;
  }

  UDPSink *udp_sink = static_cast<UDPSink *>(user_data);

  if (GstBufferList *buffer_list = gst_sample_get_buffer_list(sample.get())) {
    // TODO: use boost to properly send multiple buffers in one go (scatter-gather I/O)
    for (guint i = 0; i < gst_buffer_list_length(buffer_list); ++i) {
      GstBuffer *buffer = gst_buffer_list_get(buffer_list, i);
      std::shared_ptr<GstBuffer> buffer_ptr(gst_buffer_ref(buffer), gst_buffer_unref);
      if (auto result = send_buffer(buffer_ptr, sample, udp_sink); result != GST_FLOW_OK) {
        return result;
      }
    }
    return GST_FLOW_OK;
  } else if (GstBuffer *buffer = gst_sample_get_buffer(sample.get())) {
    std::shared_ptr<GstBuffer> buffer_ptr(gst_buffer_ref(buffer), gst_buffer_unref);
    return send_buffer(buffer_ptr, sample, udp_sink);
  } else {
    logs::log(logs::warning, "Custom sink: failed to get buffer");
    return GST_FLOW_ERROR;
  }
}

static void configure_appsink(GstElement *appsink, UDPSink *udp_sink) {
  g_object_set(appsink, "emit-signals", FALSE, NULL);
  g_object_set(appsink, "buffer-list", TRUE, NULL);

  GstAppSinkCallbacks callbacks = {nullptr};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, udp_sink, nullptr);
}
} // namespace custom_sink

/**
 * Start VIDEO pipeline
 */
void start_streaming_video(immer::box<events::VideoSession> video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                           std::shared_ptr<udp::socket> video_socket) {
  auto [color_range, color_space] = get_color_params(video_session);

  auto pipeline = fmt::format(fmt::runtime(video_session->gst_pipeline),
                              fmt::arg("session_id", video_session->session_id),
                              fmt::arg("width", video_session->display_mode.width),
                              fmt::arg("height", video_session->display_mode.height),
                              fmt::arg("fps", video_session->display_mode.refreshRate),
                              fmt::arg("bitrate", video_session->bitrate_kbps),
                              fmt::arg("client_port", client_port),
                              fmt::arg("client_ip", client_ip),
                              fmt::arg("payload_size", video_session->packet_size),
                              fmt::arg("fec_percentage", video_session->fec_percentage),
                              fmt::arg("min_required_fec_packets", video_session->min_required_fec_packets),
                              fmt::arg("slices_per_frame", video_session->slices_per_frame),
                              fmt::arg("color_space", color_space),
                              fmt::arg("color_range", color_range),
                              fmt::arg("host_port", video_session->port));
  logs::log(logs::debug, "Starting video pipeline: \n{}", pipeline);

  std::shared_ptr<custom_sink::UDPSink> udp_sink = std::make_shared<custom_sink::UDPSink>(custom_sink::UDPSink{
      .socket = video_socket,
      .client_endpoint = std::make_shared<udp::endpoint>(boost::asio::ip::make_address(client_ip), client_port)});
  std::shared_ptr<NeedContextData> ctx_data_ptr = std::make_shared<NeedContextData>(
      NeedContextData{.device_path = video_session->render_node, .gst_context = video_context});
  run_pipeline(pipeline, [video_session, event_bus, udp_sink, ctx_data_ptr](auto pipeline, auto loop) {
    if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_udp_sink")) {
      logs::log(logs::debug, "Setting up wolf_udp_sink");
      g_assert(GST_IS_APP_SINK(app_sink_el));
      configure_appsink(app_sink_el, udp_sink.get());
      gst_object_unref(app_sink_el);
    }

    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);

    // Bus message handler for switch-interpipe-src (runs in pipeline thread - safe for g_object_set)
    auto sess_id_for_handler = video_session->session_id;
    g_signal_connect(bus, "message::application", G_CALLBACK(+[](GstBus*, GstMessage* msg, gpointer user_data) {
      const GstStructure* s = gst_message_get_structure(msg);
      if (gst_structure_has_name(s, "switch-interpipe-src")) {
        guint64 session_id;
        const char* interpipe_id;
        if (gst_structure_get_uint64(s, "session-id", &session_id) &&
            (interpipe_id = gst_structure_get_string(s, "interpipe-id"))) {

          logs::log(logs::warning, "[HANG_DEBUG] Pipeline thread handling switch-interpipe-src: session {}, target {}", session_id, interpipe_id);

          /* NOW we're in the pipeline thread - safe to call g_object_set */
          auto pipe_name = fmt::format("interpipesrc_{}_video", session_id);
          auto pipeline_ptr = GST_ELEMENT(GST_MESSAGE_SRC(msg));
          if (auto src = gst_bin_get_by_name(GST_BIN(pipeline_ptr), pipe_name.c_str())) {
            logs::log(logs::warning, "[HANG_DEBUG] Switching interpipesrc listen-to: {} → {}", pipe_name, interpipe_id);

            // Set allow-renegotiation to true to handle resolution changes
            g_object_set(src, "allow-renegotiation", TRUE, nullptr);
            g_object_set(src, "listen-to", interpipe_id, nullptr);

            logs::log(logs::warning, "[HANG_DEBUG] Unrefing interpipesrc element");
            gst_object_unref(src);
            logs::log(logs::warning, "[HANG_DEBUG] Switch complete for session {}", session_id);
          } else {
            logs::log(logs::error, "[GSTREAMER] Failed to get video interpipesrc for {}", session_id);
          }
        }
      }
    }), &sess_id_for_handler);

    gst_object_unref(bus);

    // Guard against duplicate pause events
    auto pause_sent = std::make_shared<bool>(false);

    /*
     * The force IDR event will be triggered by the control stream.
     * We have to pass this back into the gstreamer pipeline
     * in order to force the encoder to produce a new IDR packet
     */
    auto idr_handler = event_bus->register_handler<immer::box<events::IDRRequestEvent>>(
        [sess_id = video_session->session_id, pipeline](const immer::box<events::IDRRequestEvent> &ctrl_ev) {
          if (ctrl_ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Forcing IDR");
            // Force IDR event, see: https://github.com/centricular/gstwebrtc-demos/issues/186
            // https://gstreamer.freedesktop.org/documentation/additional/design/keyframe-force.html?gi-language=c
            wolf::core::gstreamer::send_message(
                pipeline.get(),
                gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL));
          }
        });

    auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [sess_id = video_session->session_id, loop, pause_sent](const immer::box<events::PauseStreamEvent> &ev) {
          if (ev->session_id == sess_id) {
            // Guard against duplicate pause events (bug fix for upstream issue)
            if (*pause_sent) {
              logs::log(logs::warning, "[HANG_DEBUG] Video PauseStreamEvent DUPLICATE IGNORED for session {}", sess_id);
              return;
            }
            *pause_sent = true;

            logs::log(logs::warning, "[HANG_DEBUG] Video PauseStreamEvent for session {} (quitting main loop)", sess_id);

            /**
             * Unfortunately here we can't just pause the pipeline,
             * when a pipeline will be resumed there are a lot of breaking changes
             * like:
             *  - Client IP:PORT
             *  - AES key and IV for encrypted payloads
             *  - Client resolution, framerate, and encoding
             *
             *  The only solution is to kill the pipeline and re-create it again
             * when a resume happens
             */

            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    // Guard against duplicate switch events (same as pause guard fix)
    auto last_video_switch = std::make_shared<std::string>("");

    auto switch_producer_handler = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
        [sess_id = video_session->session_id,
         pipeline, last_video_switch](const immer::box<events::SwitchStreamProducerEvents> &switch_ev) {
          if (switch_ev->session_id == sess_id) {
            // Guard against duplicate switch events to same destination
            if (*last_video_switch == switch_ev->interpipe_src_id) {
              logs::log(logs::warning, "[HANG_DEBUG] Video SwitchStreamProducerEvents DUPLICATE IGNORED: session {} already switched to {}", sess_id, switch_ev->interpipe_src_id);
              return;
            }

            // Update last_video_switch IMMEDIATELY to prevent race conditions
            *last_video_switch = switch_ev->interpipe_src_id;

            auto state = GST_STATE(pipeline.get());
            logs::log(logs::warning,
                      "[HANG_DEBUG] Video SwitchStreamProducerEvents: session {} switching to {}, pipeline state: {}",
                      sess_id,
                      switch_ev->interpipe_src_id,
                      gst_element_state_get_name(state));

            /* DEADLOCK FIX: Post message to pipeline bus instead of calling g_object_set directly
             * Problem: g_object_set acquires global GLib type lock - if this thread crashes while
             * holding it, ALL pipeline creation blocks (can't create new sessions).
             * Solution: Post application message to pipeline's bus, let pipeline thread handle it.
             * Pipeline thread owns the GMainContext, so g_object_set runs in the right thread.
             */
            auto video_interpipe = fmt::format("{}_video", switch_ev->interpipe_src_id);
            logs::log(logs::warning, "[HANG_DEBUG] Posting switch-interpipe-src message to pipeline bus: {}", video_interpipe);

            gst_element_post_message(pipeline.get(),
              gst_message_new_application(GST_OBJECT(pipeline.get()),
                gst_structure_new("switch-interpipe-src",
                  "session-id", G_TYPE_UINT64, (guint64)sess_id,
                  "interpipe-id", G_TYPE_STRING, video_interpipe.c_str(),
                  nullptr)));
          }
        });

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [sess_id = video_session->session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {} (quitting main loop)", sess_id);
            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(idr_handler),
                                                              std::move(pause_handler),
                                                              std::move(switch_producer_handler),
                                                              std::move(stop_handler)};
  });
}

/**
 * Start AUDIO pipeline
 */
void start_streaming_audio(immer::box<events::AudioSession> audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> audio_socket,
                           const std::string &sink_name,
                           const std::string &server_name) {
  auto pipeline = fmt::format(
      fmt::runtime(audio_session->gst_pipeline),
      fmt::arg("session_id", audio_session->session_id),
      fmt::arg("channels", audio_session->audio_mode.channels),
      fmt::arg("bitrate", audio_session->audio_mode.bitrate),
      // TODO: opusenc hardcodes those two
      // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/1.24.6/subprojects/gst-plugins-base/ext/opus/gstopusenc.c#L661-666
      fmt::arg("streams", audio_session->audio_mode.streams),
      fmt::arg("coupled_streams", audio_session->audio_mode.coupled_streams),
      fmt::arg("sink_name", sink_name),
      fmt::arg("server_name", server_name),
      fmt::arg("packet_duration", audio_session->packet_duration),
      fmt::arg("aes_key", audio_session->aes_key),
      fmt::arg("aes_iv", audio_session->aes_iv),
      fmt::arg("encrypt", audio_session->encrypt_audio),
      fmt::arg("client_port", client_port),
      fmt::arg("client_ip", client_ip),
      fmt::arg("host_port", audio_session->port));
  logs::log(logs::debug, "Starting audio pipeline: \n{}", pipeline);

  std::shared_ptr<custom_sink::UDPSink> udp_sink = std::make_shared<custom_sink::UDPSink>(custom_sink::UDPSink{
      .socket = audio_socket,
      .client_endpoint = std::make_shared<udp::endpoint>(boost::asio::ip::make_address(client_ip), client_port)});

  run_pipeline(pipeline, [session_id = audio_session->session_id, udp_sink, event_bus](auto pipeline, auto loop) {
    if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_udp_sink")) {
      logs::log(logs::debug, "Setting up wolf_udp_sink");
      g_assert(GST_IS_APP_SINK(app_sink_el));
      custom_sink::configure_appsink(app_sink_el, udp_sink.get());
      gst_object_unref(app_sink_el);
    }

    // Bus message handler for switch-interpipe-src-audio (runs in pipeline thread - safe for g_object_set)
    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    g_signal_connect(bus, "message::application", G_CALLBACK(+[](GstBus*, GstMessage* msg, gpointer user_data) {
      const GstStructure* s = gst_message_get_structure(msg);
      if (gst_structure_has_name(s, "switch-interpipe-src-audio")) {
        guint64 session_id;
        const char* interpipe_id;
        if (gst_structure_get_uint64(s, "session-id", &session_id) &&
            (interpipe_id = gst_structure_get_string(s, "interpipe-id"))) {

          logs::log(logs::warning, "[HANG_DEBUG] Audio pipeline thread handling switch-interpipe-src: session {}, target {}", session_id, interpipe_id);

          /* NOW we're in the pipeline thread - safe to call g_object_set */
          auto pipe_name = fmt::format("interpipesrc_{}_audio", session_id);
          auto pipeline_ptr = GST_ELEMENT(GST_MESSAGE_SRC(msg));
          if (auto src = gst_bin_get_by_name(GST_BIN(pipeline_ptr), pipe_name.c_str())) {
            logs::log(logs::warning, "[HANG_DEBUG] Switching audio interpipesrc listen-to: {} → {}", pipe_name, interpipe_id);

            g_object_set(src, "listen-to", interpipe_id, nullptr);

            logs::log(logs::warning, "[HANG_DEBUG] Unrefing audio interpipesrc element");
            gst_object_unref(src);
            logs::log(logs::warning, "[HANG_DEBUG] Audio switch complete for session {}", session_id);
          } else {
            logs::log(logs::error, "[GSTREAMER] Failed to get audio interpipesrc for {}", session_id);
          }
        }
      }
    }), nullptr);
    gst_object_unref(bus);

    // Guard against duplicate pause events
    auto pause_sent = std::make_shared<bool>(false);

    auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [session_id, loop, pause_sent](const immer::box<events::PauseStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            // Guard against duplicate pause events (bug fix for upstream issue)
            if (*pause_sent) {
              logs::log(logs::warning, "[HANG_DEBUG] Audio PauseStreamEvent DUPLICATE IGNORED for session {}", session_id);
              return;
            }
            *pause_sent = true;

            logs::log(logs::warning, "[HANG_DEBUG] Audio PauseStreamEvent for session {} (quitting main loop)", session_id);

            /**
             * Unfortunately here we can't just pause the pipeline,
             * when a pipeline will be resumed there are a lot of breaking changes
             * like:
             *  - Client IP:PORT
             *  - AES key and IV for encrypted payloads
             *  - Client resolution, framerate, and encoding
             *
             *  The only solution is to kill the pipeline and re-create it again
             * when a resume happens
             */

            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    // Guard against duplicate switch events (same as pause guard fix)
    auto last_audio_switch = std::make_shared<std::string>("");

    auto switch_producer_handler = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
        [session_id, pipeline, last_audio_switch](const immer::box<events::SwitchStreamProducerEvents> &switch_ev) {
          if (switch_ev->session_id == session_id) {
            // Guard against duplicate switch events to same destination
            if (*last_audio_switch == switch_ev->interpipe_src_id) {
              logs::log(logs::warning, "[HANG_DEBUG] Audio SwitchStreamProducerEvents DUPLICATE IGNORED: session {} already switched to {}", session_id, switch_ev->interpipe_src_id);
              return;
            }

            // Update last_audio_switch IMMEDIATELY to prevent race conditions
            *last_audio_switch = switch_ev->interpipe_src_id;

            logs::log(logs::debug,
                      "[GSTREAMER] Switching audio producer for {} to {}",
                      session_id,
                      switch_ev->interpipe_src_id);

            /* DEADLOCK FIX: Post message to pipeline bus instead of calling g_object_set directly
             * Same fix as video pipeline - prevents global GLib type lock deadlock
             */
            auto audio_interpipe = fmt::format("{}_audio", switch_ev->interpipe_src_id);
            logs::log(logs::warning, "[HANG_DEBUG] Posting switch-interpipe-src-audio message to pipeline bus: {}", audio_interpipe);

            gst_element_post_message(pipeline.get(),
              gst_message_new_application(GST_OBJECT(pipeline.get()),
                gst_structure_new("switch-interpipe-src-audio",
                  "session-id", G_TYPE_UINT64, (guint64)session_id,
                  "interpipe-id", G_TYPE_STRING, audio_interpipe.c_str(),
                  nullptr)));
          }
        });

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {} (quitting main loop)", session_id);
            g_main_loop_quit(loop.get());  // Thread-safe, avoids abandoned GStreamer mutexes
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(pause_handler),
                                                              std::move(switch_producer_handler),
                                                              std::move(stop_handler)};
  });
}

} // namespace streaming
