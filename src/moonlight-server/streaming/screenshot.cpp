#include <gstreamer-1.0/gst/app/gstappsink.h>
#include <helpers/logger.hpp>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <memory>
#include <streaming/screenshot.hpp>
#include <streaming/streaming.hpp>

namespace streaming {

using namespace wolf::core::gstreamer;
using namespace wolf::core;

// Static member initialization
std::mutex ScreenshotManager::screenshot_mutex_;
std::map<std::size_t, ScreenshotManager::ScreenshotData> ScreenshotManager::screenshots_;

/**
 * GStreamer appsink callback to capture PNG-encoded screenshots
 */
struct ScreenshotSink {
  std::size_t session_id;
};

static GstFlowReturn on_screenshot_sample(GstAppSink *appsink, gpointer user_data) {
  ScreenshotSink *sink_data = static_cast<ScreenshotSink *>(user_data);

  std::shared_ptr<GstSample> sample(gst_app_sink_pull_sample(appsink), gst_sample_unref);
  if (!sample) {
    logs::log(logs::warning, "[SCREENSHOT] Failed to pull sample for session {}", sink_data->session_id);
    return GST_FLOW_ERROR;
  }

  GstBuffer *buffer = gst_sample_get_buffer(sample.get());
  if (!buffer) {
    logs::log(logs::warning, "[SCREENSHOT] Failed to get buffer from sample for session {}", sink_data->session_id);
    return GST_FLOW_ERROR;
  }

  // Get caps to extract width/height
  GstCaps *caps = gst_sample_get_caps(sample.get());
  int width = 640, height = 480; // defaults
  if (caps) {
    GstStructure *structure = gst_caps_get_structure(caps, 0);
    if (structure) {
      gst_structure_get_int(structure, "width", &width);
      gst_structure_get_int(structure, "height", &height);
    }
  }

  // Map buffer to read PNG data
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    // Copy PNG data
    std::vector<uint8_t> png_data(map.data, map.data + map.size);
    gst_buffer_unmap(buffer, &map);

    // Store screenshot
    ScreenshotManager::store_screenshot(sink_data->session_id, std::move(png_data), width, height);

    logs::log(logs::debug, "[SCREENSHOT] Captured screenshot for session {} ({}x{}, {} bytes)",
              sink_data->session_id, width, height, png_data.size());

    return GST_FLOW_OK;
  } else {
    logs::log(logs::error, "[SCREENSHOT] Failed to map buffer for session {}", sink_data->session_id);
    return GST_FLOW_ERROR;
  }
}

static void configure_screenshot_appsink(GstElement *appsink, ScreenshotSink *sink_data) {
  g_object_set(appsink, "emit-signals", FALSE, NULL);
  g_object_set(appsink, "max-buffers", 1, NULL);   // Keep only latest
  g_object_set(appsink, "drop", TRUE, NULL);       // Drop old frames

  GstAppSinkCallbacks callbacks = {nullptr};
  callbacks.new_sample = on_screenshot_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, sink_data, nullptr);
}

void ScreenshotManager::start_screenshot_pipeline(std::size_t session_id,
                                                   const std::shared_ptr<events::EventBusType> &event_bus) {
  // Screenshot pipeline: Convert DMA-BUF first, then process
  // videoconvert handles DMA-BUF → system memory automatically
  auto pipeline = fmt::format(
      "interpipesrc listen-to={session_id}_video is-live=true stream-sync=restart-ts "
      "max-bytes=0 max-buffers=1 leaky-type=downstream accept-events=true accept-eos-event=true ! "
      "videoconvert ! "                                         // Convert DMA-BUF to system memory first
      "videorate drop-only=true ! "                             // Throttle to 1 FPS in system memory
      "video/x-raw,framerate=1/1 ! "                            // Only 1 frame per second
      "videoscale ! "                                           // Scale down to reduce size
      "video/x-raw,width=640,height=480 ! "                     // Fixed size screenshots
      "videoconvert ! "                                         // Final format conversion for PNG
      "pngenc compression-level=6 ! "                           // Encode as PNG
      "appsink name=screenshot_sink sync=false max-buffers=1 drop=true", // Capture PNG data
      fmt::arg("session_id", session_id));

  logs::log(logs::info, "[SCREENSHOT] Starting screenshot pipeline for session {}", session_id);
  logs::log(logs::debug, "[SCREENSHOT] Pipeline: {}", pipeline);

  auto sink_data_ptr = std::make_shared<ScreenshotSink>(ScreenshotSink{.session_id = session_id});

  run_pipeline(pipeline, [=](auto pipeline_el, auto loop) {
    logs::log(logs::debug, "[SCREENSHOT] Configuring screenshot sink for session {}", session_id);

    if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline_el.get()), "screenshot_sink")) {
      g_assert(GST_IS_APP_SINK(app_sink_el));
      configure_screenshot_appsink(app_sink_el, sink_data_ptr.get());
      gst_object_unref(app_sink_el);
      logs::log(logs::info, "[SCREENSHOT] Screenshot sink configured for session {}", session_id);
    } else {
      logs::log(logs::error, "[SCREENSHOT] Failed to find screenshot_sink element for session {}", session_id);
    }

    // Stop screenshot pipeline when stream stops
    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::info, "[SCREENSHOT] Stopping screenshot pipeline for session {} due to StopStreamEvent", session_id);
            g_main_loop_quit(loop.get());

            // Clean up screenshot data
            std::lock_guard<std::mutex> lock(screenshot_mutex_);
            screenshots_.erase(session_id);
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler)};
  });

  logs::log(logs::info, "[SCREENSHOT] Screenshot pipeline stopped for session {}", session_id);
}

std::vector<uint8_t> ScreenshotManager::get_latest_screenshot(std::size_t session_id) {
  std::lock_guard<std::mutex> lock(screenshot_mutex_);

  // Debug: Log all available sessions
  logs::log(logs::debug, "[SCREENSHOT] Total sessions with screenshots: {}", screenshots_.size());
  for (const auto &[sid, data] : screenshots_) {
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - data.timestamp).count();
    logs::log(logs::debug,
              "[SCREENSHOT] Available session {} ({}x{}, {} bytes, age: {}s)",
              sid,
              data.width,
              data.height,
              data.png_data.size(),
              age);
  }

  auto it = screenshots_.find(session_id);
  if (it != screenshots_.end()) {
    // Check if screenshot is recent (within last 10 seconds)
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();

    if (age < 10) {
      logs::log(logs::debug, "[SCREENSHOT] Returning cached screenshot for session {} (age: {}s)", session_id, age);
      return it->second.png_data;
    } else {
      logs::log(logs::debug,
                "[SCREENSHOT] Cached screenshot too old for session {} (age: {}s), returning test card",
                session_id,
                age);
    }
  } else {
    logs::log(logs::debug, "[SCREENSHOT] No screenshot available for session {}, returning test card", session_id);
  }

  // Return test card if no screenshot or too old
  return generate_test_card();
}

bool ScreenshotManager::has_screenshot(std::size_t session_id) {
  std::lock_guard<std::mutex> lock(screenshot_mutex_);

  auto it = screenshots_.find(session_id);
  if (it == screenshots_.end()) {
    return false;
  }

  // Check if screenshot is recent (within last 10 seconds)
  auto now = std::chrono::steady_clock::now();
  auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();
  return age < 10;
}

void ScreenshotManager::store_screenshot(std::size_t session_id, std::vector<uint8_t> png_data, int width, int height) {
  std::lock_guard<std::mutex> lock(screenshot_mutex_);

  screenshots_[session_id] = ScreenshotData{
      .png_data = std::move(png_data),
      .timestamp = std::chrono::steady_clock::now(),
      .session_id = session_id,
      .width = width,
      .height = height,
  };

  logs::log(logs::trace, "[SCREENSHOT] Stored screenshot for session {} ({}x{})", session_id, width, height);
}

std::vector<uint8_t> ScreenshotManager::generate_test_card() {
  // Generate a simple BBC-style test card using GStreamer
  // This is a minimal PNG test pattern
  GError *error = nullptr;

  std::string pipeline_str = "videotestsrc pattern=smpte num-buffers=1 ! "
                             "video/x-raw,format=RGB,width=640,height=480 ! "
                             "pngenc ! "
                             "appsink name=test_card_sink emit-signals=false";

  gstreamer::gst_element_ptr pipeline(gst_parse_launch(pipeline_str.c_str(), &error), [](GstElement *pipeline) {
    if (pipeline) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
    }
  });

  if (!pipeline || error) {
    logs::log(logs::error, "[SCREENSHOT] Failed to create test card pipeline: {}",
              error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
    return {}; // Return empty vector on error
  }

  // Get the appsink
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline.get()), "test_card_sink");
  if (!sink) {
    logs::log(logs::error, "[SCREENSHOT] Failed to find test_card_sink");
    return {};
  }

  // Set pipeline to playing
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);

  // Wait for the EOS or sample
  GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
  gst_object_unref(sink);

  if (!sample) {
    logs::log(logs::error, "[SCREENSHOT] Failed to pull test card sample");
    return {};
  }

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return {};
  }

  // Map and copy the PNG data
  GstMapInfo map;
  std::vector<uint8_t> png_data;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    png_data = std::vector<uint8_t>(map.data, map.data + map.size);
    gst_buffer_unmap(buffer, &map);
  }

  gst_sample_unref(sample);

  logs::log(logs::debug, "[SCREENSHOT] Generated test card ({} bytes)", png_data.size());
  return png_data;
}

} // namespace streaming