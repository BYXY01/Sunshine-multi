/**
 * @file src/session_group.h
 * @brief Declarations for multi-session window capture groups.
 *
 * A session group is a declarative configuration entity: a set of window
 * matching rules combined with a Moonlight port. Each configured group is
 * rendered by its own capture thread and owns an independent
 * `platf::display_t` window-compositing backend. The configuration can be
 * supplied either through a JSON file or through command-line options.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace session_group {

  /**
   * @brief Window matching rule; any non-empty field participates in an OR
   * match (a window belongs to the group when it satisfies at least one rule).
   */
  struct window_rule_t {
    std::string box;  ///< Sandboxie box name (process-group match).
    std::string process;  ///< Process executable name match.
    std::string title;  ///< Window title wildcard match.
    std::string window_class;  ///< Window class name match.
    std::uintptr_t hwnd {0};  ///< Direct Win32 window handle (HWND) match, highest priority.

    /**
     * @brief Check whether every field is empty.
     *
     * @return True when the rule carries no matching criteria.
     */
    [[nodiscard]] bool empty() const {
      return box.empty() && process.empty() && title.empty() && window_class.empty() && hwnd == 0;
    }
  };

  /**
   * @brief Captured display name for the window compositing backend.
   */
  inline constexpr std::string_view CAPTURE_WINDOW = "window";  ///< Window compositing backend.
  inline constexpr std::string_view CAPTURE_MONITOR = "monitor";  ///< Stock monitor capture backend.

  /**
   * @brief A single declarative session group.
   */
  struct config_t {
    std::string name;  ///< Unique session group name.
    std::string capture {CAPTURE_WINDOW};  ///< Capture backend: `window` or `monitor`.
    std::uint16_t port {0};  ///< Moonlight TCP port used by this group.
    std::uintptr_t hwnd {0};  ///< Direct Win32 window handle to capture; overrides all rules when non-zero.
    std::vector<window_rule_t> rules;  ///< Window matching rules (OR).
    std::vector<std::string> aux_include;  ///< Auxiliary window classes always composited (e.g. menus/dialogs).
    std::vector<std::string> aux_exclude;  ///< Auxiliary window classes filtered from the frame.
    int max_fps {60};  ///< Maximum capture framerate.
    int bitrate_kbps {0};  ///< Stream bitrate in kbps; 0 leaves the client to decide.

    /**
     * @brief Check whether this group uses the window compositing backend.
     *
     * @return True when the capture backend is `window`.
     */
    [[nodiscard]] bool is_window_capture() const {
      return capture == CAPTURE_WINDOW;
    }
  };

  /**
   * @brief Complete set of session groups parsed from configuration.
   */
  struct groups_config_t {
    std::vector<config_t> groups;  ///< Ordered session groups.
  };

  /**
   * @brief Runtime global holding the active session groups.
   *
   * Populated from the session groups file and/or CLI options during
   * `config::parse`. Consumed by the capture orchestration layer.
   */
  extern groups_config_t active_groups;

  /**
   * @brief Parse session group configuration from JSON text.
   *
   * @param json_text Raw JSON document to parse.
   * @return Parsed groups, or nullopt when the document is malformed.
   */
  std::optional<groups_config_t> parse_groups(const std::string_view &json_text);

  /**
   * @brief Load and parse a session groups JSON file.
   *
   * @param path Path to the JSON file.
   * @return Parsed groups, or nullopt when the file cannot be read or parsed.
   */
  std::optional<groups_config_t> load_groups(const std::filesystem::path &path);

  /**
   * @brief Validate a parsed group configuration.
   *
   * Checks for unique group names, unique non-zero ports, a supported capture
   * backend, and at least one matching rule for `window` groups.
   *
   * @param groups Configuration to validate.
   * @return Human-readable error descriptions; empty when the configuration is valid.
   */
  std::vector<std::string> validate_groups(const groups_config_t &groups);

  /**
   * @brief Parse a window handle string into a numeric HWND value.
   *
   * @param value Raw handle text from configuration.
   * @return Parsed HWND value, or 0 when the text is empty or malformed.
   */
  std::uintptr_t parse_hwnd(const std::string &value);

  /**
   * @brief Accumulated command-line session group options.
   */
  struct cli_options_t {
    std::string group_name;  ///< Value of `--group`.
    std::string capture;  ///< Value of `--capture`.
    std::string box;  ///< Value of `--box`.
    std::string process;  ///< Value of `--process`.
    std::string title;  ///< Value of `--title`.
    std::string window_class;  ///< Value of `--class`.
    std::uintptr_t hwnd {0};  ///< Value of `--hwnd`.
    std::optional<std::uint16_t> port;  ///< Value of `--port`.
    std::optional<std::filesystem::path> config_file;  ///< Value of `--config`.
  };

  /**
   * @brief Check whether a long option name is a session group CLI option.
   *
   * @param name Option name without the leading `--`.
   * @return True for `group`, `capture`, `box`, `process`, `title`, `class`, `port`, or `config`.
   */
  bool is_cli_option(const std::string_view &name);

  /**
   * @brief Apply a session group CLI option to the accumulator.
   *
   * @param name Option name without the leading `--`.
   * @param value Option value.
   * @param opts Accumulator updated on success.
   * @return True when the option was recognized and applied.
   */
  bool apply_cli_option(const std::string_view &name, const std::string_view &value, cli_options_t &opts);

  /**
   * @brief Build a groups configuration from accumulated CLI options.
   *
   * When `--config` is set, the referenced file is loaded. Otherwise a single
   * group is assembled from `--group` plus the optional rule options. When no
   * session group option was provided, an empty configuration is returned.
   *
   * @param opts Accumulated CLI options.
   * @return Built configuration, or nullopt on a missing group name / load failure.
   */
  std::optional<groups_config_t> groups_from_cli(const cli_options_t &opts);

  /**
   * @brief Resolve the active window-capture group name.
   *
   * Returns the name of the single window-capture group when exactly one is
   * configured; returns an empty string when there are none or more than one.
   * Stream sessions use this to share one capture thread per group.
   *
   * @return Active window group name, or empty.
   */
  std::string resolve_active_window_group();

}  // namespace session_group
