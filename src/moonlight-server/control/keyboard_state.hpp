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

// Helper to get human-readable key name from Linux keycode (KEY_*)
// These are the codes used by evdev/kernel
inline std::string linux_key_to_name(int key_code) {
  static const std::map<int, std::string> key_names = {
    {1, "Escape"}, {2, "1"}, {3, "2"}, {4, "3"}, {5, "4"}, {6, "5"}, {7, "6"},
    {8, "7"}, {9, "8"}, {10, "9"}, {11, "0"}, {12, "-"}, {13, "="}, {14, "Backspace"},
    {15, "Tab"}, {16, "Q"}, {17, "W"}, {18, "E"}, {19, "R"}, {20, "T"}, {21, "Y"},
    {22, "U"}, {23, "I"}, {24, "O"}, {25, "P"}, {26, "["}, {27, "]"}, {28, "Enter"},
    {29, "LCtrl"}, {30, "A"}, {31, "S"}, {32, "D"}, {33, "F"}, {34, "G"}, {35, "H"},
    {36, "J"}, {37, "K"}, {38, "L"}, {39, ";"}, {40, "'"}, {41, "`"}, {42, "LShift"},
    {43, "\\"}, {44, "Z"}, {45, "X"}, {46, "C"}, {47, "V"}, {48, "B"}, {49, "N"},
    {50, "M"}, {51, ","}, {52, "."}, {53, "/"}, {54, "RShift"}, {55, "Numpad*"},
    {56, "LAlt"}, {57, "Space"}, {58, "CapsLock"},
    {59, "F1"}, {60, "F2"}, {61, "F3"}, {62, "F4"}, {63, "F5"}, {64, "F6"},
    {65, "F7"}, {66, "F8"}, {67, "F9"}, {68, "F10"},
    {69, "NumLock"}, {70, "ScrollLock"},
    {71, "Numpad7"}, {72, "Numpad8"}, {73, "Numpad9"}, {74, "Numpad-"},
    {75, "Numpad4"}, {76, "Numpad5"}, {77, "Numpad6"}, {78, "Numpad+"},
    {79, "Numpad1"}, {80, "Numpad2"}, {81, "Numpad3"},
    {82, "Numpad0"}, {83, "Numpad."},
    {87, "F11"}, {88, "F12"},
    {96, "NumpadEnter"}, {97, "RCtrl"}, {98, "Numpad/"}, {99, "SysRq"},
    {100, "RAlt"}, {102, "Home"}, {103, "Up"}, {104, "PageUp"},
    {105, "Left"}, {106, "Right"}, {107, "End"}, {108, "Down"},
    {109, "PageDown"}, {110, "Insert"}, {111, "Delete"},
    {125, "LMeta"}, {126, "RMeta"}, {127, "Menu"},
  };

  auto it = key_names.find(key_code);
  if (it != key_names.end()) {
    return it->second;
  }
  return "KEY_" + std::to_string(key_code);
}

// Check if modifier is pressed in a vector of Linux keycodes
inline bool is_shift_pressed_linux(const std::vector<int>& keys) {
  for (int key : keys) {
    if (key == 42 || key == 54) return true;  // KEY_LEFTSHIFT, KEY_RIGHTSHIFT
  }
  return false;
}

inline bool is_ctrl_pressed_linux(const std::vector<int>& keys) {
  for (int key : keys) {
    if (key == 29 || key == 97) return true;  // KEY_LEFTCTRL, KEY_RIGHTCTRL
  }
  return false;
}

inline bool is_alt_pressed_linux(const std::vector<int>& keys) {
  for (int key : keys) {
    if (key == 56 || key == 100) return true;  // KEY_LEFTALT, KEY_RIGHTALT
  }
  return false;
}

inline bool is_meta_pressed_linux(const std::vector<int>& keys) {
  for (int key : keys) {
    if (key == 125 || key == 126) return true;  // KEY_LEFTMETA, KEY_RIGHTMETA
  }
  return false;
}

} // namespace wolf::control
