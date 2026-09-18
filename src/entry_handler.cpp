/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <csignal>
#include <format>
#include <iostream>
#include <thread>

// local includes
#include "config.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#include "session_group.h"
#include "utility.h"

extern "C" {
#ifdef _WIN32
  #include <iphlpapi.h>
#endif
}

using namespace std::literals;

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

namespace args {
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name);
    }

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

#ifdef _WIN32
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif

  namespace {
    /**
     * @brief Extract the `--session <name>` value from command arguments.
     *
     * @param argc Number of arguments.
     * @param argv Argument vector.
     * @param session Output session identifier.
     * @return True when `--session` is absent or carries a value; false when it
     * is present without a value.
     */
    bool extract_session(int argc, char *argv[], std::string &session) {
      for (int i = 0; i < argc; ++i) {
        if (argv[i] == "--session"sv) {
          if (i + 1 >= argc) {
            return false;
          }
          session = argv[i + 1];
          return true;
        }
      }
      return true;
    }
  }  // namespace

  int attach(const char *name, int argc, char *argv[]) {
    if (argc < 1 || argv[0] == "help"sv) {
      return help(name);
    }

    ipc::command_t cmd;
    cmd.action = "attach";
    if (!extract_session(argc, argv, cmd.session)) {
      BOOST_LOG(error) << "attach: --session requires a value"sv;
      return -1;
    }
    cmd.hwnd = session_group::parse_hwnd(argv[0]);
    if (cmd.hwnd == 0 || cmd.session.empty()) {
      BOOST_LOG(error) << "attach: usage: sunshine --attach <hwnd> --session <name>"sv;
      return -1;
    }
    return ipc::send(cmd);
  }

  int detach(const char *name, int argc, char *argv[]) {
    if (argc < 1 || argv[0] == "help"sv) {
      return help(name);
    }

    ipc::command_t cmd;
    cmd.action = "detach";
    if (!extract_session(argc, argv, cmd.session)) {
      BOOST_LOG(error) << "detach: --session requires a value"sv;
      return -1;
    }
    cmd.hwnd = session_group::parse_hwnd(argv[0]);
    if (cmd.hwnd == 0 || cmd.session.empty()) {
      BOOST_LOG(error) << "detach: usage: sunshine --detach <hwnd> --session <name>"sv;
      return -1;
    }
    return ipc::send(cmd);
  }

  int filter(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv) {
      return help(name);
    }

    ipc::command_t cmd;
    cmd.action = "filter";
    if (!extract_session(argc, argv, cmd.session)) {
      BOOST_LOG(error) << "filter: --session requires a value"sv;
      return -1;
    }
    if (argv[0] == "--add"sv) {
      cmd.filter_remove = false;
    } else if (argv[0] == "--remove"sv) {
      cmd.filter_remove = true;
    } else {
      BOOST_LOG(error) << "filter: usage: sunshine --filter --add|--remove <class> --session <name>"sv;
      return -1;
    }
    cmd.window_class = argv[1];
    if (cmd.session.empty()) {
      BOOST_LOG(error) << "filter: --session is required"sv;
      return -1;
    }
    return ipc::send(cmd);
  }

  std::string apply_control_command(const ipc::command_t &cmd) {
    if (cmd.session.empty()) {
      return "ERR missing --session";
    }
    if (cmd.action == "attach") {
      if (cmd.hwnd == 0) {
        return "ERR attach requires a window handle";
      }
      session_group::session_runtime_attach(cmd.session, cmd.hwnd);
      return std::string {"OK attached 0x"} + std::string {util::hex(cmd.hwnd).to_string_view()} + " to session '" + cmd.session + "'";
    }
    if (cmd.action == "detach") {
      if (cmd.hwnd == 0) {
        return "ERR detach requires a window handle";
      }
      session_group::session_runtime_detach(cmd.session, cmd.hwnd);
      return std::string {"OK detached 0x"} + std::string {util::hex(cmd.hwnd).to_string_view()} + " from session '" + cmd.session + "'";
    }
    if (cmd.action == "filter") {
      if (cmd.window_class.empty()) {
        return "ERR filter requires a window class";
      }
      if (cmd.filter_remove) {
        session_group::session_runtime_remove_exclude(cmd.session, cmd.window_class);
        return "OK unexcluded class '" + cmd.window_class + "' for session '" + cmd.session + "'";
      }
      session_group::session_runtime_add_exclude(cmd.session, cmd.window_class);
      return "OK excluded class '" + cmd.window_class + "' for session '" + cmd.session + "'";
    }
    return "ERR unknown action '" + cmd.action + "'";
  }
}  // namespace args

namespace lifetime {
  char **argv;  ///< Command-line argument vector.
  std::atomic_int desired_exit_code;  ///< Desired exit code.

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  /**
   * @brief Owns Windows service-manager handles for the Sunshine service.
   */
  class service_controller {
  public:
    /**
     * @brief Open the Windows service manager and Sunshine service handle.
     *
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      service_handle = OpenServiceA(scm_handle, "SunshineService", service_desired_access);
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     *
     * @return True when the Windows service API call succeeds.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     *
     * @return True when the Windows service API call succeeds.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << std::format("{} failed to start: {}"sv, platf::SERVICE_NAME, status.dwWin32ExitCode);
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
