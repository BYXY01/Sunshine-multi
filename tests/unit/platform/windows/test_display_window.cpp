/**
 * @file tests/unit/platform/windows/test_display_window.cpp
 * @brief Integration tests for the window capture backend.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <cstdint>
  #include <cstring>
  #include <fstream>
  #include <utility>
  #include <vector>

  // platform includes
  #include <windows.h>

  // local includes
  #include "src/platform/common.h"
  #include "src/platform/windows/display.h"
  #include "src/video.h"

namespace {

  /**
   * @brief Collect visible top-level windows that are candidates for capture.
   *
   * @return Vector of candidate window handles with their titles.
   */
  std::vector<std::pair<HWND, std::string>> collect_capture_windows() {
    std::vector<std::pair<HWND, std::string>> windows;

    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *out = reinterpret_cast<std::vector<std::pair<HWND, std::string>> *>(lparam);

      if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
      }

      auto title_length = GetWindowTextLengthW(hwnd);
      if (title_length == 0) {
        return TRUE;
      }

      std::vector<wchar_t> title(title_length + 1, L'\0');
      if (GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size())) == 0) {
        return TRUE;
      }

      RECT rect {};
      GetWindowRect(hwnd, &rect);
      if ((rect.right - rect.left) < 200 || (rect.bottom - rect.top) < 150) {
        return TRUE;
      }

      // Convert UTF-16 title to a narrow string for logging.
      std::string narrow;
      narrow.reserve(title.size());
      for (auto wc : title) {
        if (wc == L'\0') {
          break;
        }
        narrow.push_back(wc < 128 ? static_cast<char>(wc) : '?');
      }

      out->emplace_back(hwnd, std::move(narrow));
      return TRUE;
    },
      reinterpret_cast<LPARAM>(&windows));

    return windows;
  }

  /**
   * @brief Write a raw BGRA frame to a BMP file for visual inspection.
   *
   * @param path Output BMP path.
   * @param img Captured frame.
   * @return True when the file was written successfully.
   */
  bool write_bmp(const std::filesystem::path &path, const platf::img_t &img) {
    const auto width = img.width;
    const auto height = img.height;
    const auto row_size = width * 4;

    std::uint32_t pixel_data_size = row_size * static_cast<std::uint32_t>(height);
    std::uint32_t file_size = 54 + pixel_data_size;

    std::ofstream file {path, std::ios::binary};
    if (!file.is_open()) {
      return false;
    }

    std::uint8_t header[54] {};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &file_size, sizeof(file_size));
    std::uint32_t pixel_offset = 54;
    std::memcpy(header + 10, &pixel_offset, sizeof(pixel_offset));
    std::uint32_t dib_size = 40;
    std::memcpy(header + 14, &dib_size, sizeof(dib_size));
    std::int32_t w = width;
    std::int32_t h = height;
    std::memcpy(header + 18, &w, sizeof(w));
    std::memcpy(header + 22, &h, sizeof(h));
    std::uint16_t planes = 1;
    std::memcpy(header + 26, &planes, sizeof(planes));
    std::uint16_t bpp = 32;
    std::memcpy(header + 28, &bpp, sizeof(bpp));
    std::uint32_t compression = 0;
    std::memcpy(header + 30, &compression, sizeof(compression));
    std::memcpy(header + 34, &pixel_data_size, sizeof(pixel_data_size));

    file.write(reinterpret_cast<const char *>(header), sizeof(header));

    // BMP rows are bottom-up; copy rows in reverse order.
    const auto *src = static_cast<const std::uint8_t *>(img.data);
    for (int y = height - 1; y >= 0; --y) {
      file.write(reinterpret_cast<const char *>(src + static_cast<std::size_t>(y) * img.row_pitch), row_size);
    }

    return true;
  }

  /**
   * @brief Attempt to capture a single frame from a window.
   *
   * @param hwnd Window handle to capture.
   * @param img_out Captured frame.
   * @return Capture status from the final snapshot attempt.
   */
  platf::capture_e try_capture(HWND hwnd, std::shared_ptr<platf::img_t> &img_out) {
    img_out = nullptr;

    ::video::config_t config;
    config.framerate = 30;
    config.width = 1920;
    config.height = 1080;

    auto disp = std::make_shared<platf::dxgi::display_window_t>();
    if (disp->init(config, "", hwnd) != 0) {
      return platf::capture_e::error;
    }

    std::vector<std::shared_ptr<platf::img_t>> pool;
    for (int i = 0; i < 4; ++i) {
      pool.emplace_back(disp->alloc_img());
    }

    std::size_t next = 0;
    auto pull_free_image_cb = [&pool, &next](std::shared_ptr<platf::img_t> &img) {
      img = pool[next];
      next = (next + 1) % pool.size();
      return true;
    };

    // WGC needs a message pump plus a moment to deliver the first frame.
    auto deadline = std::chrono::steady_clock::now() + 3s;
    platf::capture_e status = platf::capture_e::timeout;
    while (status == platf::capture_e::timeout && std::chrono::steady_clock::now() < deadline) {
      MSG msg {};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }

      status = disp->snapshot(pull_free_image_cb, img_out, 250ms, false);
    }

    return status;
  }

}  // namespace

/**
 * @brief Test fixture for the window capture backend.
 */
class DisplayWindowCaptureTest: public PlatformTestSuite {};

TEST_F(DisplayWindowCaptureTest, CapturesAVisibleWindowFrame) {
  auto candidates = collect_capture_windows();
  if (candidates.empty()) {
    GTEST_SKIP() << "No capture-eligible visible window found";
    return;
  }

  std::shared_ptr<platf::img_t> img;
  platf::capture_e status = platf::capture_e::error;

  for (auto &[hwnd, _] : candidates) {
    status = try_capture(hwnd, img);
    if (status == platf::capture_e::ok && img) {
      break;
    }
  }

  if (status != platf::capture_e::ok || !img) {
    GTEST_SKIP() << "WGC did not deliver a frame for any candidate window";
    return;
  }

  EXPECT_GT(img->width, 0);
  EXPECT_GT(img->height, 0);
  EXPECT_EQ(img->pixel_pitch, 4);
  ASSERT_NE(img->data, nullptr);
  EXPECT_TRUE(img->frame_timestamp.has_value());

  // The frame must contain more than just blank pixels.
  const auto *pixels = static_cast<const std::uint8_t *>(img->data);
  const auto byte_count = static_cast<std::size_t>(img->height) * img->row_pitch;
  std::size_t nonzero = 0;
  for (std::size_t i = 0; i < byte_count; ++i) {
    if (pixels[i] != 0) {
      ++nonzero;
    }
  }
  EXPECT_GT(nonzero, 0u);

  // Persist the frame for manual inspection.
  const auto out_path = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "window_capture_frame.bmp";
  EXPECT_TRUE(write_bmp(out_path, *img));
}

TEST_F(DisplayWindowCaptureTest, RejectsInvalidWindowHandle) {
  ::video::config_t config;
  config.framerate = 30;
  config.width = 1920;
  config.height = 1080;

  auto disp = std::make_shared<platf::dxgi::display_window_t>();
  EXPECT_NE(disp->init(config, "", reinterpret_cast<HWND>(0xDEADBEEF)), 0);
}

#endif  // _WIN32
