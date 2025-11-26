#include "inputtino/input.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <inputtino/protected_types.hpp>
#include <inputtino/keyboard.hpp>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace inputtino {

using namespace std::string_literals;

std::vector<std::string> Keyboard::get_nodes() const {
  std::vector<std::string> nodes;

  if (auto kb = _state->kb.get()) {
    nodes.emplace_back(libevdev_uinput_get_devnode(kb));
  }

  return nodes;
}

Result<libevdev_uinput_ptr> create_keyboard(const DeviceDefinition &device) {
  auto dev = libevdev_new();
  libevdev_uinput *uidev;

  libevdev_set_name(dev, device.name.c_str());
  libevdev_set_id_vendor(dev, device.vendor_id);
  libevdev_set_id_product(dev, device.product_id);
  libevdev_set_id_version(dev, device.version);
  libevdev_set_id_bustype(dev, BUS_USB);

  libevdev_enable_event_type(dev, EV_KEY);
  libevdev_enable_event_code(dev, EV_KEY, KEY_BACKSPACE, nullptr);

  for (auto ev : keyboard::key_mappings) {
    libevdev_enable_event_code(dev, EV_KEY, ev.second.linux_code, nullptr);
  }

  auto err = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
  libevdev_free(dev);
  if (err != 0) {
    return Error(strerror(-err));
  }

  return libevdev_uinput_ptr{uidev, ::libevdev_uinput_destroy};
}

static std::optional<keyboard::KEY_MAP> press_btn(libevdev_uinput *kb, short key_code) {
  auto search_key = keyboard::key_mappings.find(key_code);
  if (search_key != keyboard::key_mappings.end()) {
    auto mapped_key = search_key->second;

    libevdev_uinput_write_event(kb, EV_MSC, MSC_SCAN, mapped_key.scan_code);
    libevdev_uinput_write_event(kb, EV_KEY, mapped_key.linux_code, 1);
    libevdev_uinput_write_event(kb, EV_SYN, SYN_REPORT, 0);
    return mapped_key;
  }
  return {};
}

Keyboard::Keyboard() : _state(std::make_shared<KeyboardState>()) {}

Keyboard::~Keyboard() {
  if (_state) {
    _state->stop_repeat_thread = true;
    if (_state->repeat_press_t.joinable()) {
      _state->repeat_press_t.join();
    }
  }
}

Result<Keyboard> Keyboard::create(const DeviceDefinition &device, int millis_repress_key) {
  auto kb_el = create_keyboard(device);
  if (kb_el) {
    Keyboard kb;
    kb._state->kb = std::move(*kb_el);
    // HELIX: Removed auto-repeat thread entirely.
    // The thread had a thread-safety bug: it iterated cur_press_keys while
    // press()/release() modified the vector without synchronization.
    // This caused keys to get stuck or stop working on RHEL (kernel 5.14).
    //
    // Note: This virtual keyboard does NOT have EV_REP enabled, so the kernel
    // will NOT generate auto-repeat events. With this thread removed, there is
    // NO auto-repeat functionality. This is a temporary fix to test if the
    // thread-safety issue is the root cause of the stuck key problem.
    // A proper fix would either add mutex synchronization to this thread,
    // or enable EV_REP on the virtual device to let the kernel handle it.
    (void)millis_repress_key; // Suppress unused parameter warning
    return kb;
  } else {
    return Error(kb_el.getErrorMessage());
  }
}

void Keyboard::press(short key_code) {
  if (auto keyboard = _state->kb.get()) {
    if (auto key = press_btn(keyboard, key_code)) {
      _state->cur_press_keys.push_back(key_code);
    }
  }
}

void Keyboard::release(short key_code) {
  auto search_key = keyboard::key_mappings.find(key_code);
  if (search_key != keyboard::key_mappings.end()) {
    if (auto keyboard = _state->kb.get()) {
      auto mapped_key = search_key->second;
      this->_state->cur_press_keys.erase(
          std::remove(this->_state->cur_press_keys.begin(), this->_state->cur_press_keys.end(), key_code),
          this->_state->cur_press_keys.end());

      libevdev_uinput_write_event(keyboard, EV_MSC, MSC_SCAN, mapped_key.scan_code);
      libevdev_uinput_write_event(keyboard, EV_KEY, mapped_key.linux_code, 0);
      libevdev_uinput_write_event(keyboard, EV_SYN, SYN_REPORT, 0);
    }
  }
}

std::vector<short> Keyboard::get_pressed_keys() const {
  // Return a copy of inputtino's internal pressed keys state
  return _state->cur_press_keys;
}

bool Keyboard::query_evdev_key_state(int linux_keycode) const {
  // Query the kernel's evdev state for this key via EVIOCGKEY ioctl
  if (auto kb = _state->kb.get()) {
    // Get the device node path
    const char* devnode = libevdev_uinput_get_devnode(kb);
    if (!devnode) return false;

    int fd = open(devnode, O_RDONLY);
    if (fd < 0) return false;

    // EVIOCGKEY returns a bitmask of currently pressed keys
    // We need (KEY_MAX + 7) / 8 bytes to hold the full bitmask
    unsigned char key_states[(KEY_MAX + 7) / 8] = {0};
    if (ioctl(fd, EVIOCGKEY(sizeof(key_states)), key_states) < 0) {
      close(fd);
      return false;
    }
    close(fd);

    // Check if the specific key is pressed in the bitmask
    return (key_states[linux_keycode / 8] >> (linux_keycode % 8)) & 1;
  }
  return false;
}

std::vector<int> Keyboard::get_evdev_pressed_keys() const {
  std::vector<int> pressed;

  if (auto kb = _state->kb.get()) {
    const char* devnode = libevdev_uinput_get_devnode(kb);
    if (!devnode) return pressed;

    int fd = open(devnode, O_RDONLY);
    if (fd < 0) return pressed;

    unsigned char key_states[(KEY_MAX + 7) / 8] = {0};
    if (ioctl(fd, EVIOCGKEY(sizeof(key_states)), key_states) < 0) {
      close(fd);
      return pressed;
    }
    close(fd);

    // Iterate through all keys we support and check if they're pressed
    for (const auto& mapping : keyboard::key_mappings) {
      int linux_code = mapping.second.linux_code;
      if (linux_code > 0 && linux_code < KEY_MAX) {
        if ((key_states[linux_code / 8] >> (linux_code % 8)) & 1) {
          pressed.push_back(linux_code);
        }
      }
    }
  }

  return pressed;
}

} // namespace inputtino