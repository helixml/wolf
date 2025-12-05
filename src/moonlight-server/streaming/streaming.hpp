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

/**
 * Start a test pattern producer pipeline for apps that don't use waylanddisplaysrc.
 * Creates: {source_pipeline} ! [gpu_upload] ! interpipesink name={session_id}_video
 * This allows lobby switching to work for placeholder apps (e.g., Blank app with videotestsrc).
 *
 * GPU upload is added based on buffer_caps to EXACTLY match waylanddisplaysrc output format,
 * avoiding any format conversion when interpipesrc switches between test pattern and lobby:
 * - NVIDIA (CUDAMemory): cudaupload ! video/x-raw(memory:CUDAMemory), format=NV12
 * - AMD/Intel (DMABuf): vapostproc ! video/x-raw(memory:DMABuf), drm-format=NV12
 * - Fallback: no GPU upload (CPU memory) - lobby switching may cause black screen
 */
void start_test_pattern_producer(const std::string &session_id,
                                 const std::string &source_pipeline,
                                 const std::string &buffer_caps,
                                 const wolf::core::virtual_display::DisplayMode &display_mode,
                                 std::shared_ptr<events::EventBusType> event_bus);

/**
 * Start a test audio producer pipeline for apps that don't use PulseAudio.
 * Creates: {source_pipeline} ! audio/x-raw,... ! interpipesink name={session_id}_audio
 * This allows lobby switching to work for placeholder apps (e.g., Blank app with audiotestsrc).
 */
void start_test_audio_producer(const std::string &session_id,
                               const std::string &source_pipeline,
                               int channel_count,
                               std::shared_ptr<events::EventBusType> event_bus);

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

  /* Thread lifecycle logging and monitoring */
  pid_t tid = syscall(SYS_gettid);
  std::string pipeline_short = pipeline_desc.substr(0, 80);
  logs::log(logs::info, "[THREAD_LIFECYCLE] Pipeline thread started: TID={} pipeline={}...", tid, pipeline_short);

  // Register thread for heartbeat monitoring
  wolf::monitoring::ScopedThreadMonitor thread_monitor("GStreamer-Pipeline", pipeline_short);

  /*
   * adds a watch for new message on our pipeline's message bus to
   * the default GLib main context, which is the main context that our
   * GLib main loop is attached to below
   */
  auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
  gst_bus_add_signal_watch(bus);

  // Heartbeat handler - updates on every bus message (proves thread is alive and processing)
  // Allocate monitor pointer on heap to safely pass to GLib callback
  auto* monitor_ptr = new wolf::monitoring::ScopedThreadMonitor*(&thread_monitor);
  g_signal_connect_data(bus, "message", G_CALLBACK(+[](GstBus*, GstMessage*, gpointer user_data) {
    auto** monitor = static_cast<wolf::monitoring::ScopedThreadMonitor**>(user_data);
    (*monitor)->heartbeat();
  }), monitor_ptr, [](gpointer data, GClosure*) {
    // Cleanup: delete the pointer when signal is disconnected
    delete static_cast<wolf::monitoring::ScopedThreadMonitor**>(data);
  }, GConnectFlags(0));

  g_signal_connect(bus, "message::error", G_CALLBACK(gstreamer::pipeline_error_handler), loop.get());
  g_signal_connect(bus, "message::eos", G_CALLBACK(gstreamer::pipeline_eos_handler), loop.get());
  gst_object_unref(bus);

  // Add buffer probe to detect buffer flow (proves pipeline is processing data)
  // Probe all source pads in the pipeline to catch buffer flow
  // IMPORTANT: Probes execute in GStreamer streaming threads, so we pass the pipeline TID explicitly
  GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipeline.get()));
  GValue item = G_VALUE_INIT;
  int probe_count = 0;

  while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
    GstElement* element = GST_ELEMENT(g_value_get_object(&item));

    // Probe the src pad if it exists
    GstPad* src_pad = gst_element_get_static_pad(element, "src");
    if (src_pad) {
      gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo*, gpointer user_data) -> GstPadProbeReturn {
          // Heartbeat the registered pipeline thread (not the probe's thread!)
          pid_t pipeline_tid = *static_cast<pid_t*>(user_data);
          wolf::monitoring::ThreadMonitor::get().heartbeat_for_tid(pipeline_tid);
          return GST_PAD_PROBE_OK;
        }, new pid_t(tid), [](gpointer data) {
          // Cleanup when probe is removed
          delete static_cast<pid_t*>(data);
        });
      gst_object_unref(src_pad);
      probe_count++;
    }
    g_value_reset(&item);
  }
  g_value_unset(&item);
  gst_iterator_free(it);

  logs::log(logs::debug, "[THREAD_LIFECYCLE] Added {} buffer probes to pipeline (TID={})", probe_count, tid);

  /* Set the pipeline to "playing" state*/
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(reinterpret_cast<GstBin *>(pipeline.get()),
                                    GST_DEBUG_GRAPH_SHOW_ALL,
                                    "pipeline-start");

  // Note: Heartbeat is now updated by the watchdog thread's global scan
  // We don't use bus handlers as they can interfere with GStreamer pipeline startup

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