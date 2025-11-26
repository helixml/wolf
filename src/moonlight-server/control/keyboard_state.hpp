#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wolf::control {

/**
 * Global keyboard state tracker for observability.
 * Tracks which keys Wolf believes are currently pressed per session.
 * This is independent of inputtino's internal state and provides
 * visibility into keyboard events as they flow through Wolf.
 */
class KeyboardStateTracker {
public:
  struct SessionKeyState {
    std::size_t session_id;
    std::set<short> pressed_keys;         // Moonlight key codes (Windows VK codes)
    int64_t last_update_ms;
    std::string device_name;
  };

  static KeyboardStateTracker& get() {
    static KeyboardStateTracker instance;
    return instance;
  }

  void key_press(std::size_t session_id, short key_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = sessions_[session_id];
    state.session_id = session_id;
    state.pressed_keys.insert(key_code);
    state.last_update_ms = now_ms();
  }

  void key_release(std::size_t session_id, short key_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
      it->second.pressed_keys.erase(key_code);
      it->second.last_update_ms = now_ms();
    }
  }

  void release_all(std::size_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
      it->second.pressed_keys.clear();
      it->second.last_update_ms = now_ms();
    }
  }

  void remove_session(std::size_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
  }

  std::vector<SessionKeyState> get_all_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionKeyState> result;
    for (const auto& [id, state] : sessions_) {
      result.push_back(state);
    }
    return result;
  }

  std::optional<SessionKeyState> get_session(std::size_t session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  // Returns pressed keys for a session and releases them
  std::set<short> reset_session(std::size_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
      auto keys = it->second.pressed_keys;
      it->second.pressed_keys.clear();
      it->second.last_update_ms = now_ms();
      return keys;
    }
    return {};
  }

private:
  KeyboardStateTracker() = default;

  int64_t now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }

  mutable std::mutex mutex_;
  std::map<std::size_t, SessionKeyState> sessions_;
};

// Helper to get human-readable key name from Moonlight key code (Windows VK code)
inline std::string moonlight_key_to_name(short key_code) {
  // Common Windows Virtual Key codes
  static const std::map<short, std::string> key_names = {
    {0x08, "Backspace"}, {0x09, "Tab"}, {0x0D, "Enter"}, {0x10, "Shift"},
    {0x11, "Ctrl"}, {0x12, "Alt"}, {0x13, "Pause"}, {0x14, "CapsLock"},
    {0x1B, "Escape"}, {0x20, "Space"}, {0x21, "PageUp"}, {0x22, "PageDown"},
    {0x23, "End"}, {0x24, "Home"}, {0x25, "Left"}, {0x26, "Up"},
    {0x27, "Right"}, {0x28, "Down"}, {0x2D, "Insert"}, {0x2E, "Delete"},
    {0x30, "0"}, {0x31, "1"}, {0x32, "2"}, {0x33, "3"}, {0x34, "4"},
    {0x35, "5"}, {0x36, "6"}, {0x37, "7"}, {0x38, "8"}, {0x39, "9"},
    {0x41, "A"}, {0x42, "B"}, {0x43, "C"}, {0x44, "D"}, {0x45, "E"},
    {0x46, "F"}, {0x47, "G"}, {0x48, "H"}, {0x49, "I"}, {0x4A, "J"},
    {0x4B, "K"}, {0x4C, "L"}, {0x4D, "M"}, {0x4E, "N"}, {0x4F, "O"},
    {0x50, "P"}, {0x51, "Q"}, {0x52, "R"}, {0x53, "S"}, {0x54, "T"},
    {0x55, "U"}, {0x56, "V"}, {0x57, "W"}, {0x58, "X"}, {0x59, "Y"},
    {0x5A, "Z"}, {0x5B, "LWin"}, {0x5C, "RWin"},
    {0x60, "Numpad0"}, {0x61, "Numpad1"}, {0x62, "Numpad2"}, {0x63, "Numpad3"},
    {0x64, "Numpad4"}, {0x65, "Numpad5"}, {0x66, "Numpad6"}, {0x67, "Numpad7"},
    {0x68, "Numpad8"}, {0x69, "Numpad9"},
    {0x6A, "Multiply"}, {0x6B, "Add"}, {0x6D, "Subtract"}, {0x6E, "Decimal"},
    {0x6F, "Divide"},
    {0x70, "F1"}, {0x71, "F2"}, {0x72, "F3"}, {0x73, "F4"}, {0x74, "F5"},
    {0x75, "F6"}, {0x76, "F7"}, {0x77, "F8"}, {0x78, "F9"}, {0x79, "F10"},
    {0x7A, "F11"}, {0x7B, "F12"},
    {0x90, "NumLock"}, {0x91, "ScrollLock"},
    {0xA0, "LShift"}, {0xA1, "RShift"}, {0xA2, "LCtrl"}, {0xA3, "RCtrl"},
    {0xA4, "LAlt"}, {0xA5, "RAlt"},
    {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."},
    {0xBF, "/"}, {0xC0, "`"}, {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"},
    {0xDE, "'"},
  };

  auto it = key_names.find(key_code);
  if (it != key_names.end()) {
    return it->second;
  }
  return "Key_0x" + std::to_string(key_code);
}

// Modifier key codes (Windows VK codes)
constexpr short VK_SHIFT = 0x10;
constexpr short VK_CTRL = 0x11;
constexpr short VK_ALT = 0x12;
constexpr short VK_LWIN = 0x5B;
constexpr short VK_RWIN = 0x5C;
constexpr short VK_LSHIFT = 0xA0;
constexpr short VK_RSHIFT = 0xA1;
constexpr short VK_LCTRL = 0xA2;
constexpr short VK_RCTRL = 0xA3;
constexpr short VK_LALT = 0xA4;
constexpr short VK_RALT = 0xA5;

inline bool is_shift_pressed(const std::set<short>& keys) {
  return keys.count(VK_SHIFT) || keys.count(VK_LSHIFT) || keys.count(VK_RSHIFT);
}

inline bool is_ctrl_pressed(const std::set<short>& keys) {
  return keys.count(VK_CTRL) || keys.count(VK_LCTRL) || keys.count(VK_RCTRL);
}

inline bool is_alt_pressed(const std::set<short>& keys) {
  return keys.count(VK_ALT) || keys.count(VK_LALT) || keys.count(VK_RALT);
}

inline bool is_meta_pressed(const std::set<short>& keys) {
  return keys.count(VK_LWIN) || keys.count(VK_RWIN);
}

} // namespace wolf::control
