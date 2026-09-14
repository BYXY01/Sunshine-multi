/**
 * @file src/session_group.cpp
 * @brief Definitions for multi-session window capture groups.
 */
// standard includes
#include <fstream>
#include <sstream>
#include <unordered_set>

// lib includes
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "logging.h"
#include "session_group.h"

using namespace std::literals;

namespace session_group {
  namespace pt = boost::property_tree;

  /**
   * @brief Runtime global holding the active session groups.
   */
  groups_config_t active_groups;

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

    auto groups_node = root.get_child_optional("session_groups");
    if (!groups_node) {
      BOOST_LOG(warning) << "session_group: session groups JSON is missing the \"session_groups\" key"sv;
      return groups_config_t {};
    }

    groups_config_t result;
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

      if (auto aux_node = group_node.get_child_optional("aux_include")) {
        for (auto &[_, aux] : *aux_node) {
          group.aux_include.emplace_back(aux.data());
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
    std::unordered_set<std::uint16_t> ports;

    for (const auto &group : groups.groups) {
      if (group.name.empty()) {
        errors.emplace_back("session group name must not be empty");
      } else if (!names.insert(group.name).second) {
        errors.emplace_back("duplicate session group name: " + group.name);
      }

      if (group.capture != CAPTURE_WINDOW && group.capture != CAPTURE_MONITOR) {
        errors.emplace_back("invalid capture backend for group '" + group.name + "': " + group.capture);
      }

      if (group.port == 0) {
        errors.emplace_back("session group '" + group.name + "' must define a non-zero port");
      } else if (!ports.insert(group.port).second) {
        errors.emplace_back("duplicate port for session group '" + group.name + "': " + std::to_string(group.port));
      }

      if (group.capture == CAPTURE_WINDOW && group.hwnd == 0 && group.rules.empty()) {
        errors.emplace_back("window capture group '" + group.name + "' must define at least one matching rule or a group-level hwnd");
      }
    }

    return errors;
  }

  bool is_cli_option(const std::string_view &name) {
    return name == "group" || name == "capture" || name == "box" || name == "process" || name == "title" || name == "class" || name == "hwnd" || name == "port" || name == "config";
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
    } else if (name == "config") {
      opts.config_file = std::filesystem::path {std::string {value}};
    } else {
      return false;
    }

    return true;
  }

  std::optional<groups_config_t> groups_from_cli(const cli_options_t &opts) {
    if (opts.config_file) {
      return load_groups(*opts.config_file);
    }

    const bool has_group_option = !opts.group_name.empty() || !opts.capture.empty() || !opts.box.empty() || !opts.process.empty() || !opts.title.empty() || !opts.window_class.empty() || opts.hwnd != 0 || opts.port.has_value();
    if (!has_group_option) {
      return groups_config_t {};
    }

    if (opts.group_name.empty()) {
      BOOST_LOG(error) << "session_group: --group is required when specifying a session group on the command line"sv;
      return std::nullopt;
    }

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

    groups_config_t result;
    result.groups.emplace_back(std::move(group));
    return result;
  }

}  // namespace session_group
