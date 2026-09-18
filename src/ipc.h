/**
 * @file src/ipc.h
 * @brief Declarations for the single-instance control IPC channel.
 *
 * The resident Sunshine instance owns a platform IPC endpoint (a named pipe on
 * Windows, a Unix domain socket elsewhere) and a single-instance guard. A short
 * lived CLI invocation (`sunshine attach <hwnd> --session <name>`) connects to
 * the running instance, forwards the command, and exits without starting a
 * second service body.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <string>

namespace ipc {

  /**
   * @brief A runtime window-capture command forwarded to the resident instance.
   */
  struct command_t {
    std::string action;  ///< Command name: `attach`, `detach`, or `filter`.
    std::string session;  ///< Session identifier (client name) the command targets.
    std::uintptr_t hwnd {0};  ///< Window handle target for attach/detach.
    std::string window_class;  ///< Window class target for filter.
    bool filter_remove {false};  ///< True for `filter --remove`, false for `filter --add`.
  };

  /// Handler invoked by the server for each received command; returns the reply text.
  using handler_t = std::function<std::string(const command_t &)>;

  /**
   * @brief Register the command handler invoked by the IPC server.
   *
   * @param handler Handler that applies the command and returns a reply.
   */
  void set_handler(handler_t handler);

  /**
   * @brief Acquire the single-instance guard and start the control IPC server.
   *
   * @return 0 on success; nonzero when another instance is already running.
   */
  int start();

  /**
   * @brief Stop the control IPC server and release the single-instance guard.
   */
  void stop();

  /**
   * @brief Forward a control command to the resident Sunshine instance.
   *
   * @param cmd Command to forward.
   * @return 0 on success; nonzero when no resident instance is reachable.
   */
  int send(const command_t &cmd);

}  // namespace ipc
