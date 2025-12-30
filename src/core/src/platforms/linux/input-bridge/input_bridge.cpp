#include "input_bridge.hpp"
#include <cstring>
#include <fmt/format.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace wolf::core::input {

InputBridge::~InputBridge() {
  disconnect();
}

InputBridge::InputBridge(InputBridge &&other) noexcept
    : socket_fd_(other.socket_fd_), socket_path_(std::move(other.socket_path_)) {
  other.socket_fd_ = -1;
}

InputBridge &InputBridge::operator=(InputBridge &&other) noexcept {
  if (this != &other) {
    disconnect();
    socket_fd_ = other.socket_fd_;
    socket_path_ = std::move(other.socket_path_);
    other.socket_fd_ = -1;
  }
  return *this;
}

bool InputBridge::connect(const std::string &socket_path) {
  disconnect();

  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(socket_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  socket_path_ = socket_path;
  return true;
}

void InputBridge::disconnect() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  socket_path_.clear();
}

void InputBridge::send(const std::string &json) {
  if (socket_fd_ < 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(send_mutex_);
  std::string msg = json + "\n";
  ::send(socket_fd_, msg.c_str(), msg.size(), MSG_NOSIGNAL);
}

// Mouse methods
void InputBridge::move(float dx, float dy) {
  send(fmt::format(R"({{"type":"mouse_move_rel","dx":{},"dy":{}}})", dx, dy));
}

void InputBridge::move_abs(float x, float y, int screen_width, int screen_height) {
  // The input bridge expects absolute coordinates in screen pixels
  send(fmt::format(R"({{"type":"mouse_move_abs","x":{},"y":{}}})", x, y));
}

void InputBridge::press(int button) {
  send(fmt::format(R"({{"type":"button","button":{},"state":true}})", button));
}

void InputBridge::release(int button) {
  send(fmt::format(R"({{"type":"button","button":{},"state":false}})", button));
}

void InputBridge::vertical_scroll(int amount) {
  // Mutter's NotifyPointerAxis uses 10.0 = one discrete scroll step
  //
  // The Helix frontend already scales scroll values to target ~10-15 per notch:
  //   - Mouse wheel: browser sends ~100-150px, frontend scales /10 → 10-15
  //   - Trackpad: browser sends ~4px per event, frontend scales /2 → 2
  //
  // So the incoming 'amount' is already in a scale close to Mutter's expectation.
  // We pass it through directly as a double for smooth scrolling.
  //
  // Negative = scroll up for both Moonlight and Mutter.
  double mutter_dy = static_cast<double>(amount);
  send(fmt::format(R"({{"type":"scroll_smooth","dx":0.0,"dy":{}}})", mutter_dy));
}

void InputBridge::horizontal_scroll(int amount) {
  double mutter_dx = static_cast<double>(amount);
  send(fmt::format(R"({{"type":"scroll_smooth","dx":{},"dy":0.0}})", mutter_dx));
}

// Keyboard methods
void InputBridge::key_press(int keycode) {
  // Convert from Windows virtual key to Linux evdev keycode
  // The input handler already does this conversion, so keycode should be evdev
  send(fmt::format(R"({{"type":"key","keycode":{},"state":true}})", keycode));
}

void InputBridge::key_release(int keycode) {
  send(fmt::format(R"({{"type":"key","keycode":{},"state":false}})", keycode));
}

// Touch methods
// Note: x and y are normalized 0..1 values, convert to screen coordinates for Mutter
void InputBridge::touch_down(int slot, float x, float y, int screen_width, int screen_height) {
  float abs_x = x * static_cast<float>(screen_width);
  float abs_y = y * static_cast<float>(screen_height);
  send(fmt::format(R"({{"type":"touch_down","slot":{},"x":{},"y":{}}})", slot, abs_x, abs_y));
}

void InputBridge::touch_motion(int slot, float x, float y, int screen_width, int screen_height) {
  float abs_x = x * static_cast<float>(screen_width);
  float abs_y = y * static_cast<float>(screen_height);
  send(fmt::format(R"({{"type":"touch_motion","slot":{},"x":{},"y":{}}})", slot, abs_x, abs_y));
}

void InputBridge::touch_up(int slot) {
  send(fmt::format(R"({{"type":"touch_up","slot":{}}})", slot));
}

// InputBridgeMouse implementation
InputBridgeMouse::InputBridgeMouse(std::shared_ptr<InputBridge> bridge) : bridge_(std::move(bridge)) {}

void InputBridgeMouse::move(float dx, float dy) {
  bridge_->move(dx, dy);
}

void InputBridgeMouse::move_abs(float x, float y, int screen_width, int screen_height) {
  bridge_->move_abs(x, y, screen_width, screen_height);
}

void InputBridgeMouse::press(int button) {
  bridge_->press(button);
}

void InputBridgeMouse::release(int button) {
  bridge_->release(button);
}

void InputBridgeMouse::vertical_scroll(int amount) {
  bridge_->vertical_scroll(amount);
}

void InputBridgeMouse::horizontal_scroll(int amount) {
  bridge_->horizontal_scroll(amount);
}

// InputBridgeKeyboard implementation
InputBridgeKeyboard::InputBridgeKeyboard(std::shared_ptr<InputBridge> bridge) : bridge_(std::move(bridge)) {}

void InputBridgeKeyboard::press(short keycode) {
  bridge_->key_press(keycode);
}

void InputBridgeKeyboard::release(short keycode) {
  bridge_->key_release(keycode);
}

// InputBridgeTouchScreen implementation
InputBridgeTouchScreen::InputBridgeTouchScreen(std::shared_ptr<InputBridge> bridge,
                                                 int screen_width, int screen_height)
    : bridge_(std::move(bridge)), screen_width_(screen_width), screen_height_(screen_height) {
  // Initialize all slots to inactive
  active_slots_.fill(false);
}

void InputBridgeTouchScreen::place_finger(int slot, float x, float y) {
  // Mutter's RemoteDesktop D-Bus API requires NotifyTouchDown before NotifyTouchMotion
  // Track which slots have been touched down to send the correct event type
  if (slot >= 0 && slot < static_cast<int>(active_slots_.size())) {
    if (!active_slots_[slot]) {
      // First touch for this slot - send touch_down
      active_slots_[slot] = true;
      bridge_->touch_down(slot, x, y, screen_width_, screen_height_);
    } else {
      // Slot already active - send touch_motion
      bridge_->touch_motion(slot, x, y, screen_width_, screen_height_);
    }
  } else {
    // Slot out of range, still try to send motion (best effort)
    bridge_->touch_motion(slot, x, y, screen_width_, screen_height_);
  }
}

void InputBridgeTouchScreen::release_finger(int slot) {
  if (slot >= 0 && slot < static_cast<int>(active_slots_.size())) {
    active_slots_[slot] = false;
  }
  bridge_->touch_up(slot);
}

} // namespace wolf::core::input
