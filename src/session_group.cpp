/**
 * @file src/session_group.cpp
 * @brief Definitions for multi-session window capture groups.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
#endif

// lib includes
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "logging.h"
#include "session_group.h"
#include "utility.h"

using namespace std::literals;

namespace session_group {
  namespace pt = boost::property_tree;

  /**
   * @brief Runtime global holding the active session groups.
   */
  groups_config_t active_groups;

  std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_port_range(const std::string &range) {
    auto dash = range.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= range.size()) {
      return std::nullopt;
    }

    auto to_port = [](const std::string &text) -> std::optional<std::uint16_t> {
      if (text.empty()) {
        return std::nullopt;
      }
      try {
        std::size_t pos = 0;
        auto parsed = std::stoul(text, &pos, 10);
        if (pos != text.size() || parsed == 0 || parsed > 65535) {
          return std::nullopt;
        }
        return static_cast<std::uint16_t>(parsed);
      } catch (const std::exception &) {
        return std::nullopt;
      }
    };

    auto start = to_port(range.substr(0, dash));
    auto end = to_port(range.substr(dash + 1));
    if (!start || !end || *start > *end) {
      return std::nullopt;
    }
    return std::pair {*start, *end};
  }

  namespace {
    /**
     * @brief Check whether a port range overlaps any reserved Sunshine service port.
     *
     * The standard GameStream services own fixed ports regardless of session
     * groups: HTTP 47989, HTTPS 47984, Web UI HTTPS 47990, and RTSP 48010
     * (the default listener is always bound to `RTSP_SETUP_PORT`). Allocating a
     * group port from one of these would bind a second acceptor onto the same
     * port (allowed by SO_REUSEADDR on Windows), causing incoming RTSP
     * connections to be delivered to the wrong listener.
     *
     * @param bounds Inclusive port range bounds.
     * @return The first reserved port inside the range, or nullopt when the
     * range avoids all reserved ports.
     */
    std::optional<std::uint16_t> port_range_overlaps_reserved(const std::pair<std::uint16_t, std::uint16_t> &bounds) {
      static constexpr std::array<std::uint16_t, 4> reserved { 47984, 47989, 47990, 48010 };
      for (std::uint16_t port = bounds.first;; ++port) {
        if (std::ranges::find(reserved, port) != reserved.end()) {
          return port;
        }
        if (port == bounds.second) {
          break;
        }
      }
      return std::nullopt;
    }
  }  // namespace

  port_allocator_t::port_allocator_t(std::uint16_t start, std::uint16_t end):
      start_ {start},
      end_ {end},
      in_use_(static_cast<std::size_t>(end - start) + 1, false),
      next_hint_ {start} {}

  std::optional<std::uint16_t> port_allocator_t::allocate() {
    if (next_hint_ > end_) {
      return std::nullopt;
    }
    for (auto port = next_hint_; port <= end_; ++port) {
      if (!in_use_[static_cast<std::size_t>(port) - start_]) {
        in_use_[static_cast<std::size_t>(port) - start_] = true;
        next_hint_ = port + 1;
        return static_cast<std::uint16_t>(port);
      }
    }
    return std::nullopt;
  }

  void port_allocator_t::release(std::uint16_t port) {
    if (port < start_ || port > end_) {
      return;
    }
    auto index = static_cast<std::size_t>(port) - start_;
    if (in_use_[index]) {
      in_use_[index] = false;
      if (port < next_hint_) {
        next_hint_ = port;
      }
    }
  }

  std::size_t port_allocator_t::available() const {
    return static_cast<std::size_t>(std::count(in_use_.begin(), in_use_.end(), false));
  }

  std::size_t port_allocator_t::size() const {
    return in_use_.size();
  }

  namespace {
    std::mutex group_port_mutex;  ///< Guards the group-to-port mapping.
    std::unordered_map<std::string, std::uint16_t> group_ports;  ///< Group name to assigned RTSP port.
  }  // namespace

  std::optional<std::uint16_t> port_for_group(const std::string &group_name) {
    std::scoped_lock lock {group_port_mutex};
    auto it = group_ports.find(group_name);
    if (it == group_ports.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /**
   * @brief Register the RTSP port assigned to a session group.
   *
   * @param group_name Session group name.
   * @param port Assigned RTSP port.
   */
  void register_group_port(const std::string &group_name, std::uint16_t port) {
    std::scoped_lock lock {group_port_mutex};
    group_ports[group_name] = port;
  }

  /**
   * @brief Clear all registered group-to-port mappings.
   */
  void clear_group_ports() {
    std::scoped_lock lock {group_port_mutex};
    group_ports.clear();
  }

  /**
   * @brief Parse a window handle string into a numeric HWND value.
   *
   * Accepts decimal ("1234") or hexadecimal ("0x4D2") forms. An empty string
   * yields zero.
   *
   * @param value Raw handle text from configuration.
   * @return Parsed HWND value, or 0 when the text is empty or malformed.
   */
  std::uintptr_t parse_hwnd(const std::string &value) {
    if (value.empty()) {
      return 0;
    }
    try {
      std::size_t pos = 0;
      auto parsed = std::stoull(value, &pos, 0);
      if (pos != value.size()) {
        return 0;
      }
      return static_cast<std::uintptr_t>(parsed);
    } catch (const std::exception &) {
      return 0;
    }
  }

  std::optional<groups_config_t> parse_groups(const std::string_view &json_text) {
    pt::ptree root;
    std::istringstream stream {std::string {json_text}};
    try {
      pt::read_json(stream, root);
    } catch (const pt::json_parser_error &e) {
      BOOST_LOG(error) << "session_group: failed to parse session groups JSON: "sv << e.what();
      return std::nullopt;
    }

    groups_config_t result;
    result.port_mode = root.get<std::string>("port_mode", "");
    result.port_range = root.get<std::string>("port_range", "");
    result.default_group = root.get<std::string>("default_group", "");

    auto groups_node = root.get_child_optional("session_groups");
    if (!groups_node) {
      BOOST_LOG(warning) << "session_group: session groups JSON is missing the \"session_groups\" key"sv;
      return result;
    }
    for (auto &[_, group_node] : *groups_node) {
      config_t group;
      group.name = group_node.get<std::string>("name", "");
      group.capture = group_node.get<std::string>("capture", std::string {CAPTURE_WINDOW});
      group.port = static_cast<std::uint16_t>(group_node.get<int>("port", 0));
      group.hwnd = parse_hwnd(group_node.get<std::string>("hwnd", ""));
      group.max_fps = group_node.get<int>("max_fps", 60);
      group.bitrate_kbps = group_node.get<int>("bitrate_kbps", 0);

      if (auto rules_node = group_node.get_child_optional("rules")) {
        for (auto &[_, rule_node] : *rules_node) {
          window_rule_t rule;
          rule.box = rule_node.get<std::string>("box", "");
          rule.process = rule_node.get<std::string>("process", "");
          rule.title = rule_node.get<std::string>("title", "");
          rule.window_class = rule_node.get<std::string>("class", "");
          rule.hwnd = parse_hwnd(rule_node.get<std::string>("hwnd", ""));
          if (!rule.empty()) {
            group.rules.emplace_back(std::move(rule));
          }
        }
      }

      if (auto aux_node = group_node.get_child_optional("aux_exclude")) {
        for (auto &[_, aux] : *aux_node) {
          group.aux_exclude.emplace_back(aux.data());
        }
      }

      result.groups.emplace_back(std::move(group));
    }

    return result;
  }

  std::optional<groups_config_t> load_groups(const std::filesystem::path &path) {
    std::ifstream file {path};
    if (!file.is_open()) {
      BOOST_LOG(error) << "session_group: failed to open session groups file: "sv << path.string();
      return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_groups(buffer.str());
  }

  std::vector<std::string> validate_groups(const groups_config_t &groups) {
    std::vector<std::string> errors;
    std::unordered_set<std::string> names;

    if (groups.port_mode.empty()) {
      errors.emplace_back("port_mode must be explicitly set to 'single-port' or 'per-group-port'");
    } else if (groups.port_mode != PORT_MODE_SINGLE && groups.port_mode != PORT_MODE_PER_GROUP) {
      errors.emplace_back("invalid port_mode '" + groups.port_mode + "' (expected 'single-port' or 'per-group-port')");
    }

    if (groups.port_mode == PORT_MODE_PER_GROUP) {
      auto bounds = parse_port_range(groups.port_range);
      if (groups.port_range.empty()) {
        errors.emplace_back("port_mode 'per-group-port' requires a non-empty port_range (e.g. \"48100-48110\")");
      } else if (!bounds) {
        errors.emplace_back("invalid port_range '" + groups.port_range + "' (expected \"start-end\" with 1 <= start <= end <= 65535)");
      } else if (groups.groups.size() > static_cast<std::size_t>(bounds->second - bounds->first) + 1) {
        errors.emplace_back("too many session groups (" + std::to_string(groups.groups.size()) + ") for port_range '" + groups.port_range + "'; the range size limits the maximum number of groups");
      } else if (auto reserved = port_range_overlaps_reserved(*bounds)) {
        errors.emplace_back("port_range '" + groups.port_range + "' includes reserved Sunshine port " + std::to_string(*reserved) + "; ports 47984/47989/47990/48010 are used by the HTTP/HTTPS/Web UI/RTSP services and must not be allocated to session groups");
      }
    }

    if (!groups.default_group.empty()) {
      const bool found = std::ranges::any_of(groups.groups, [&](const auto &group) {
        return group.is_window_capture() && group.name == groups.default_group;
      });
      if (!found) {
        errors.emplace_back("default_group '" + groups.default_group + "' does not match any window capture group");
      }
    }

    for (const auto &group : groups.groups) {
      if (group.name.empty()) {
        errors.emplace_back("session group name must not be empty");
      } else if (!names.insert(group.name).second) {
        errors.emplace_back("duplicate session group name: " + group.name);
      }

      if (group.capture != CAPTURE_WINDOW && group.capture != CAPTURE_MONITOR) {
        errors.emplace_back("invalid capture backend for group '" + group.name + "': " + group.capture);
      }

      if (group.capture == CAPTURE_WINDOW && group.hwnd == 0 && group.rules.empty()) {
        errors.emplace_back("window capture group '" + group.name + "' must define at least one matching rule or a group-level hwnd");
      }
    }

    return errors;
  }

  bool is_cli_option(const std::string_view &name) {
    return name == "group" || name == "capture" || name == "box" || name == "process" || name == "title" || name == "class" || name == "hwnd" || name == "port" || name == "port-mode" || name == "port-range" || name == "default-group" || name == "config";
  }

  bool apply_cli_option(const std::string_view &name, const std::string_view &value, cli_options_t &opts) {
    if (name == "group") {
      opts.group_name = std::string {value};
    } else if (name == "capture") {
      opts.capture = std::string {value};
    } else if (name == "box") {
      opts.box = std::string {value};
    } else if (name == "process") {
      opts.process = std::string {value};
    } else if (name == "title") {
      opts.title = std::string {value};
    } else if (name == "class") {
      opts.window_class = std::string {value};
    } else if (name == "hwnd") {
      opts.hwnd = parse_hwnd(std::string {value});
    } else if (name == "port") {
      try {
        auto parsed = std::stoi(std::string {value});
        if (parsed <= 0 || parsed > 65535) {
          BOOST_LOG(error) << "session_group: invalid --port value: "sv << value;
          return false;
        }
        opts.port = static_cast<std::uint16_t>(parsed);
      } catch (const std::exception &) {
        BOOST_LOG(error) << "session_group: invalid --port value: "sv << value;
        return false;
      }
    } else if (name == "port-mode") {
      opts.port_mode = std::string {value};
    } else if (name == "port-range") {
      opts.port_range = std::string {value};
    } else if (name == "default-group") {
      opts.default_group = std::string {value};
    } else if (name == "config") {
      opts.config_file = std::filesystem::path {std::string {value}};
    } else {
      return false;
    }

    return true;
  }

  std::optional<groups_config_t> groups_from_cli(const cli_options_t &opts) {
    if (opts.config_file) {
      auto groups = load_groups(*opts.config_file);
      if (!groups) {
        return std::nullopt;
      }
      // Command-line options override the config file.
      if (!opts.port_mode.empty()) {
        groups->port_mode = opts.port_mode;
      }
      if (!opts.port_range.empty()) {
        groups->port_range = opts.port_range;
      }
      if (!opts.default_group.empty()) {
        groups->default_group = opts.default_group;
      }
      return groups;
    }

    const bool has_group_option = !opts.group_name.empty() || !opts.capture.empty() || !opts.box.empty() || !opts.process.empty() || !opts.title.empty() || !opts.window_class.empty() || opts.hwnd != 0 || opts.port.has_value();
    if (!has_group_option && opts.port_mode.empty() && opts.port_range.empty() && opts.default_group.empty()) {
      return groups_config_t {};
    }

    if (has_group_option && opts.group_name.empty()) {
      BOOST_LOG(error) << "session_group: --group is required when specifying a session group on the command line"sv;
      return std::nullopt;
    }

    groups_config_t result;
    result.port_mode = opts.port_mode;
    result.port_range = opts.port_range;
    result.default_group = opts.default_group;

    if (has_group_option) {
      config_t group;
      group.name = opts.group_name;
      group.capture = opts.capture.empty() ? std::string {CAPTURE_WINDOW} : opts.capture;
      group.port = opts.port.value_or(0);
      group.hwnd = opts.hwnd;

      window_rule_t rule;
      rule.box = opts.box;
      rule.process = opts.process;
      rule.title = opts.title;
      rule.window_class = opts.window_class;
      rule.hwnd = opts.hwnd;
      if (!rule.empty()) {
        group.rules.emplace_back(std::move(rule));
      }

      result.groups.emplace_back(std::move(group));
    }
    return result;
  }

  std::string resolve_active_window_group() {
    std::string active;
    for (const auto &group : active_groups.groups) {
      if (!group.is_window_capture()) {
        continue;
      }
      if (!active.empty()) {
        // Multiple window groups: routing needs per-port dispatch, which is
        // not wired up yet, so fall back to the legacy shared capture.
        return {};
      }
      active = group.name;
    }
    return active;
  }

  std::string resolve_launch_group(const std::string &requested) {
    if (!requested.empty()) {
      for (const auto &group : active_groups.groups) {
        if (group.is_window_capture() && group.name == requested) {
          return requested;
        }
      }
      return {};
    }

    // A configured default group receives sessions without an explicit group.
    if (!active_groups.default_group.empty()) {
      for (const auto &group : active_groups.groups) {
        if (group.is_window_capture() && group.name == active_groups.default_group) {
          return active_groups.default_group;
        }
      }
    }

    // Without a default, a lone window group captures the session.
    return resolve_active_window_group();
  }

#ifdef _WIN32

  /**
   * @brief Convert a wide string to UTF-8 with ASCII-only lowercasing.
   *
   * Non-ASCII code points are preserved verbatim (UTF-8 encoded) rather than
   * truncated to a single byte, so Chinese/Japanese titles and class names
   * survive the comparison pipeline. Only ASCII letters are lowercased.
   *
   * @param wide Wide string to convert.
   * @return UTF-8 string with ASCII letters lowercased; empty on conversion failure.
   */
  std::string to_utf8_lower(const std::wstring &wide) {
    if (wide.empty()) {
      return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
      return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    for (auto &c : result) {
      if (static_cast<unsigned char>(c) < 0x80) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    return result;
  }

  /**
   * @brief Lowercase ASCII characters in a UTF-8 string in place.
   *
   * Used to normalize configured class names (e.g. `aux_exclude` entries)
   * before comparing them against the lowercased window class name.
   *
   * @param s String to normalize.
   * @return The normalized string.
   */
  std::string to_ascii_lower(std::string s) {
    for (auto &c : s) {
      if (static_cast<unsigned char>(c) < 0x80) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    return s;
  }

  /**
   * @brief Return the ASCII-lowercased UTF-8 executable name (basename) of a window's process.
   *
   * @param hwnd Window handle.
   * @return Lowercased process executable name, or empty when unavailable.
   */
  std::string window_process_name(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
      return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
      return {};
    }

    auto close = util::fail_guard([&]() {
      CloseHandle(process);
    });

    std::wstring image_path(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(image_path.size());
    if (!QueryFullProcessImageNameW(process, 0, image_path.data(), &size)) {
      return {};
    }
    image_path.resize(size);

    auto name = std::filesystem::path {image_path}.filename().wstring();
    return to_utf8_lower(name);
  }

  /**
   * @brief Return the lowercased class name of a window.
   *
   * @param hwnd Window handle.
   * @return Lowercased class name, or empty when unavailable.
   */
  std::string window_class_name(HWND hwnd) {
    std::wstring class_name(256, L'\0');
    int len = GetClassNameW(hwnd, class_name.data(), static_cast<int>(class_name.size()));
    if (len == 0) {
      return {};
    }
    class_name.resize(static_cast<std::size_t>(len));

    return to_utf8_lower(class_name);
  }

  /**
   * @brief Return the lowercased window title.
   *
   * @param hwnd Window handle.
   * @return Lowercased window title, or empty when unavailable.
   */
  std::string window_title(HWND hwnd) {
    auto length = GetWindowTextLengthW(hwnd);
    if (length == 0) {
      return {};
    }

    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<std::size_t>(length));

    return to_utf8_lower(title);
  }

  /**
   * @brief Check whether a window title contains a search term (fuzzy).
   *
   * Substring, space-stripped substring, and word-based matches are tried,
   * mirroring the window-title matching approach used by the reference
   * implementation.
   *
   * @param title Lowercased window title.
   * @param search Lowercased search term.
   * @return True when the title matches the search term.
   */
  bool title_matches(const std::string &title, const std::string &search) {
    if (search.empty()) {
      return false;
    }
    if (title.find(search) != std::string::npos) {
      return true;
    }

    auto strip_spaces = [](std::string s) {
      s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
      return s;
    };

    if (strip_spaces(title).find(strip_spaces(search)) != std::string::npos) {
      return true;
    }

    // Word-based: every search word (length >= 2) must appear in the title.
    std::istringstream stream {search};
    std::string word;
    bool any_word = false;
    while (stream >> word) {
      if (word.size() < 2) {
        continue;
      }
      any_word = true;
      if (title.find(word) == std::string::npos) {
        return false;
      }
    }
    return any_word;
  }

  /**
   * @brief Test whether a single rule matches a window.
   *
   * A rule matches when the window handle equals the rule's hwnd, or the
   * process name matches, or the title matches, or the class matches. The
   * box field is accepted for compatibility but is intentionally ignored.
   *
   * @param rule Rule to test.
   * @param hwnd Window handle.
   * @param process_name Cached process name of the window.
   * @param title Cached window title.
   * @param class_name Cached class name of the window.
   * @return True when the rule matches.
   */
  bool rule_matches(const window_rule_t &rule, std::uintptr_t hwnd, const std::string &process_name, const std::string &title, const std::string &class_name) {
    if (rule.hwnd != 0 && rule.hwnd == hwnd) {
      return true;
    }
    if (!rule.process.empty() && rule.process == process_name) {
      return true;
    }
    if (!rule.title.empty() && title_matches(title, rule.title)) {
      return true;
    }
    if (!rule.window_class.empty() && rule.window_class == class_name) {
      return true;
    }
    return false;
  }

#endif  // _WIN32

  bool match_window(const config_t &group, std::uintptr_t hwnd) {
#ifdef _WIN32
    if (!group.is_window_capture()) {
      return false;
    }

    auto process_name = window_process_name(reinterpret_cast<HWND>(hwnd));
    auto title = window_title(reinterpret_cast<HWND>(hwnd));
    auto class_name = window_class_name(reinterpret_cast<HWND>(hwnd));

    for (const auto &rule : group.rules) {
      if (rule_matches(rule, hwnd, process_name, title, class_name)) {
        return true;
      }
    }
#endif  // _WIN32
    return false;
  }

  std::uintptr_t match_window_hwnd(const config_t &group) {
#ifdef _WIN32
    if (!group.is_window_capture()) {
      return 0;
    }

    // An explicit handle wins immediately.
    if (group.hwnd != 0) {
      return group.hwnd;
    }
    for (const auto &rule : group.rules) {
      if (rule.hwnd != 0) {
        return rule.hwnd;
      }
    }

    // Otherwise enumerate visible top-level windows. Auxiliary window classes
    // listed in `aux_exclude` are skipped, and the largest matching window is
    // preferred so that a small popup or menu never wins over the main window.
    struct enum_ctx_t {
      const config_t *group;  ///< Group rules to match against.
      std::vector<std::string> excluded;  ///< Lowercased aux_exclude class names.
      std::uintptr_t best;  ///< Largest matching HWND found so far.
      long best_area;  ///< Pixel area of the best match.
    };
    std::vector<std::string> excluded;
    excluded.reserve(group.aux_exclude.size());
    for (const auto &aux : group.aux_exclude) {
      excluded.push_back(to_ascii_lower(aux));
    }
    enum_ctx_t ctx {&group, std::move(excluded), 0, 0};

    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *data = reinterpret_cast<enum_ctx_t *>(lparam);

      if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
      }

      auto process_name = window_process_name(hwnd);
      auto title = window_title(hwnd);
      auto class_name = window_class_name(hwnd);

      if (std::find(data->excluded.begin(), data->excluded.end(), class_name) != data->excluded.end()) {
        return TRUE;
      }

      for (const auto &rule : data->group->rules) {
        if (rule_matches(rule, reinterpret_cast<std::uintptr_t>(hwnd), process_name, title, class_name)) {
          RECT rect {};
          GetWindowRect(hwnd, &rect);
          long area = (static_cast<long>(rect.right) - static_cast<long>(rect.left)) *
            (static_cast<long>(rect.bottom) - static_cast<long>(rect.top));
          if (area > data->best_area) {
            data->best_area = area;
            data->best = reinterpret_cast<std::uintptr_t>(hwnd);
          }
          break;
        }
      }
      return TRUE;
    },
      reinterpret_cast<LPARAM>(&ctx));

    return ctx.best;
#else
    (void) group;
    return 0;
#endif  // _WIN32
  }

}  // namespace session_group
