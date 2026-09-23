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
#include <unordered_set>
#include <vector>

namespace session_group {

  /**
   * @brief Window matching rule; any non-empty field participates in an OR
   * match (a window belongs to the group when it satisfies at least one rule).
   */
  struct window_rule_t {
    std::string box;  ///< Process-group container name (accepted for compatibility, matching not implemented).
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
   * @brief A single session inside a group.
   *
   * A session is one independent stream frame: the set of windows it captures
   * (main window plus its menus/dialogs) is composited, by real Z-order, into
   * that session's own picture. The session identifier is the Moonlight
   * `appid`, which is how a client selects which session (application) to
   * stream.
   */
  struct session_config_t {
    int id {0};  ///< Session identifier; equals the Moonlight `appid` used for routing.
    std::string name;  ///< Human-readable session name (client-facing).
    std::uintptr_t hwnd {0};  ///< Direct Win32 window handle; overrides all rules when non-zero.
    std::vector<window_rule_t> rules;  ///< Window matching rules for this session (OR).
    std::vector<std::string> aux_exclude;  ///< Auxiliary window classes filtered from this session's frame.

    /**
     * @brief Check whether the session carries no matching criteria.
     *
     * @return True when neither an explicit hwnd nor any rule is set.
     */
    [[nodiscard]] bool empty() const {
      return hwnd == 0 && rules.empty();
    }
  };

  /**
   * @brief A single declarative session group.
   *
   * A group is the isolation boundary (one capture thread, one port in
   * `per-group-port` mode) and holds one or more sessions. Each session is an
   * independent picture; the group-level fields apply to every session in it.
   */
  struct config_t {
    std::string name;  ///< Unique session group name.
    std::string capture {CAPTURE_WINDOW};  ///< Capture backend: `window` or `monitor`.
    std::uint16_t port {0};  ///< Moonlight TCP port used by this group.
    int max_fps {60};  ///< Maximum capture framerate.
    int bitrate_kbps {0};  ///< Stream bitrate in kbps; 0 leaves the client to decide.
    std::vector<session_config_t> sessions;  ///< Sessions (applications) served by this group.

    /**
     * @brief Check whether this group uses the window compositing backend.
     *
     * @return True when the capture backend is `window`.
     */
    [[nodiscard]] bool is_window_capture() const {
      return capture == CAPTURE_WINDOW;
    }
  };

  inline constexpr std::string_view PORT_MODE_SINGLE = "single-port";  ///< Single shared RTSP acceptor mode.
  inline constexpr std::string_view PORT_MODE_PER_GROUP = "per-group-port";  ///< One RTSP acceptor (and port) per group.

  /**
   * @brief Complete set of session groups parsed from configuration.
   */
  struct groups_config_t {
    std::string port_mode;  ///< Global port mode: `single-port` or `per-group-port` (mandatory).
    std::string port_range;  ///< Port range "start-end" required by `per-group-port`.
    std::string default_group;  ///< Group receiving sessions without an explicit group.
    std::vector<config_t> groups;  ///< Ordered session groups.
  };

  /**
   * @brief Parse a port range string into its inclusive bounds.
   *
   * Accepts the "48100-48110" form. The start must be non-zero, the end must
   * not exceed 65535, and start must be less than or equal to end.
   *
   * @param range Raw range text from configuration.
   * @return (start, end) bounds, or nullopt when the text is malformed.
   */
  std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_port_range(const std::string &range);

  /**
   * @brief Port allocator handing out free ports from a range.
   *
   * Ports are allocated in ascending order, always picking the smallest free
   * port, and released ports are returned to the pool for reuse. When the
   * pool is exhausted `allocate()` returns nullopt instead of failing.
   */
  class port_allocator_t {
  public:
    /**
     * @brief Construct an allocator over an inclusive port range.
     *
     * @param start First port of the range.
     * @param end Last port of the range (must be >= start).
     */
    port_allocator_t(std::uint16_t start, std::uint16_t end);

    /**
     * @brief Allocate the smallest free port from the pool.
     *
     * @return Allocated port, or nullopt when every port is in use.
     */
    std::optional<std::uint16_t> allocate();

    /**
     * @brief Return a previously allocated port to the pool.
     *
     * Releasing a port that is not currently allocated is a no-op.
     *
     * @param port Port to release.
     */
    void release(std::uint16_t port);

    /**
     * @brief Get the number of free ports remaining.
     *
     * @return Count of ports not currently allocated.
     */
    std::size_t available() const;

    /**
     * @brief Get the total number of ports in the range.
     *
     * @return Size of the full range.
     */
    std::size_t size() const;

  private:
    std::uint16_t start_;  ///< First port of the range.
    std::uint16_t end_;  ///< Last port of the range.
    std::vector<bool> in_use_;  ///< Per-port allocation state (indexed by port - start).
    std::uint32_t next_hint_;  ///< Smallest candidate for the next allocation.
  };

  /**
   * @brief Look up the RTSP port currently assigned to a session group.
   *
   * Populated by the RTSP layer when per-group-port mode binds each group's
   * acceptor; returns nullopt for unknown groups or single-port mode.
   *
   * @param group_name Session group name.
   * @return Assigned port, or nullopt when the group has no dedicated port.
   */
  std::optional<std::uint16_t> port_for_group(const std::string &group_name);

  /**
   * @brief Register the RTSP port assigned to a session group.
   *
   * Called by the RTSP layer when per-group-port mode binds a group's
   * acceptor; the mapping is used to build the client-facing RTSP URL.
   *
   * @param group_name Session group name.
   * @param port Assigned RTSP port.
   */
  void register_group_port(const std::string &group_name, std::uint16_t port);

  /**
   * @brief Clear all registered group-to-port mappings.
   */
  void clear_group_ports();

  /**
   * @brief Runtime global holding the active session groups.
   *
   * Populated from the session groups file and/or CLI options during
   * `config::parse`. Consumed by the capture orchestration layer.
   */
  extern groups_config_t active_groups;

  /**
   * @brief Seed the runtime group registry from parsed configuration.
   *
   * The configuration file is treated as a preset template: parsing yields a
   * set of groups/sessions that are registered here through the same entry
   * point the runtime control plane will use. The registry is the single source
   * of truth for the capture and routing layers.
   *
   * @param groups Parsed configuration to register.
   */
  void seed_active_groups(groups_config_t groups);

  /**
   * @brief Runtime-mutable window capture state for a single streaming session.
   *
   * Populated through the runtime `attach` / `detach` / `filter` commands
   * (forwarded over the control IPC channel from a short-lived CLI process).
   * The capture thread reads a snapshot on every refresh so changes take
   * effect on the next captured frame.
   */
  struct session_runtime_t {
    std::unordered_set<std::uintptr_t> manual_attach;  ///< Windows force-included by `attach <hwnd>`.
    std::unordered_set<std::uintptr_t> manual_detach;  ///< Windows force-excluded by `detach <hwnd>`.
    std::vector<std::string> extra_exclude;  ///< Extra window classes excluded by `filter`, beyond `aux_exclude`.
  };

  /**
   * @brief Force-include a window in a session's composition at runtime.
   *
   * @param session Session identifier (client name).
   * @param hwnd Window handle to include.
   */
  void session_runtime_attach(const std::string &session, std::uintptr_t hwnd);

  /**
   * @brief Force-exclude a window from a session's composition at runtime.
   *
   * @param session Session identifier (client name).
   * @param hwnd Window handle to exclude.
   */
  void session_runtime_detach(const std::string &session, std::uintptr_t hwnd);

  /**
   * @brief Add a window class to a session's runtime exclude list.
   *
   * @param session Session identifier (client name).
   * @param window_class Window class name to exclude.
   */
  void session_runtime_add_exclude(const std::string &session, const std::string &window_class);

  /**
   * @brief Remove a window class from a session's runtime exclude list.
   *
   * @param session Session identifier (client name).
   * @param window_class Window class name to re-admit.
   */
  void session_runtime_remove_exclude(const std::string &session, const std::string &window_class);

  /**
   * @brief Snapshot a session's runtime window capture state.
   *
   * @param session Session identifier (client name).
   * @return Snapshot of the session's runtime state.
   */
  session_runtime_t session_runtime_snapshot(const std::string &session);

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
   * Checks that `port_mode` is explicitly set and valid, that
   * `per-group-port` defines a parseable `port_range` large enough for the
   * number of groups, that group names are unique and non-empty, that the
   * capture backend is supported, and that `window` groups define at least
   * one matching rule (or a group-level hwnd).
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
    std::string port_mode;  ///< Value of `--port-mode`.
    std::string port_range;  ///< Value of `--port-range`.
    std::string default_group;  ///< Value of `--default-group`.
    std::optional<std::filesystem::path> config_file;  ///< Value of `--config`.
  };

  /**
   * @brief Check whether a long option name is a session group CLI option.
   *
   * @param name Option name without the leading `--`.
   * @return True for `group`, `capture`, `box`, `process`, `title`, `class`, `port`, `port-mode`, `port-range`, `default-group`, or `config`.
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

  /**
   * @brief Resolve the session group a launch session belongs to.
   *
   * An explicit group name is honored when it names a configured window
   * group. Without one, the configured default group is used when set and
   * valid; otherwise the single window group (if any) is used. When none
   * applies the result is empty so the launch session stays ungrouped.
   *
   * @param requested Explicit group name from the launch request; may be empty.
   * @return Resolved window group name, or empty when none applies.
   */
  std::string resolve_launch_group(const std::string &requested);

  /**
   * @brief Get the first session of a group, if any.
   *
   * Placeholder used by the capture layer until session-id routing (R3) lands:
   * the capture backend still resolves a single session per group.
   *
   * @param group Group to inspect.
   * @return Pointer to the first session, or nullptr when the group has none.
   */
  const session_config_t *first_session(const config_t &group);

  /**
   * @brief Match a single top-level window against a session's rules.
   *
   * The window belongs to the session when any rule matches it (OR semantics).
   * A rule matches when the window handle equals the rule's hwnd, or the
   * process name matches `process`, or the window title matches `title`, or
   * the window class matches `class`. The `box` field is accepted for
   * configuration compatibility but is intentionally not evaluated here.
   *
   * @param session Session rules to test against.
   * @param hwnd Window handle to evaluate.
   * @return True when the window belongs to the session.
   */
  bool match_window(const session_config_t &session, std::uintptr_t hwnd);

  /**
   * @brief Find the best matching HWND for a session.
   *
   * An explicit session-level or rule-level hwnd wins immediately. Otherwise
   * visible top-level windows are enumerated: windows whose class appears in
   * `aux_exclude` are skipped, and among the remaining matches the window with
   * the largest area (the main window) is returned. Returns 0 when no window
   * matches.
   *
   * @param session Session to resolve.
   * @return Matching HWND, or 0 when none matches.
   */
  std::uintptr_t match_window_hwnd(const session_config_t &session);

}  // namespace session_group
