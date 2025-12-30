#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace wolf::core::input {

/**
 * Input bridge client for RemoteDesktop mode.
 *
 * Connects to a Unix socket inside the container and sends input events
 * using a simple JSON protocol. The container runs an input bridge daemon
 * that forwards these events to the desktop environment via D-Bus.
 *
 * JSON protocol:
 *   {"type": "mouse_move_abs", "x": 100, "y": 200}
 *   {"type": "mouse_move_rel", "dx": 10, "dy": -5}
 *   {"type": "button", "button": 1, "state": true}
 *   {"type": "scroll", "dx": 0, "dy": -1}
 *   {"type": "key", "keycode": 36, "state": true}
 */
class InputBridge {
public:
  InputBridge() = default;
  ~InputBridge();

  // Non-copyable
  InputBridge(const InputBridge &) = delete;
  InputBridge &operator=(const InputBridge &) = delete;

  // Movable
  InputBridge(InputBridge &&other) noexcept;
  InputBridge &operator=(InputBridge &&other) noexcept;

  /**
   * Connect to the input bridge socket.
   * @param socket_path Path to the Unix socket inside the container
   * @return true if connected successfully
   */
  bool connect(const std::string &socket_path);

  /**
   * Disconnect from the input bridge.
   */
  void disconnect();

  /**
   * Check if connected.
   */
  bool is_connected() const { return socket_fd_ >= 0; }

  // Mouse methods
  void move(float dx, float dy);
  void move_abs(float x, float y, int screen_width, int screen_height);
  void press(int button);
  void release(int button);
  void vertical_scroll(int amount);
  void horizontal_scroll(int amount);

  // Keyboard methods
  void key_press(int keycode);
  void key_release(int keycode);

  // Touch methods
  void touch_down(int slot, float x, float y, int screen_width, int screen_height);
  void touch_motion(int slot, float x, float y, int screen_width, int screen_height);
  void touch_up(int slot);

private:
  void send(const std::string &json);

  int socket_fd_ = -1;
  std::string socket_path_;
  std::mutex send_mutex_;
};

/**
 * Mouse wrapper for InputBridge - provides the same interface as input::Mouse
 */
class InputBridgeMouse {
public:
  explicit InputBridgeMouse(std::shared_ptr<InputBridge> bridge);

  void move(float dx, float dy);
  void move_abs(float x, float y, int screen_width, int screen_height);
  void press(int button);
  void release(int button);
  void vertical_scroll(int amount);
  void horizontal_scroll(int amount);

private:
  std::shared_ptr<InputBridge> bridge_;
};

/**
 * Keyboard wrapper for InputBridge - provides the same interface as input::Keyboard
 */
class InputBridgeKeyboard {
public:
  explicit InputBridgeKeyboard(std::shared_ptr<InputBridge> bridge);

  void press(short keycode);
  void release(short keycode);

private:
  std::shared_ptr<InputBridge> bridge_;
};

/**
 * TouchScreen wrapper for InputBridge - provides the same interface as input::TouchScreen
 *
 * Tracks active touch slots to properly distinguish between touch_down (first contact)
 * and touch_motion (subsequent moves) as required by Mutter's RemoteDesktop D-Bus API.
 */
class InputBridgeTouchScreen {
public:
  explicit InputBridgeTouchScreen(std::shared_ptr<InputBridge> bridge,
                                   int screen_width, int screen_height);

  void place_finger(int slot, float x, float y);
  void release_finger(int slot);

private:
  std::shared_ptr<InputBridge> bridge_;
  int screen_width_;
  int screen_height_;
  std::array<bool, 10> active_slots_{}; // Track which slots have touch_down sent
};

} // namespace wolf::core::input
