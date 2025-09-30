#pragma once

#include <boost/asio.hpp>
#include <core/gstreamer.hpp>
#include <events/events.hpp>
#include <gst/gst.h>
#include <gstreamer-1.0/gst/app/gstappsink.h>
#include <memory>
#include <mutex>
#include <streaming/streaming.hpp>
#include <vector>

namespace streaming {

/**
 * Screenshot manager for streaming sessions
 * Captures periodic screenshots from the interpipe video source
 */
class ScreenshotManager {
public:
  struct ScreenshotData {
    std::vector<uint8_t> png_data;
    std::chrono::steady_clock::time_point timestamp;
    std::size_t session_id;
    int width;
    int height;
  };

  /**
   * Start screenshot pipeline for a session
   * Captures 1 frame per second from the interpipe and stores as PNG
   */
  static void start_screenshot_pipeline(std::size_t session_id,
                                         const std::shared_ptr<events::EventBusType> &event_bus);

  /**
   * Get the latest screenshot for a session
   * Returns BBC test card image if no screenshot available
   */
  static std::vector<uint8_t> get_latest_screenshot(std::size_t session_id);

  /**
   * Check if a screenshot is available for a session
   */
  static bool has_screenshot(std::size_t session_id);

  /**
   * Store screenshot data (public so it can be called from GStreamer callbacks)
   */
  static void store_screenshot(std::size_t session_id, std::vector<uint8_t> png_data, int width, int height);

private:
  static std::mutex screenshot_mutex_;
  static std::map<std::size_t, ScreenshotData> screenshots_;

  /**
   * Generate BBC test card image (640x480 PNG)
   */
  static std::vector<uint8_t> generate_test_card();
};

} // namespace streaming