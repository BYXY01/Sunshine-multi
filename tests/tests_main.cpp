/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */

// test includes
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

// standard includes
#include <cstdlib>
#include <string_view>

using namespace std::literals;

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);

  // Consume custom test arguments (gtest leaves unknown args in argv).
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == "--window-hwnd"sv && i + 1 < argc) {
      test_args::window_hwnd = std::stoull(argv[++i], nullptr, 0);
    }
  }

  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
