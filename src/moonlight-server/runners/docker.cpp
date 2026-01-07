#include <runners/docker.hpp>

namespace wolf::core::docker {

void create_udev_hw_files(std::filesystem::path base_hw_db_path,
                          std::vector<std::pair<std::string, std::vector<std::string>>> udev_hw_db_entries) {
  for (const auto &[filename, content] : udev_hw_db_entries) {
    auto host_file_path = (base_hw_db_path / filename).string();
    logs::log(logs::debug, "[DOCKER] Writing hwdb file: {}", host_file_path);
    std::ofstream host_file(host_file_path);
    host_file << utils::join(content, "\n");
    host_file.close();
  }
}

/**
 * @brief returns the major number associated with the requested device type (if found)
 * Internally will read /proc/devices line by line and return the first match
 */
static std::optional<std::string> get_device_major(std::string_view type) {
  std::ifstream devices_file("/proc/devices");
  std::string line;
  while (std::getline(devices_file, line)) {
    if (line.find(type) != std::string::npos) {
      // Example line: "244 hidraw" or " 13 input" (note the leading space)
      line.erase(line.begin(),
                 std::find_if(line.begin(), line.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }));
      return line.substr(0, line.find(' '));
    }
  }
  return std::nullopt;
}

void RunDocker::run(std::string_view session_id,
                    std::string_view app_state_folder,
                    std::string_view host_xdg_runtime_dir,
                    std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                    const immer::array<std::string> &virtual_inputs,
                    const immer::array<std::pair<std::string, std::string>> &paths,
                    const immer::map<std::string, std::string> &env_variables,
                    std::string_view render_node) {

  std::vector<std::string> full_env;
  full_env.insert(full_env.end(), this->container.env.begin(), this->container.env.end());
  for (const auto &env_var : env_variables) {
    full_env.push_back(fmt::format("{}={}", env_var.first, env_var.second));
  }

  std::vector<Device> devices;
  devices.insert(devices.end(), this->container.devices.begin(), this->container.devices.end());
  for (const auto &v_input : virtual_inputs) {
    devices.push_back(Device{.path_on_host = to_string(v_input),
                             .path_in_container = to_string(v_input),
                             .cgroup_permission = "mrw"});
  }

  std::vector<MountPoint> mounts;
  mounts.insert(mounts.end(), this->container.mounts.begin(), this->container.mounts.end());
  for (const auto &path : paths) {
    mounts.insert(mounts.end(), MountPoint{.source = path.first, .destination = path.second, .mode = "rw"});
  }

  // Fake udev
  auto udev_base_path = std::filesystem::path(app_state_folder) / "udev";
  auto hw_db_path = udev_base_path / "data";
  auto fake_udev_cli_path = std::string(utils::get_env("WOLF_DOCKER_FAKE_UDEV_PATH", ""));
  bool use_fake_udev = !fake_udev_cli_path.empty() || std::filesystem::exists(fake_udev_cli_path);
  if (use_fake_udev) {
    logs::log(logs::debug, "[DOCKER] Using fake-udev, creating {}", hw_db_path.string());
    std::filesystem::create_directories(hw_db_path);

    // Check if /run/udev/control exists
    auto udev_ctrl_path = udev_base_path / "control";
    if (!std::filesystem::exists(udev_ctrl_path)) {
      if (auto control_file = std::ofstream(udev_ctrl_path)) {
        control_file.close();
        std::filesystem::permissions(udev_ctrl_path, std::filesystem::perms::all); // set 777
      }
    }
  } else {
    logs::log(logs::warning,
              "[DOCKER] Unable to use fake-udev, check the env variable WOLF_DOCKER_FAKE_UDEV_PATH and the file at {}",
              fake_udev_cli_path);
  }

  // PipeWire socket sharing - allows Wolf to connect to PipeWire running inside the container
  // This is needed for PipeWire ScreenCast video capture mode (GNOME 49+)
  // IMPORTANT: Only override XDG_RUNTIME_DIR for pipewire mode!
  // For wayland mode (Sway/KDE), XDG_RUNTIME_DIR must remain /tmp/sockets where Wolf's
  // gst-wayland-display compositor creates the wayland socket.
  auto pipewire_base_path = std::filesystem::path(app_state_folder) / "pipewire";
  std::filesystem::create_directories(pipewire_base_path);
  logs::log(logs::debug, "[DOCKER] PipeWire socket path: {}", pipewire_base_path.string());

  // Check if we're in pipewire mode by looking at WOLF_VIDEO_SOURCE_MODE in env_variables
  bool use_pipewire_mode = false;
  if (auto mode_it = env_variables.find("WOLF_VIDEO_SOURCE_MODE")) {
    use_pipewire_mode = (*mode_it == "pipewire");
    logs::log(logs::debug, "[DOCKER] Video source mode: {}, use_pipewire_mode: {}", *mode_it, use_pipewire_mode);
  }

  if (use_pipewire_mode) {
    // Mount at /run/user/1000 where PipeWire daemon creates its socket (pipewire-0)
    mounts.push_back(MountPoint{.source = pipewire_base_path.string(), .destination = "/run/user/1000", .mode = "rw"});
    // Set XDG_RUNTIME_DIR in container to match the mount point
    // This overrides the /tmp/sockets value from common.cpp, which is correct for pipewire mode
    full_env.push_back("XDG_RUNTIME_DIR=/run/user/1000");
    logs::log(logs::debug, "[DOCKER] Pipewire mode: XDG_RUNTIME_DIR=/run/user/1000");
  } else {
    // For wayland mode (Sway/KDE), do NOT override XDG_RUNTIME_DIR
    // The correct value (/tmp/sockets) is already set by common.cpp, and that's where
    // Wolf's gst-wayland-display creates the wayland socket for nested compositors.
    // Still create the pipewire directory in case it's needed later, but don't mount it
    logs::log(logs::debug, "[DOCKER] Wayland mode: keeping XDG_RUNTIME_DIR from common.cpp (should be /tmp/sockets)");
  }

  // Per-lobby socket mounting - provides isolated API for multi-tenant security
  // The lobby.sock is created by LobbySocketServer in the same app_state_folder
  // Mount it at a known location so the container can use it
  auto lobby_socket_host_path = std::filesystem::path(app_state_folder) / "lobby.sock";
  auto lobby_socket_container_path = "/var/run/wolf/lobby.sock";
  // Only mount if the socket exists (it's created by LobbySocketServer before container starts)
  if (std::filesystem::exists(lobby_socket_host_path)) {
    logs::log(logs::debug, "[DOCKER] Mounting per-lobby socket: {} -> {}",
              lobby_socket_host_path.string(), lobby_socket_container_path);
    mounts.push_back(MountPoint{.source = lobby_socket_host_path.string(),
                                .destination = lobby_socket_container_path,
                                .mode = "rw"});
    full_env.push_back(fmt::format("WOLF_LOBBY_SOCKET_PATH={}", lobby_socket_container_path));
  } else {
    logs::log(logs::warning, "[DOCKER] Per-lobby socket not found at {}, container won't have isolated API access",
              lobby_socket_host_path.string());
  }

  // Add equivalent of --gpu=all if on NVIDIA without the custom driver volume
  auto final_json_opts = this->base_create_json;
  if (get_vendor(render_node) == NVIDIA && !utils::get_env("NVIDIA_DRIVER_VOLUME_NAME")) {
    logs::log(logs::info, "NVIDIA_DRIVER_VOLUME_NAME not set, assuming nvidia driver toolkit is installed..");
    {
      auto parsed_json = utils::parse_json(final_json_opts).as_object();
      auto default_gpu_config = boost::json::array{                    // [
                                                   boost::json::object{// {
                                                                       {"DeviceIDs", {"all"}},
                                                                       {"Capabilities", boost::json::array{{"gpu"}}}}};
      if (auto host_config_ptr = parsed_json.if_contains("HostConfig")) {
        auto host_config = host_config_ptr->as_object();
        if (host_config.find("DeviceRequests") == host_config.end()) {
          host_config["DeviceRequests"] = default_gpu_config;
          host_config["Runtime"] = "nvidia";
          parsed_json["HostConfig"] = host_config;
          final_json_opts = boost::json::serialize(parsed_json);
        } else {
          logs::log(logs::debug, "DeviceRequests manually set in base_create_json, skipping..");
        }
      } else {
        logs::log(logs::warning, "HostConfig not found in base_create_json.");
        parsed_json["HostConfig"] = boost::json::object{{"DeviceRequests", default_gpu_config}, {"Runtime", "nvidia"}};
        final_json_opts = boost::json::serialize(parsed_json);
      }
    }

    // Setup -e NVIDIA_VISIBLE_DEVICES=all  -e NVIDIA_DRIVER_CAPABILITIES=all if not present
    {
      auto nvd_env = std::find_if(full_env.begin(), full_env.end(), [](const std::string &env) {
        return env.find("NVIDIA_VISIBLE_DEVICES") != std::string::npos;
      });
      if (nvd_env == full_env.end()) {
        full_env.push_back("NVIDIA_VISIBLE_DEVICES=all");
      }

      auto nvd_caps_env = std::find_if(full_env.begin(), full_env.end(), [](const std::string &env) {
        return env.find("NVIDIA_DRIVER_CAPABILITIES") != std::string::npos;
      });
      if (nvd_caps_env == full_env.end()) {
        full_env.push_back("NVIDIA_DRIVER_CAPABILITIES=all");
      }
    }
  }

  { // Setup Wolf socket path (if the runner needs it, and it hasn't been overridden via ENV)
    auto socket_path_container_env = std::find_if(full_env.begin(), full_env.end(), [](const std::string &env) {
      return env.find("WOLF_SOCKET_PATH") != std::string::npos;
    });
    if (!get_env("WOLF_SOCKET_PATH") && socket_path_container_env != full_env.end()) {
      // Change the associated mount point to pick up the right path from the host
      for (auto &mount : mounts) {
        if (mount.destination.find("wolf.sock") != std::string::npos) {
          mount.source = std::filesystem::path(host_xdg_runtime_dir) / "wolf.sock";
          break;
        }
      }
    }
  }

  // when creating a virtual DualSense device we need to also mount a `/dev/hidraw*` device.
  // unfortunately hidraw devices use dynamically assigned major numbers rather than static ones
  // so we'll get the major number from reading `/proc/devices` for `hidraw` and `input`
  // and set the right entries in `DeviceCgroupRules`
  {
    auto hidraw_major = get_device_major("hidraw");
    auto input_major = get_device_major("input");
    if (hidraw_major && input_major) {
      logs::log(logs::debug,
                "[DOCKER] Setting DeviceCgroupRules for hidraw:{} and input:{}",
                *hidraw_major,
                *input_major);
      auto parsed_json = utils::parse_json(final_json_opts).as_object();
      if (auto host_config_ptr = parsed_json.if_contains("HostConfig")) {
        auto host_config = host_config_ptr->as_object();
        host_config["DeviceCgroupRules"] = json::array{
            fmt::format("c {}:* rwm", *hidraw_major),
            fmt::format("c {}:* rwm", *input_major),
        };
        parsed_json["HostConfig"] = host_config;
      } else {
        parsed_json["HostConfig"] = json::object{
            {"DeviceCgroupRules",
             json::array{
                 fmt::format("c {}:* rwm", *hidraw_major),
                 fmt::format("c {}:* rwm", *input_major),
             }},
        };
      }
      final_json_opts = boost::json::serialize(parsed_json);
    } else {
      logs::log(logs::warning, "[DOCKER] Failed to get major numbers for hidraw and input");
    }
  }

  logs::log(logs::debug, "[DOCKER] Container options: {}", final_json_opts);

  Container new_container = {.id = "",
                             .name = fmt::format("{}_{}", this->container.name, session_id),
                             .image = this->container.image,
                             .status = CREATED,
                             .ports = this->container.ports,
                             .mounts = mounts,
                             .devices = devices,
                             .env = full_env};

  if (auto docker_container = docker_api.create(new_container, final_json_opts)) {
    auto container_id = docker_container->id;
    docker_api.start_by_id(container_id);

    logs::log(logs::info, "[DOCKER] Starting container: {}", docker_container->name);
    logs::log(logs::debug, "[DOCKER] Starting container: {}", *docker_container);

    auto terminate_handler = this->ev_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, container_id, this](const immer::box<events::StopStreamEvent> &terminate_ev) {
          if (std::to_string(terminate_ev->session_id) == session_id) {
            docker_api.stop_by_id(container_id);
          }
        });

    auto terminate_lobby_handler = this->ev_bus->register_handler<immer::box<events::StopLobbyEvent>>(
        [session_id, container_id, this](const immer::box<events::StopLobbyEvent> &terminate_ev) {
          if (terminate_ev->lobby_id == session_id) {
            docker_api.stop_by_id(container_id);
          }
        });

    auto unplug_device_handler = this->ev_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
        [session_id, container_id, hw_db_path, this](const immer::box<events::UnplugDeviceEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[DOCKER] Received UnplugDeviceEvent for session: {}", ev->session_id);
            for (const auto &[filename, content] : ev->udev_hw_db_entries) {
              try {
                std::filesystem::remove(hw_db_path / filename);
              } catch (const std::filesystem::filesystem_error &e) {
                logs::log(logs::warning, "[DOCKER] Failed to remove udev hwdb entry: {}", e.what());
              }
            }

            for (auto udev_ev : ev->udev_events) {
              udev_ev["ACTION"] = "remove";
              std::string udev_msg = base64_encode(map_to_string(udev_ev));
              std::string cmd;
              if (udev_ev.count("DEVNAME") == 0) {
                cmd = fmt::format("fake-udev -m {}", udev_msg);
              } else {
                cmd = fmt::format("fake-udev -m {} && rm {}", udev_msg, udev_ev["DEVNAME"]);
              }
              logs::log(logs::debug, "[DOCKER] Executing command: {}", cmd);
              docker_api.exec(container_id, {"/bin/bash", "-c", cmd}, "root");
            }
          }
        });

    do {
      // Plug all devices that are waiting in the queue
      while (auto device_ev = plugged_devices_queue->pop(50ms)) {
        if (device_ev->get().session_id == session_id) {
          logs::log(logs::debug, "[DOCKER] Plugging device from queue in session: {}", session_id);
          if (use_fake_udev) {
            create_udev_hw_files(hw_db_path, device_ev->get().udev_hw_db_entries);
          }

          for (auto udev_ev : device_ev->get().udev_events) {
            std::string cmd;
            std::string udev_msg = base64_encode(map_to_string(udev_ev));
            if (udev_ev.count("DEVNAME") == 0) {
              cmd = fmt::format("fake-udev -m {}", udev_msg);
            } else {
              cmd = fmt::format("mkdir -p /dev/input && mknod {} c {} {} && chmod 777 {} && fake-udev -m {}",
                                udev_ev["DEVNAME"],
                                udev_ev["MAJOR"],
                                udev_ev["MINOR"],
                                udev_ev["DEVNAME"],
                                udev_msg);
            }
            logs::log(logs::debug, "[DOCKER] Executing command: {}", cmd);
            docker_api.exec(container_id, {"/bin/bash", "-c", cmd}, "root");
          }
        }
      }

      std::this_thread::sleep_for(500ms);

    } while (docker_api.get_by_id(container_id)->status == RUNNING);

    logs::log(logs::debug, "[DOCKER] Container logs: \n{}", docker_api.get_logs(container_id));
    logs::log(logs::debug, "[DOCKER] Stopping container: {}", docker_container->name);
    if (const auto env = utils::get_env("WOLF_STOP_CONTAINER_ON_EXIT")) {
      if (std::string(env) == "TRUE") {
        docker_api.stop_by_id(container_id);
        docker_api.remove_by_id(container_id);
      }
    }
    logs::log(logs::info, "Stopped container: {}", docker_container->name);
    try {
      std::filesystem::remove_all(udev_base_path);
    } catch (const std::filesystem::filesystem_error &e) {
      logs::log(logs::warning, "Failed to remove udev base path: {}", e.what());
    }
    try {
      std::filesystem::remove_all(pipewire_base_path);
    } catch (const std::filesystem::filesystem_error &e) {
      logs::log(logs::warning, "Failed to remove pipewire base path: {}", e.what());
    }
  }
}

} // namespace wolf::core::docker