/**
 * @file tests/unit/test_session_group.cpp
 * @brief Tests for the session groups configuration and CLI parsing.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

// local includes
#include <src/session_group.h>

using namespace std::literals;

namespace {

  /**
   * @brief Session group config fixture providing valid JSON documents.
   */
  class SessionGroupTest: public testing::Test {
  protected:
    /**
     * @brief Get a valid two-group JSON document for parsing tests.
     *
     * @return Valid session groups JSON text.
     */
    std::string_view valid_groups_json() const {
      return R"({
        "port_mode": "per-group-port",
        "port_range": "48010-48100",
        "session_groups": [
          {
            "name": "user1-notepad",
            "capture": "window",
            "port": 48010,
            "rules": [
              {"box": "cap_u1_notepad"},
              {"process": "notepad.exe"}
            ],
            "aux_exclude": ["tooltips_class32", "IME"],
            "max_fps": 60,
            "bitrate_kbps": 20000
          },
          {
            "name": "user2-chrome",
            "capture": "window",
            "port": 48011,
            "rules": [
              {"process": "chrome.exe"}
            ]
          }
        ]
      })"sv;
    }
  };

}  // namespace

TEST_F(SessionGroupTest, ParsesValidGroupConfiguration) {
  auto groups = session_group::parse_groups(valid_groups_json());
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 2);
  EXPECT_EQ(groups->port_mode, session_group::PORT_MODE_PER_GROUP);
  EXPECT_EQ(groups->port_range, "48010-48100");

  const auto &first = groups->groups[0];
  EXPECT_EQ(first.name, "user1-notepad");
  EXPECT_EQ(first.capture, session_group::CAPTURE_WINDOW);
  EXPECT_EQ(first.port, 48010);
  EXPECT_EQ(first.rules.size(), 2);
  EXPECT_EQ(first.rules[0].box, "cap_u1_notepad");
  EXPECT_EQ(first.rules[1].process, "notepad.exe");
  ASSERT_EQ(first.aux_exclude.size(), 2);
  EXPECT_EQ(first.aux_exclude[0], "tooltips_class32");
  EXPECT_EQ(first.max_fps, 60);
  EXPECT_EQ(first.bitrate_kbps, 20000);

  const auto &second = groups->groups[1];
  EXPECT_EQ(second.name, "user2-chrome");
  EXPECT_EQ(second.port, 48011);
  EXPECT_EQ(second.rules.size(), 1);
}

TEST(SessionGroupParseHwndTest, ParsesDecimalAndHexadecimalHandles) {
  EXPECT_EQ(session_group::parse_hwnd(""), 0);
  EXPECT_EQ(session_group::parse_hwnd("0"), 0);
  EXPECT_EQ(session_group::parse_hwnd("1234"), 1234);
  EXPECT_EQ(session_group::parse_hwnd("0x4D2"), 0x4D2);
  EXPECT_EQ(session_group::parse_hwnd("not-a-handle"), 0);
  EXPECT_EQ(session_group::parse_hwnd("1234junk"), 0);
}

TEST_F(SessionGroupTest, ParsesGroupLevelHwnd) {
  auto groups = session_group::parse_groups(R"({
    "session_groups": [
      {"name": "hwnd-group", "capture": "window", "port": 48010, "hwnd": "0x4D2"}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 1);
  EXPECT_EQ(groups->groups[0].hwnd, 0x4D2);
  EXPECT_TRUE(groups->groups[0].is_window_capture());
}

TEST_F(SessionGroupTest, ParsesHwndRule) {
  auto groups = session_group::parse_groups(R"({
    "session_groups": [
      {"name": "hwnd-rule", "capture": "window", "port": 48010,
       "rules": [{"hwnd": "9999"}, {"process": "fallback.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 1);
  ASSERT_EQ(groups->groups[0].rules.size(), 2);
  EXPECT_EQ(groups->groups[0].rules[0].hwnd, 9999);
  EXPECT_EQ(groups->groups[0].rules[1].process, "fallback.exe");
}

TEST(SessionGroupHwndRuleTest, HwndRuleIsNotEmpty) {
  session_group::window_rule_t rule;
  rule.hwnd = 1234;
  EXPECT_FALSE(rule.empty());
}

TEST_F(SessionGroupTest, RejectsMalformedJson) {
  EXPECT_FALSE(session_group::parse_groups("this is not json"sv).has_value());
  EXPECT_FALSE(session_group::parse_groups(""sv).has_value());
}

TEST_F(SessionGroupTest, ReturnsEmptyGroupsWhenRootKeyMissing) {
  auto groups = session_group::parse_groups(R"({"foo": "bar"})"sv);
  ASSERT_TRUE(groups.has_value());
  EXPECT_TRUE(groups->groups.empty());
}

TEST_F(SessionGroupTest, AppliesDefaultsForMissingFields) {
  auto groups = session_group::parse_groups(R"({
    "session_groups": [
      {
        "name": "minimal",
        "port": 48010
      }
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 1);

  const auto &group = groups->groups[0];
  EXPECT_EQ(group.name, "minimal");
  EXPECT_EQ(group.capture, session_group::CAPTURE_WINDOW);
  EXPECT_EQ(group.max_fps, 60);
  EXPECT_EQ(group.bitrate_kbps, 0);
  EXPECT_TRUE(group.rules.empty());
  EXPECT_TRUE(group.aux_exclude.empty());
}

TEST_F(SessionGroupTest, SkipsEmptyRules) {
  auto groups = session_group::parse_groups(R"({
    "session_groups": [
      {
        "name": "empty-rule",
        "port": 48010,
        "rules": [
          {"box": "", "process": "", "title": "", "class": ""},
          {"box": "cap_box"}
        ]
      }
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 1);
  EXPECT_EQ(groups->groups[0].rules.size(), 1);
  EXPECT_EQ(groups->groups[0].rules[0].box, "cap_box");
}

TEST_F(SessionGroupTest, ValidatesUniqueNames) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "session_groups": [
      {"name": "dup", "capture": "monitor"},
      {"name": "dup", "capture": "monitor"}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_NE(std::find(errors.begin(), errors.end(), "duplicate session group name: dup"), errors.end());
}

TEST_F(SessionGroupTest, ValidatesWindowGroupRequiresRulesOrHwnd) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "session_groups": [
      {"name": "no-rules", "capture": "window"}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "window capture group 'no-rules' must define at least one matching rule or a group-level hwnd");

  auto with_hwnd = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "session_groups": [
      {"name": "hwnd-ok", "capture": "window", "hwnd": "0x4D2"}
    ]
  })"sv);
  ASSERT_TRUE(with_hwnd.has_value());
  EXPECT_TRUE(session_group::validate_groups(*with_hwnd).empty());

  auto with_rule = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "session_groups": [
      {"name": "rule-ok", "capture": "window", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(with_rule.has_value());
  EXPECT_TRUE(session_group::validate_groups(*with_rule).empty());
}

TEST_F(SessionGroupTest, ValidatesInvalidCaptureBackend) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "session_groups": [
      {"name": "bad-capture", "capture": "hologram", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "invalid capture backend for group 'bad-capture': hologram");
}

TEST_F(SessionGroupTest, ValidationRequiresExplicitPortMode) {
  auto groups = session_group::parse_groups(R"({
    "session_groups": [
      {"name": "no-mode", "capture": "window", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "port_mode must be explicitly set to 'single-port' or 'per-group-port'");
}

TEST_F(SessionGroupTest, ValidationRejectsUnknownPortMode) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "hybrid",
    "session_groups": [
      {"name": "bad-mode", "capture": "window", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "invalid port_mode 'hybrid' (expected 'single-port' or 'per-group-port')");
}

TEST_F(SessionGroupTest, PerGroupPortRequiresPortRange) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "per-group-port",
    "session_groups": [
      {"name": "no-range", "capture": "window", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "port_mode 'per-group-port' requires a non-empty port_range (e.g. \"48010-48100\")");
}

TEST_F(SessionGroupTest, PerGroupPortRejectsMalformedRange) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "per-group-port",
    "port_range": "48010",
    "session_groups": [
      {"name": "bad-range", "capture": "window", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "invalid port_range '48010' (expected \"start-end\" with 1 <= start <= end <= 65535)");
}

TEST_F(SessionGroupTest, PerGroupPortRejectsTooManyGroups) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "per-group-port",
    "port_range": "48010-48011",
    "session_groups": [
      {"name": "a", "capture": "window", "rules": [{"process": "a.exe"}]},
      {"name": "b", "capture": "window", "rules": [{"process": "b.exe"}]},
      {"name": "c", "capture": "window", "rules": [{"process": "c.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "too many session groups (3) for port_range '48010-48011'; the range size limits the maximum number of groups");
}

TEST_F(SessionGroupTest, ParsesDefaultGroup) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "default_group": "user1-notepad",
    "session_groups": [
      {"name": "user1-notepad", "capture": "window", "rules": [{"process": "notepad.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());
  EXPECT_EQ(groups->default_group, "user1-notepad");
  EXPECT_TRUE(session_group::validate_groups(*groups).empty());
}

TEST_F(SessionGroupTest, ValidationRejectsUnknownDefaultGroup) {
  auto groups = session_group::parse_groups(R"({
    "port_mode": "single-port",
    "default_group": "ghost",
    "session_groups": [
      {"name": "real", "capture": "window", "rules": [{"process": "x.exe"}]}
    ]
  })"sv);
  ASSERT_TRUE(groups.has_value());

  auto errors = session_group::validate_groups(*groups);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors[0], "default_group 'ghost' does not match any window capture group");
}

TEST_F(SessionGroupTest, ValidGroupsProduceNoErrors) {
  auto groups = session_group::parse_groups(valid_groups_json());
  ASSERT_TRUE(groups.has_value());
  EXPECT_TRUE(session_group::validate_groups(*groups).empty());
}

TEST(SessionGroupCliTest, RecognizesGroupOptions) {
  EXPECT_TRUE(session_group::is_cli_option("group"));
  EXPECT_TRUE(session_group::is_cli_option("capture"));
  EXPECT_TRUE(session_group::is_cli_option("box"));
  EXPECT_TRUE(session_group::is_cli_option("process"));
  EXPECT_TRUE(session_group::is_cli_option("title"));
  EXPECT_TRUE(session_group::is_cli_option("class"));
  EXPECT_TRUE(session_group::is_cli_option("hwnd"));
  EXPECT_TRUE(session_group::is_cli_option("port"));
  EXPECT_TRUE(session_group::is_cli_option("port-mode"));
  EXPECT_TRUE(session_group::is_cli_option("port-range"));
  EXPECT_TRUE(session_group::is_cli_option("default-group"));
  EXPECT_TRUE(session_group::is_cli_option("config"));
  EXPECT_FALSE(session_group::is_cli_option("help"));
  EXPECT_FALSE(session_group::is_cli_option("creds"));
}

TEST(SessionGroupCliTest, AppliesValidOptions) {
  session_group::cli_options_t opts;
  EXPECT_TRUE(session_group::apply_cli_option("group", "user1-notepad", opts));
  EXPECT_TRUE(session_group::apply_cli_option("capture", "window", opts));
  EXPECT_TRUE(session_group::apply_cli_option("box", "cap_u1_notepad", opts));
  EXPECT_TRUE(session_group::apply_cli_option("process", "notepad.exe", opts));
  EXPECT_TRUE(session_group::apply_cli_option("title", "Notepad*", opts));
  EXPECT_TRUE(session_group::apply_cli_option("class", "#32770", opts));
  EXPECT_TRUE(session_group::apply_cli_option("hwnd", "0x4D2", opts));
  EXPECT_TRUE(session_group::apply_cli_option("port", "48010", opts));
  EXPECT_TRUE(session_group::apply_cli_option("port-mode", "per-group-port", opts));
  EXPECT_TRUE(session_group::apply_cli_option("port-range", "48010-48100", opts));
  EXPECT_TRUE(session_group::apply_cli_option("default-group", "user1-notepad", opts));
  EXPECT_TRUE(session_group::apply_cli_option("config", "session-groups.json", opts));

  EXPECT_EQ(opts.group_name, "user1-notepad");
  EXPECT_EQ(opts.capture, "window");
  EXPECT_EQ(opts.box, "cap_u1_notepad");
  EXPECT_EQ(opts.process, "notepad.exe");
  EXPECT_EQ(opts.title, "Notepad*");
  EXPECT_EQ(opts.window_class, "#32770");
  EXPECT_EQ(opts.hwnd, 0x4D2);
  ASSERT_TRUE(opts.port.has_value());
  EXPECT_EQ(*opts.port, 48010);
  EXPECT_EQ(opts.port_mode, "per-group-port");
  EXPECT_EQ(opts.port_range, "48010-48100");
  EXPECT_EQ(opts.default_group, "user1-notepad");
  ASSERT_TRUE(opts.config_file.has_value());
  EXPECT_EQ(opts.config_file->string(), "session-groups.json");
}

TEST(SessionGroupCliTest, RejectsInvalidPortValues) {
  session_group::cli_options_t opts;
  EXPECT_FALSE(session_group::apply_cli_option("port", "0", opts));
  EXPECT_FALSE(session_group::apply_cli_option("port", "65536", opts));
  EXPECT_FALSE(session_group::apply_cli_option("port", "not-a-number", opts));
  EXPECT_FALSE(opts.port.has_value());
}

TEST(SessionGroupCliTest, RejectsUnknownOptions) {
  session_group::cli_options_t opts;
  EXPECT_FALSE(session_group::apply_cli_option("unknown", "value", opts));
}

TEST(SessionGroupCliTest, BuildsSingleGroupFromOptions) {
  session_group::cli_options_t opts;
  opts.group_name = "user1-notepad";
  opts.capture = "window";
  opts.box = "cap_u1_notepad";
  opts.port = 48010;

  auto groups = session_group::groups_from_cli(opts);
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 1);

  const auto &group = groups->groups[0];
  EXPECT_EQ(group.name, "user1-notepad");
  EXPECT_EQ(group.capture, "window");
  EXPECT_EQ(group.port, 48010);
  ASSERT_EQ(group.rules.size(), 1);
  EXPECT_EQ(group.rules[0].box, "cap_u1_notepad");
}

TEST(SessionGroupCliTest, BuildsGroupWithHwnd) {
  session_group::cli_options_t opts;
  opts.group_name = "hwnd-group";
  opts.hwnd = 0x4D2;
  opts.port = 48010;

  auto groups = session_group::groups_from_cli(opts);
  ASSERT_TRUE(groups.has_value());
  ASSERT_EQ(groups->groups.size(), 1);

  const auto &group = groups->groups[0];
  EXPECT_EQ(group.name, "hwnd-group");
  EXPECT_EQ(group.hwnd, 0x4D2);
  ASSERT_EQ(group.rules.size(), 1);
  EXPECT_EQ(group.rules[0].hwnd, 0x4D2);
}

TEST(SessionGroupCliTest, RequiresGroupNameWhenOptionsPresent) {
  session_group::cli_options_t opts;
  opts.box = "cap_u1_notepad";

  EXPECT_FALSE(session_group::groups_from_cli(opts).has_value());
}

TEST(SessionGroupCliTest, ReturnsEmptyGroupsWithoutOptions) {
  session_group::cli_options_t opts;
  auto groups = session_group::groups_from_cli(opts);
  ASSERT_TRUE(groups.has_value());
  EXPECT_TRUE(groups->groups.empty());
}

TEST(SessionGroupCliTest, BuildsGroupsConfigWithPortModeFromOptions) {
  session_group::cli_options_t opts;
  opts.port_mode = "per-group-port";
  opts.port_range = "48010-48100";

  auto groups = session_group::groups_from_cli(opts);
  ASSERT_TRUE(groups.has_value());
  EXPECT_TRUE(groups->groups.empty());
  EXPECT_EQ(groups->port_mode, "per-group-port");
  EXPECT_EQ(groups->port_range, "48010-48100");
}

TEST(SessionGroupCliTest, CliOverridesConfigFilePortMode) {
  auto path = std::filesystem::temp_directory_path() / "sunshine_test_session_groups_override.json";
  {
    std::ofstream file {path};
    file << R"({
      "port_mode": "single-port",
      "session_groups": [
        {"name": "cfg-group", "capture": "window", "rules": [{"process": "x.exe"}]}
      ]
    })";
  }

  session_group::cli_options_t opts;
  opts.config_file = path;
  opts.port_mode = "per-group-port";
  opts.port_range = "48010-48100";
  opts.default_group = "cfg-group";

  auto groups = session_group::groups_from_cli(opts);
  std::filesystem::remove(path);
  ASSERT_TRUE(groups.has_value());
  EXPECT_EQ(groups->groups.size(), 1);
  EXPECT_EQ(groups->groups[0].name, "cfg-group");
  EXPECT_EQ(groups->port_mode, "per-group-port");
  EXPECT_EQ(groups->port_range, "48010-48100");
  EXPECT_EQ(groups->default_group, "cfg-group");
}

TEST(SessionGroupParsePortRangeTest, ParsesValidRanges) {
  auto range = session_group::parse_port_range("48010-48100");
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->first, 48010);
  EXPECT_EQ(range->second, 48100);

  auto single = session_group::parse_port_range("47989-47989");
  ASSERT_TRUE(single.has_value());
  EXPECT_EQ(single->first, 47989);
  EXPECT_EQ(single->second, 47989);
}

TEST(SessionGroupParsePortRangeTest, RejectsMalformedRanges) {
  EXPECT_FALSE(session_group::parse_port_range("").has_value());
  EXPECT_FALSE(session_group::parse_port_range("48010").has_value());
  EXPECT_FALSE(session_group::parse_port_range("48010-").has_value());
  EXPECT_FALSE(session_group::parse_port_range("-48100").has_value());
  EXPECT_FALSE(session_group::parse_port_range("48100-48010").has_value());
  EXPECT_FALSE(session_group::parse_port_range("0-48010").has_value());
  EXPECT_FALSE(session_group::parse_port_range("48010-65536").has_value());
  EXPECT_FALSE(session_group::parse_port_range("abc-def").has_value());
}

TEST(SessionGroupPortAllocatorTest, AllocatesSmallestFreePortFirst) {
  session_group::port_allocator_t allocator {48010, 48012};
  EXPECT_EQ(allocator.size(), 3);
  EXPECT_EQ(allocator.available(), 3);

  EXPECT_EQ(*allocator.allocate(), 48010);
  EXPECT_EQ(*allocator.allocate(), 48011);
  EXPECT_EQ(*allocator.allocate(), 48012);
  EXPECT_EQ(allocator.available(), 0);
  EXPECT_FALSE(allocator.allocate().has_value());
}

TEST(SessionGroupPortAllocatorTest, ReleasesAndReusesPorts) {
  session_group::port_allocator_t allocator {48010, 48012};
  ASSERT_EQ(*allocator.allocate(), 48010);
  ASSERT_EQ(*allocator.allocate(), 48011);

  allocator.release(48010);
  EXPECT_EQ(allocator.available(), 2);
  // The smallest free port is reused.
  EXPECT_EQ(*allocator.allocate(), 48010);
}

TEST(SessionGroupPortAllocatorTest, ReleaseOutOfRangeIsIgnored) {
  session_group::port_allocator_t allocator {48010, 48012};
  allocator.release(9999);
  EXPECT_EQ(allocator.available(), 3);
  EXPECT_EQ(*allocator.allocate(), 48010);
}

TEST(SessionGroupPortAllocatorTest, SinglePortRange) {
  session_group::port_allocator_t allocator {47989, 47989};
  EXPECT_EQ(allocator.size(), 1);
  EXPECT_EQ(*allocator.allocate(), 47989);
  EXPECT_FALSE(allocator.allocate().has_value());
}

TEST(SessionGroupPortAllocatorTest, HandlesUpperPortBound) {
  // next_hint_ must not overflow when the last port of the range is allocated.
  session_group::port_allocator_t allocator {65534, 65535};
  EXPECT_EQ(*allocator.allocate(), 65534);
  EXPECT_EQ(*allocator.allocate(), 65535);
  EXPECT_EQ(allocator.available(), 0);
  EXPECT_FALSE(allocator.allocate().has_value());

  allocator.release(65534);
  EXPECT_EQ(*allocator.allocate(), 65534);
}

TEST(SessionGroupPortForGroupTest, ReturnsRegisteredPort) {
  session_group::clear_group_ports();
  EXPECT_FALSE(session_group::port_for_group("user1").has_value());

  session_group::register_group_port("user1", 48010);
  EXPECT_EQ(*session_group::port_for_group("user1"), 48010);
  EXPECT_FALSE(session_group::port_for_group("unknown").has_value());

  session_group::clear_group_ports();
  EXPECT_FALSE(session_group::port_for_group("user1").has_value());
}

namespace {

  /**
   * @brief RAII guard that restores the active session groups afterwards.
   */
  class ActiveGroupsGuard {
  public:
    ActiveGroupsGuard():
        saved_ {session_group::active_groups} {}

    ~ActiveGroupsGuard() {
      session_group::active_groups = std::move(saved_);
    }

  private:
    session_group::groups_config_t saved_;  ///< Active groups restored on destruction.
  };

}  // namespace

TEST(SessionGroupResolveActiveTest, ReturnsNameForSingleWindowGroup) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"user1", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 20000});

  EXPECT_EQ(session_group::resolve_active_window_group(), "user1");
}

TEST(SessionGroupResolveActiveTest, ReturnsEmptyWhenNoWindowGroup) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"mon", std::string {session_group::CAPTURE_MONITOR}, 48010, 0, {}, {}, 60, 0});

  EXPECT_TRUE(session_group::resolve_active_window_group().empty());
}

TEST(SessionGroupResolveActiveTest, ReturnsEmptyForMultipleWindowGroups) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"a", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 0});
  session_group::active_groups.groups.emplace_back(session_group::config_t {"b", std::string {session_group::CAPTURE_WINDOW}, 48011, 0x4D3, {}, {}, 60, 0});

  EXPECT_TRUE(session_group::resolve_active_window_group().empty());
}

TEST(SessionGroupResolveLaunchTest, HonorsExplicitGroupName) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"a", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 0});

  EXPECT_EQ(session_group::resolve_launch_group("a"), "a");
  EXPECT_TRUE(session_group::resolve_launch_group("missing").empty());
}

TEST(SessionGroupResolveLaunchTest, FallsBackToSingleWindowGroup) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"user1", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 20000});

  EXPECT_EQ(session_group::resolve_launch_group(""), "user1");
}

TEST(SessionGroupResolveLaunchTest, ReturnsEmptyForMultipleGroupsWithoutExplicitName) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"a", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 0});
  session_group::active_groups.groups.emplace_back(session_group::config_t {"b", std::string {session_group::CAPTURE_WINDOW}, 48011, 0x4D3, {}, {}, 60, 0});

  EXPECT_TRUE(session_group::resolve_launch_group("").empty());
}

TEST(SessionGroupResolveLaunchTest, UsesConfiguredDefaultGroup) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.default_group = "b";
  session_group::active_groups.groups.emplace_back(session_group::config_t {"a", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 0});
  session_group::active_groups.groups.emplace_back(session_group::config_t {"b", std::string {session_group::CAPTURE_WINDOW}, 48011, 0x4D3, {}, {}, 60, 0});

  EXPECT_EQ(session_group::resolve_launch_group(""), "b");
  // An explicit group still wins over the default.
  EXPECT_EQ(session_group::resolve_launch_group("a"), "a");
}

TEST(SessionGroupResolveLaunchTest, IgnoresUnknownDefaultGroup) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.default_group = "ghost";
  session_group::active_groups.groups.emplace_back(session_group::config_t {"a", std::string {session_group::CAPTURE_WINDOW}, 48010, 0x4D2, {}, {}, 60, 0});

  // "ghost" is not a configured window group, so the lone group wins.
  EXPECT_EQ(session_group::resolve_launch_group(""), "a");
}

TEST(SessionGroupResolveLaunchTest, IgnoresNonWindowGroups) {
  ActiveGroupsGuard guard;
  session_group::active_groups = session_group::groups_config_t {};
  session_group::active_groups.groups.emplace_back(session_group::config_t {"mon", std::string {session_group::CAPTURE_MONITOR}, 48010, 0, {}, {}, 60, 0});

  EXPECT_TRUE(session_group::resolve_launch_group("").empty());
  EXPECT_TRUE(session_group::resolve_launch_group("mon").empty());
}

TEST(SessionGroupMatcherTest, GroupLevelHwndWinsImmediately) {
  session_group::config_t group;
  group.capture = std::string {session_group::CAPTURE_WINDOW};
  group.hwnd = 0x1234;

  EXPECT_EQ(session_group::match_window_hwnd(group), 0x1234);
}

TEST(SessionGroupMatcherTest, RuleLevelHwndWinsWhenGroupHasNone) {
  session_group::config_t group;
  group.capture = std::string {session_group::CAPTURE_WINDOW};

  session_group::window_rule_t rule;
  rule.hwnd = 0x5678;
  group.rules.emplace_back(rule);

  EXPECT_EQ(session_group::match_window_hwnd(group), 0x5678);
}

TEST(SessionGroupMatcherTest, ReturnsZeroForNonWindowGroup) {
  session_group::config_t group;
  group.capture = std::string {session_group::CAPTURE_MONITOR};
  group.hwnd = 0x1234;

  EXPECT_EQ(session_group::match_window_hwnd(group), 0);
}

TEST(SessionGroupMatcherTest, InvalidWindowNeverMatchesEmptyRules) {
  // An invalid handle cannot match a group whose rules carry no criteria.
  session_group::config_t group;
  group.capture = std::string {session_group::CAPTURE_WINDOW};

  EXPECT_FALSE(session_group::match_window(group, 0xDEADBEEF));
  EXPECT_FALSE(session_group::match_window(group, 0));
}
