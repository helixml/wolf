#pragma once
#include <boost/asio.hpp>
#include <core/gstreamer.hpp>
#include <core/virtual-display.hpp>
#include <events/events.hpp>
#include <fmt/format.h>
#include <gst-plugin/gstrtpmoonlightpay_audio.hpp>
#include <gst-plugin/gstrtpmoonlightpay_video.hpp>
#include <gst-plugin/video.hpp>
#include <gst/gst.h>
#include <gstreamer-1.0/gst/app/gstappsrc.h>
#include <immer/box.hpp>
#include <memory>
#include <monitoring/thread-monitor.hpp>
#include <moonlight/fec.hpp>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace streaming {

using namespace wolf::core;
using boost::asio::ip::udp;

struct WaylandDisplayReady {
  /**
   * The name of the wayland socket that our custom compositor is listening on
   */
  std::string wayland_socket_name;
  /**
   * The wayland plugin element,
   * we need a reference so that we can send events directly to it (mouse, keyboard, ...)
   */
  gstreamer::gst_element_ptr wayland_plugin;
};

void start_video_producer(const std::string &session_id,
                          const std::string &buffer_format,
                          const std::string &render_node,
                          const wolf::core::virtual_display::DisplayMode &display_mode,
                          std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                          std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready,
                          std::shared_ptr<events::EventBusType> event_bus);

void start_audio_producer(const std::string &session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name);

void start_streaming_video(immer::box<events::VideoSession> video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                           std::shared_ptr<udp::socket> video_socket);

void start_streaming_audio(immer::box<events::AudioSession> audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> audio_socket,
                           const std::string &sink_name,
                           const std::string &server_name);

static bool run_pipeline(
    const std::string &pipeline_desc,
    const std::function<immer::array<immer::box<events::EventBusHandlers>>(gstreamer::gst_element_ptr /* pipeline */, gstreamer::gst_main_loop_ptr /* loop */)>
        &on_pipeline_ready) {
  GError *error = nullptr;
  gstreamer::gst_element_ptr pipeline(gst_parse_launch(pipeline_desc.c_str(), &error), [](const auto &pipeline) {
    logs::log(logs::trace, "~pipeline");
    gst_object_unref(pipeline);
  });

  if (!pipeline) {
    logs::log(logs::error, "[GSTREAMER] Pipeline parse error: {}", error->message);
    g_error_free(error);
    return false;
  } else if (error) { // Please note that you might get a return value that is not NULL even though the error is set. In
                      // this case there was a recoverable parsing error and you can try to play the pipeline.
    logs::log(logs::warning, "[GSTREAMER] Pipeline parse error (recovered): {}", error->message);
    g_error_free(error);
  }

  gstreamer::gst_main_context_ptr context = {g_main_context_new(), ::g_main_context_unref};
  g_main_context_push_thread_default(context.get());
  gstreamer::gst_main_loop_ptr loop(g_main_loop_new(context.get(), FALSE), ::g_main_loop_unref);

  /* Let the calling thread set extra things */
  auto handlers = on_pipeline_ready(pipeline, loop);

  /*
   * adds a watch for new message on our pipeline's message bus to
   * the default GLib main context, which is the main context that our
   * GLib main loop is attached to below
   */
  auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message::error", G_CALLBACK(gstreamer::pipeline_error_handler), loop.get());
  g_signal_connect(bus, "message::eos", G_CALLBACK(gstreamer::pipeline_eos_handler), loop.get());
  gst_object_unref(bus);

  /* Set the pipeline to "playing" state*/
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(reinterpret_cast<GstBin *>(pipeline.get()),
                                    GST_DEBUG_GRAPH_SHOW_ALL,
                                    "pipeline-start");

  /* Thread lifecycle logging and monitoring */
  pid_t tid = syscall(SYS_gettid);
  std::string pipeline_short = pipeline_desc.substr(0, 80);
  logs::log(logs::info, "[THREAD_LIFECYCLE] Pipeline thread started: TID={} pipeline={}...", tid, pipeline_short);

  // Register thread for heartbeat monitoring
  wolf::monitoring::ScopedThreadMonitor thread_monitor("GStreamer-Pipeline", pipeline_short);

  // Add periodic heartbeat callback (every 100ms during pipeline execution)
  // This lets the watchdog detect stuck threads (>30s without heartbeat = deadlock)
  g_timeout_add(100, [](gpointer user_data) -> gboolean {
    auto* monitor = static_cast<wolf::monitoring::ScopedThreadMonitor*>(user_data);
    monitor->heartbeat();
    return G_SOURCE_CONTINUE;  // Keep calling this callback
  }, &thread_monitor);

  /* Thread cleanup handler - logs if thread exits unexpectedly */
  struct CleanupData {
    std::string pipeline_desc_short;
    pid_t tid;
  } cleanup_data = {pipeline_desc.substr(0, 100), tid};

  auto cleanup_handler = [](void* arg) {
    auto* data = (CleanupData*)arg;
    logs::log(logs::error, "[THREAD_EXIT] GStreamer pipeline thread (TID={}) exited UNEXPECTEDLY: {}...",
              data->tid, data->pipeline_desc_short);
    // Note: Cannot unlock mutexes held by GStreamer internal code (libgstbase, interpipe, etc.)
    // Thread 43209 died inside GStreamer library, not our code - we have no access to those mutexes
    // This cleanup handler is purely for logging/detection
  };

  pthread_cleanup_push(cleanup_handler, &cleanup_data);

  /* The main loop will be run until someone calls g_main_loop_quit() */
  // Note: Using GStreamer's standard g_main_loop_run - it handles all the internal threading
  g_main_loop_run(loop.get());

  pthread_cleanup_pop(0);  // Don't execute cleanup on normal exit

  /* Out of the main loop, clean up nicely */
  logs::log(logs::info, "[THREAD_LIFECYCLE] Pipeline thread (TID={}) exiting normally, cleaning up", tid);
  gst_element_set_state(pipeline.get(), GST_STATE_PAUSED);
  gst_element_set_state(pipeline.get(), GST_STATE_READY);
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);

  return true;
}

/**
 * @return the Gstreamer version we are linked to
 */
inline std::string get_gst_version() {
  guint major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  return fmt::format("{}.{}.{}-{}", major, minor, micro, nano);
}

/**
 * GStreamer needs to be initialised once per run
 * Call this method in your main.
 */
inline void init() {
  /* It is also possible to call the init function with two NULL arguments,
   * in which case no command line options will be parsed by GStreamer.
   */
  gst_init(nullptr, nullptr);
  logs::log(logs::info, "Gstreamer version: {}", get_gst_version());

  gst_element_register(nullptr, "rtpmoonlightpay_video", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_video);
  gst_element_register(nullptr, "rtpmoonlightpay_audio", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_audio);

  moonlight::fec::init();
}

} // namespace streaming