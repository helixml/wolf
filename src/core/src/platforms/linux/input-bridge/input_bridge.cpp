#include "input_bridge.hpp"
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <linux/input-event-codes.h>
#include <map>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Debug counter for move_abs logging
static int move_abs_log_count = 0;

// Mapping from Windows Virtual Key codes (used by Moonlight) to Linux evdev keycodes
// This is the same mapping used by WaylandKeyboard in gst-wayland-display.cpp
static const std::map<unsigned int, unsigned int> vk_to_evdev = {
    {0x08, KEY_BACKSPACE},  {0x09, KEY_TAB},
    {0x0C, KEY_CLEAR},      {0x0D, KEY_ENTER},
    {0x10, KEY_LEFTSHIFT},  {0x11, KEY_LEFTCTRL},
    {0x12, KEY_LEFTALT},    {0x13, KEY_PAUSE},
    {0x14, KEY_CAPSLOCK},   {0x15, KEY_KATAKANAHIRAGANA},
    {0x16, KEY_HANGEUL},    {0x17, KEY_HANJA},
    {0x19, KEY_KATAKANA},   {0x1B, KEY_ESC},
    {0x20, KEY_SPACE},      {0x21, KEY_PAGEUP},
    {0x22, KEY_PAGEDOWN},   {0x23, KEY_END},
    {0x24, KEY_HOME},       {0x25, KEY_LEFT},
    {0x26, KEY_UP},         {0x27, KEY_RIGHT},
    {0x28, KEY_DOWN},       {0x29, KEY_SELECT},
    {0x2A, KEY_PRINT},      {0x2C, KEY_SYSRQ},
    {0x2D, KEY_INSERT},     {0x2E, KEY_DELETE},
    {0x2F, KEY_HELP},       {0x30, KEY_0},
    {0x31, KEY_1},          {0x32, KEY_2},
    {0x33, KEY_3},          {0x34, KEY_4},
    {0x35, KEY_5},          {0x36, KEY_6},
    {0x37, KEY_7},          {0x38, KEY_8},
    {0x39, KEY_9},          {0x41, KEY_A},
    {0x42, KEY_B},          {0x43, KEY_C},
    {0x44, KEY_D},          {0x45, KEY_E},
    {0x46, KEY_F},          {0x47, KEY_G},
    {0x48, KEY_H},          {0x49, KEY_I},
    {0x4A, KEY_J},          {0x4B, KEY_K},
    {0x4C, KEY_L},          {0x4D, KEY_M},
    {0x4E, KEY_N},          {0x4F, KEY_O},
    {0x50, KEY_P},          {0x51, KEY_Q},
    {0x52, KEY_R},          {0x53, KEY_S},
    {0x54, KEY_T},          {0x55, KEY_U},
    {0x56, KEY_V},          {0x57, KEY_W},
    {0x58, KEY_X},          {0x59, KEY_Y},
    {0x5A, KEY_Z},          {0x5B, KEY_LEFTMETA},
    {0x5C, KEY_RIGHTMETA},  {0x5F, KEY_SLEEP},
    {0x60, KEY_KP0},        {0x61, KEY_KP1},
    {0x62, KEY_KP2},        {0x63, KEY_KP3},
    {0x64, KEY_KP4},        {0x65, KEY_KP5},
    {0x66, KEY_KP6},        {0x67, KEY_KP7},
    {0x68, KEY_KP8},        {0x69, KEY_KP9},
    {0x6A, KEY_KPASTERISK}, {0x6B, KEY_KPPLUS},
    {0x6C, KEY_KPCOMMA},    {0x6D, KEY_KPMINUS},
    {0x6E, KEY_KPDOT},      {0x6F, KEY_KPSLASH},
    {0x70, KEY_F1},         {0x71, KEY_F2},
    {0x72, KEY_F3},         {0x73, KEY_F4},
    {0x74, KEY_F5},         {0x75, KEY_F6},
    {0x76, KEY_F7},         {0x77, KEY_F8},
    {0x78, KEY_F9},         {0x79, KEY_F10},
    {0x7A, KEY_F11},        {0x7B, KEY_F12},
    {0x90, KEY_NUMLOCK},    {0x91, KEY_SCROLLLOCK},
    {0xA0, KEY_LEFTSHIFT},  {0xA1, KEY_RIGHTSHIFT},
    {0xA2, KEY_LEFTCTRL},   {0xA3, KEY_RIGHTCTRL},
    {0xA4, KEY_LEFTALT},    {0xA5, KEY_RIGHTALT},
    {0xBA, KEY_SEMICOLON},  {0xBB, KEY_EQUAL},
    {0xBC, KEY_COMMA},      {0xBD, KEY_MINUS},
    {0xBE, KEY_DOT},        {0xBF, KEY_SLASH},
    {0xC0, KEY_GRAVE},      {0xDB, KEY_LEFTBRACE},
    {0xDC, KEY_BACKSLASH},  {0xDD, KEY_RIGHTBRACE},
    {0xDE, KEY_APOSTROPHE}, {0xE2, KEY_102ND},
};

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
  // Debug logging: log first 5 and then every 100th
  move_abs_log_count++;
  if (move_abs_log_count <= 5 || move_abs_log_count % 100 == 0) {
    fprintf(stderr, "[INPUT_BRIDGE] move_abs #%d: x=%.1f y=%.1f screen=%dx%d\n",
            move_abs_log_count, x, y, screen_width, screen_height);
    fflush(stderr);
  }
  // The input bridge expects absolute coordinates in screen pixels
  send(fmt::format(R"({{"type":"mouse_move_abs","x":{},"y":{}}})", x, y));
}

// Convert Moonlight button codes to evdev button codes for Mutter's D-Bus API
// Moonlight: 1=left, 2=middle, 3=right, 4=side, 5+=extra
// Evdev: 272=BTN_LEFT, 273=BTN_RIGHT, 274=BTN_MIDDLE, 275=BTN_SIDE, 276=BTN_EXTRA
static int moonlight_button_to_evdev(int button) {
  switch (button) {
  case 1:
    return 272; // BTN_LEFT
  case 2:
    return 274; // BTN_MIDDLE
  case 3:
    return 273; // BTN_RIGHT
  case 4:
    return 275; // BTN_SIDE
  default:
    return 276 + (button - 5); // BTN_EXTRA and beyond
  }
}

void InputBridge::press(int button) {
  int evdev_button = moonlight_button_to_evdev(button);
  fprintf(stderr, "[INPUT_BRIDGE] press: moonlight_button=%d -> evdev_button=%d\n", button, evdev_button);
  fflush(stderr);
  send(fmt::format(R"({{"type":"button","button":{},"state":true}})", evdev_button));
}

void InputBridge::release(int button) {
  int evdev_button = moonlight_button_to_evdev(button);
  fprintf(stderr, "[INPUT_BRIDGE] release: moonlight_button=%d -> evdev_button=%d\n", button, evdev_button);
  fflush(stderr);
  send(fmt::format(R"({{"type":"button","button":{},"state":false}})", evdev_button));
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
  // Convert from Windows Virtual Key code (Moonlight) to Linux evdev keycode
  // Moonlight sends Windows VK codes (e.g., 0x41='A'), but Mutter's D-Bus API expects evdev keycodes
  auto it = vk_to_evdev.find(static_cast<unsigned int>(keycode));
  if (it != vk_to_evdev.end()) {
    send(fmt::format(R"({{"type":"key","keycode":{},"state":true}})", it->second));
  } else {
    fprintf(stderr, "[INPUT_BRIDGE] Unknown key code: 0x%02X (%d)\n", keycode, keycode);
    fflush(stderr);
  }
}

void InputBridge::key_release(int keycode) {
  // Convert from Windows Virtual Key code (Moonlight) to Linux evdev keycode
  auto it = vk_to_evdev.find(static_cast<unsigned int>(keycode));
  if (it != vk_to_evdev.end()) {
    send(fmt::format(R"({{"type":"key","keycode":{},"state":false}})", it->second));
  } else {
    fprintf(stderr, "[INPUT_BRIDGE] Unknown key code: 0x%02X (%d)\n", keycode, keycode);
    fflush(stderr);
  }
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
