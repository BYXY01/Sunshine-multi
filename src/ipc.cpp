/**
 * @file src/ipc.cpp
 * @brief Definitions for the single-instance control IPC channel.
 */
// standard includes
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

// local includes
#include "ipc.h"
#include "logging.h"
#include "utility.h"

using namespace std::literals;

namespace ipc {
  namespace {
#ifdef _WIN32
    constexpr const char *k_pipe_name = R"(\\.\pipe\sunshine-ctrl)";  ///< Named pipe endpoint.
    constexpr const char *k_singleton_name = "Local\\Sunshine-singleton";  ///< Single-instance mutex name.
    HANDLE singleton_mutex {nullptr};  ///< Handle held while the instance is resident.
    HANDLE listen_pipe {INVALID_HANDLE_VALUE};  ///< Named pipe currently waiting for a client.
#else
    constexpr const char *k_socket_name = "sunshine.sock";  ///< Unix domain socket file name.
    int listen_fd {-1};  ///< Unix listening socket.
#endif

    std::atomic_bool ipc_running {false};  ///< Server accept-loop running flag.
    std::thread ipc_thread;  ///< Server accept-loop thread.
    handler_t command_handler;  ///< Handler applying received commands.

    /**
     * @brief Serialize a command into a length-prefixed frame.
     *
     * @param cmd Command to serialize.
     * @return Encoded frame.
     */
    std::string encode(const command_t &cmd) {
      std::string payload;
      payload += cmd.action;
      payload.push_back('\0');
      payload += cmd.session;
      payload.push_back('\0');
      payload += std::to_string(cmd.hwnd);
      payload.push_back('\0');
      payload += cmd.window_class;
      payload.push_back('\0');
      payload += cmd.filter_remove ? "1" : "0";

      const auto len = static_cast<std::uint32_t>(payload.size());
      std::string frame(reinterpret_cast<const char *>(&len), sizeof(len));
      frame += payload;
      return frame;
    }

    /**
     * @brief Deserialize a length-prefixed command payload.
     *
     * @param payload Serialized command fields.
     * @return Decoded command, or nullopt when the payload is malformed.
     */
    std::optional<command_t> decode(std::string_view payload) {
      std::vector<std::string> fields;
      std::size_t pos = 0;
      for (int i = 0; i < 5 && pos <= payload.size(); ++i) {
        auto end = payload.find('\0', pos);
        if (end == std::string_view::npos) {
          fields.emplace_back(payload.substr(pos));
          pos = payload.size() + 1;
        } else {
          fields.emplace_back(payload.substr(pos, end - pos));
          pos = end + 1;
        }
      }
      if (fields.size() != 5) {
        return std::nullopt;
      }

      command_t cmd;
      cmd.action = fields[0];
      cmd.session = fields[1];
      cmd.hwnd = static_cast<std::uintptr_t>(std::strtoull(fields[2].c_str(), nullptr, 10));
      cmd.window_class = fields[3];
      cmd.filter_remove = fields[4] == "1";
      if (cmd.action.empty()) {
        return std::nullopt;
      }
      return cmd;
    }

    /**
     * @brief Apply a command via the registered handler.
     *
     * @param payload Serialized command payload.
     * @return Reply text.
     */
    std::string apply(const std::string &payload) {
      auto cmd = decode(payload);
      if (!cmd) {
        return "ERR malformed command";
      }
      if (!command_handler) {
        return "ERR no command handler";
      }
      return command_handler(*cmd);
    }

#ifdef _WIN32
    /**
     * @brief Service a single named-pipe connection: read one frame, apply it, reply.
     *
     * @param pipe Client pipe handle; closed when the function returns.
     */
    void handle_windows_connection(HANDLE pipe) {
      auto close = util::fail_guard([&]() {
        CloseHandle(pipe);
      });

      std::uint32_t len = 0;
      DWORD read = 0;
      if (!ReadFile(pipe, &len, sizeof(len), &read, nullptr) || read != sizeof(len) || len == 0 || len > (1u << 20)) {
        return;
      }
      std::string payload(len, '\0');
      std::size_t got = 0;
      while (got < len) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, payload.data() + got, static_cast<DWORD>(len - got), &chunk, nullptr) || chunk == 0) {
          return;
        }
        got += chunk;
      }

      std::string reply = apply(payload);
      const auto reply_len = static_cast<std::uint32_t>(reply.size());
      std::string out(reinterpret_cast<const char *>(&reply_len), sizeof(reply_len));
      out += reply;
      DWORD written = 0;
      WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    }

    /**
     * @brief Named-pipe accept loop running on the IPC server thread.
     */
    void windows_accept_loop() {
      while (ipc_running) {
        HANDLE pipe = CreateNamedPipeA(
          k_pipe_name,
          PIPE_ACCESS_DUPLEX,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
          PIPE_UNLIMITED_INSTANCES,
          1 << 20,
          1 << 20,
          0,
          nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
          if (!ipc_running) {
            break;
          }
          BOOST_LOG(warning) << "ipc: CreateNamedPipe failed with error "sv << GetLastError();
          std::this_thread::sleep_for(100ms);
          continue;
        }
        listen_pipe = pipe;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
          CloseHandle(pipe);
          listen_pipe = INVALID_HANDLE_VALUE;
          continue;
        }
        listen_pipe = INVALID_HANDLE_VALUE;
        std::thread(handle_windows_connection, pipe).detach();
      }
    }
#else
    /**
     * @brief Return the Unix domain socket path for the control channel.
     *
     * @return Socket path under `$XDG_RUNTIME_DIR`, falling back to `/tmp`.
     */
    std::string socket_path() {
      const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      std::string base = (runtime_dir && *runtime_dir) ? runtime_dir : "/tmp";
      return base + "/" + k_socket_name;
    }

    /**
     * @brief Read exactly n bytes from a socket.
     *
     * @param fd Socket file descriptor.
     * @param buf Output buffer.
     * @param n Number of bytes to read.
     * @return True when all bytes were read.
     */
    bool read_full(int fd, void *buf, std::size_t n) {
      auto *p = static_cast<char *>(buf);
      std::size_t got = 0;
      while (got < n) {
        auto r = ::recv(fd, p + got, n - got, 0);
        if (r <= 0) {
          return false;
        }
        got += static_cast<std::size_t>(r);
      }
      return true;
    }

    /**
     * @brief Write exactly n bytes to a socket.
     *
     * @param fd Socket file descriptor.
     * @param buf Input buffer.
     * @param n Number of bytes to write.
     * @return True when all bytes were written.
     */
    bool write_full(int fd, const void *buf, std::size_t n) {
      auto *p = static_cast<const char *>(buf);
      std::size_t sent = 0;
      while (sent < n) {
        auto r = ::send(fd, p + sent, n - sent, 0);
        if (r <= 0) {
          return false;
        }
        sent += static_cast<std::size_t>(r);
      }
      return true;
    }

    /**
     * @brief Service a single socket connection: read one frame, apply it, reply.
     *
     * @param fd Client socket; closed when the function returns.
     */
    void handle_unix_connection(int fd) {
      auto close = util::fail_guard([&]() {
        ::close(fd);
      });

      std::uint32_t len = 0;
      if (!read_full(fd, &len, sizeof(len)) || len == 0 || len > (1u << 20)) {
        return;
      }
      std::string payload(len, '\0');
      if (!read_full(fd, payload.data(), len)) {
        return;
      }

      std::string reply = apply(payload);
      const auto reply_len = static_cast<std::uint32_t>(reply.size());
      std::string out(reinterpret_cast<const char *>(&reply_len), sizeof(reply_len));
      out += reply;
      write_full(fd, out.data(), out.size());
    }

    /**
     * @brief Unix domain socket accept loop running on the IPC server thread.
     */
    void unix_accept_loop() {
      while (ipc_running) {
        int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
          if (!ipc_running) {
            break;
          }
          continue;
        }
        std::thread(handle_unix_connection, client).detach();
      }
    }
#endif
  }  // namespace

  void set_handler(handler_t handler) {
    command_handler = std::move(handler);
  }

  int start() {
#ifdef _WIN32
    singleton_mutex = CreateMutexA(nullptr, TRUE, k_singleton_name);
    if (!singleton_mutex) {
      BOOST_LOG(error) << "ipc: failed to create single-instance mutex: "sv << GetLastError();
      return -1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      BOOST_LOG(error) << "ipc: another Sunshine instance is already running"sv;
      CloseHandle(singleton_mutex);
      singleton_mutex = nullptr;
      return -1;
    }
    ipc_running = true;
    ipc_thread = std::thread(windows_accept_loop);
#else
    const auto path = socket_path();
    listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      BOOST_LOG(error) << "ipc: failed to create control socket"sv;
      return -1;
    }

    sockaddr_un probe {};
    probe.sun_family = AF_UNIX;
    std::snprintf(probe.sun_path, sizeof(probe.sun_path), "%s", path.c_str());
    int probe_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe_fd >= 0) {
      if (::connect(probe_fd, reinterpret_cast<struct sockaddr *>(&probe), sizeof(probe)) == 0) {
        ::close(probe_fd);
        ::close(listen_fd);
        listen_fd = -1;
        BOOST_LOG(error) << "ipc: another Sunshine instance is already running"sv;
        return -1;
      }
      ::close(probe_fd);
      ::unlink(path.c_str());
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (::bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd, 8) != 0) {
      BOOST_LOG(error) << "ipc: failed to bind/listen control socket: "sv << std::strerror(errno);
      ::close(listen_fd);
      listen_fd = -1;
      return -1;
    }
    ipc_running = true;
    ipc_thread = std::thread(unix_accept_loop);
#endif
    BOOST_LOG(info) << "Started control IPC"sv;
    return 0;
  }

  void stop() {
    ipc_running = false;
#ifdef _WIN32
    if (ipc_thread.joinable()) {
      // The accept loop may be blocked in ConnectNamedPipe; detach on shutdown.
      ipc_thread.detach();
    }
    if (listen_pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(listen_pipe);
      listen_pipe = INVALID_HANDLE_VALUE;
    }
    if (singleton_mutex) {
      CloseHandle(singleton_mutex);
      singleton_mutex = nullptr;
    }
#else
    if (listen_fd >= 0) {
      ::shutdown(listen_fd, SHUT_RDWR);
      ::close(listen_fd);
      listen_fd = -1;
    }
    if (ipc_thread.joinable()) {
      ipc_thread.join();
    }
    ::unlink(socket_path().c_str());
#endif
  }

  int send(const command_t &cmd) {
    const auto frame = encode(cmd);
    std::string reply;

#ifdef _WIN32
    HANDLE pipe = CreateFileA(k_pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      BOOST_LOG(error) << "ipc: no resident Sunshine instance is running"sv;
      return -1;
    }
    auto close = util::fail_guard([&]() {
      CloseHandle(pipe);
    });

    DWORD written = 0;
    if (!WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr)) {
      return -1;
    }
    std::uint32_t len = 0;
    DWORD read = 0;
    if (!ReadFile(pipe, &len, sizeof(len), &read, nullptr) || read != sizeof(len)) {
      return -1;
    }
    reply.resize(len);
    std::size_t got = 0;
    while (got < len) {
      DWORD chunk = 0;
      if (!ReadFile(pipe, reply.data() + got, static_cast<DWORD>(len - got), &chunk, nullptr) || chunk == 0) {
        return -1;
      }
      got += chunk;
    }
#else
    const auto path = socket_path();
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    auto close = util::fail_guard([&]() {
      ::close(fd);
    });

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
      BOOST_LOG(error) << "ipc: no resident Sunshine instance is running"sv;
      return -1;
    }
    if (!write_full(fd, frame.data(), frame.size())) {
      return -1;
    }
    std::uint32_t len = 0;
    if (!read_full(fd, &len, sizeof(len))) {
      return -1;
    }
    reply.resize(len);
    if (!read_full(fd, reply.data(), len)) {
      return -1;
    }
#endif

    std::cout << reply << std::endl;
    return 0;
  }
}  // namespace ipc
