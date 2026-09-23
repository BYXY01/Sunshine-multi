/**
 * @file tests/unit/test_session_group_window_matcher.cpp
 * @brief Tests for the window matcher against real Win32 windows (bug fixes).
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <string>

// local includes
#include <src/session_group.h>

#ifdef _WIN32
  #include <windows.h>

namespace {

  /**
   * @brief Lowercased executable basename of the test process.
   *
   * @return ASCII-lowercased test executable name.
   */
  std::string test_process_name() {
    wchar_t buf[MAX_PATH] {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path {buf, n};
    auto pos = path.find_last_of(L"\\/");
    std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    std::string result;
    result.reserve(name.size());
    for (auto wc : name) {
      result.push_back(wc < 0x80 ? static_cast<char>(std::tolower(static_cast<unsigned char>(wc))) : '?');
    }
    return result;
  }

  /**
   * @brief RAII helper owning a registered window class plus a visible,
   * off-screen top-level window.
   */
  class TestWindow {
  public:
    /**
     * @brief Register a unique class and create an off-screen visible window.
     *
     * @param class_name Class name to register (must be unique per test).
     * @param title Window title text.
     * @param width Client width.
     * @param height Client height.
     */
    TestWindow(const std::wstring &class_name, const std::wstring &title, int width, int height):
        class_name_ {class_name} {
      WNDCLASSW wc {};
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = class_name_.c_str();
      atom_ = RegisterClassW(&wc);
      hwnd_ = CreateWindowW(class_name_.c_str(), title.c_str(), WS_OVERLAPPED,
        -20000, -20000, width, height, nullptr, nullptr, wc.hInstance, nullptr);
      if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_SHOW);
      }
    }

    /**
     * @brief Destroy the window and unregister its class.
     */
    ~TestWindow() {
      if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
      }
      if (atom_ != 0) {
        UnregisterClassW(class_name_.c_str(), GetModuleHandleW(nullptr));
      }
    }

    TestWindow(const TestWindow &) = delete;
    TestWindow &operator=(const TestWindow &) = delete;

    /**
     * @brief Get the window handle.
     *
     * @return HWND value, or nullptr when creation failed.
     */
    HWND hwnd() const {
      return hwnd_;
    }

  private:
    std::wstring class_name_;  ///< Registered class name.
    ATOM atom_ {0};  ///< Class registration atom.
    HWND hwnd_ {nullptr};  ///< Created window.
  };

  /**
   * @brief Build a session with a single process rule matching the test
   * executable.
   *
   * @return Configured session.
   */
  session_group::session_config_t process_session() {
    session_group::session_config_t session;
    session.id = 1;
    session.name = "test";
    session_group::window_rule_t rule;
    rule.process = test_process_name();
    session.rules.push_back(std::move(rule));
    return session;
  }

}  // namespace
#endif  // _WIN32

#ifdef _WIN32

TEST(SessionGroupWindowMatcherTest, ChineseTitleRuleMatches) {
  // Regression for the bug where non-ASCII (CJK) titles were truncated by a
  // single-byte tolower(), making a "无标题" rule unable to match the window.
  TestWindow win {L"SunshineTestCJKWin", L"无标题 - Notepad", 400, 300};
  ASSERT_NE(win.hwnd(), nullptr);

  session_group::session_config_t session;
  session_group::window_rule_t rule;
  // "无标题" encoded as UTF-8.
  rule.title = "\xE6\x97\xA0\xE6\xA0\x87\xE9\xA2\x98";
  session.rules.push_back(std::move(rule));

  EXPECT_TRUE(session_group::match_window(session, reinterpret_cast<std::uintptr_t>(win.hwnd())));
}

TEST(SessionGroupWindowMatcherTest, AsciiTitleRuleIsCaseInsensitive) {
  TestWindow win {L"SunshineTestAsciiWin", L"My Game 1.0", 400, 300};
  ASSERT_NE(win.hwnd(), nullptr);

  session_group::session_config_t session;
  session_group::window_rule_t rule;
  rule.title = "my game 1.0";
  session.rules.push_back(std::move(rule));

  EXPECT_TRUE(session_group::match_window(session, reinterpret_cast<std::uintptr_t>(win.hwnd())));
}

TEST(SessionGroupWindowMatcherTest, ClassRuleMatches) {
  TestWindow win {L"SunshineTestClassWin", L"classy", 400, 300};
  ASSERT_NE(win.hwnd(), nullptr);

  session_group::session_config_t session;
  session_group::window_rule_t rule;
  rule.window_class = "sunshinetestclasswin";
  session.rules.push_back(std::move(rule));

  EXPECT_TRUE(session_group::match_window(session, reinterpret_cast<std::uintptr_t>(win.hwnd())));
}

TEST(SessionGroupWindowMatcherTest, MatchWindowHwndPrefersLargestWindow) {
  // Regression for the bug where the first enumerated matching window was
  // returned, which could be a small popup rather than the main window.
  TestWindow small {L"SunshineTestSmallWin", L"small", 120, 60};
  TestWindow large {L"SunshineTestLargeWin", L"large", 800, 600};
  ASSERT_NE(small.hwnd(), nullptr);
  ASSERT_NE(large.hwnd(), nullptr);

  EXPECT_EQ(session_group::match_window_hwnd(process_session()), reinterpret_cast<std::uintptr_t>(large.hwnd()));
}

TEST(SessionGroupWindowMatcherTest, MatchWindowHwndSkipsAuxExcludedWindows) {
  TestWindow main {L"SunshineTestMainWin", L"main", 800, 600};
  TestWindow popup {L"SunshineTestPopupWin", L"popup", 100, 50};
  ASSERT_NE(main.hwnd(), nullptr);
  ASSERT_NE(popup.hwnd(), nullptr);

  auto session = process_session();
  session.aux_exclude.emplace_back("SunshineTestPopupWin");  // mixed case is normalized
  EXPECT_EQ(session_group::match_window_hwnd(session), reinterpret_cast<std::uintptr_t>(main.hwnd()));
}

TEST(SessionGroupWindowMatcherTest, MatchWindowHwndReturnsZeroWhenAllExcluded) {
  TestWindow main {L"SunshineTestMainWin", L"main", 800, 600};
  TestWindow popup {L"SunshineTestPopupWin", L"popup", 100, 50};
  ASSERT_NE(main.hwnd(), nullptr);
  ASSERT_NE(popup.hwnd(), nullptr);

  auto session = process_session();
  session.aux_exclude.emplace_back("sunshinetestmainwin");
  session.aux_exclude.emplace_back("SunshineTestPopupWin");
  EXPECT_EQ(session_group::match_window_hwnd(session), 0);
}

#else  // _WIN32

TEST(SessionGroupWindowMatcherTest, MatchWindowIsFalseOnNonWindows) {
  session_group::session_config_t session;
  session_group::window_rule_t rule;
  rule.title = "anything";
  session.rules.push_back(std::move(rule));

  EXPECT_FALSE(session_group::match_window(session, 0x1234));
}

#endif  // _WIN32
