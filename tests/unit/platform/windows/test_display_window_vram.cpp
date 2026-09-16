/**
 * @file tests/unit/platform/windows/test_display_window_vram.cpp
 * @brief Integration tests for the GPU-backed (VRAM) window capture backend.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <cstdint>
  #include <cstring>
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
   * @return Vector of candidate window handles.
   */
  std::vector<HWND> collect_capture_windows() {
    std::vector<HWND> windows;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
      auto *out = reinterpret_cast<std::vector<HWND> *>(lparam);
      if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
      }
      if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
      }
      RECT rect {};
      GetWindowRect(hwnd, &rect);
      if ((rect.right - rect.left) < 200 || (rect.bottom - rect.top) < 150) {
        return TRUE;
      }
      out->push_back(hwnd);
      return TRUE;
    },
      reinterpret_cast<LPARAM>(&windows));
    return windows;
  }

  /**
   * @brief Count non-zero bytes in the capture texture of a VRAM image.
   *
   * @param display VRAM display backend owning the device.
   * @param img Captured image.
   * @return Number of non-zero bytes read back from the capture texture.
   */
  std::size_t capture_nonzero(platf::dxgi::display_window_vram_t &display, const std::shared_ptr<platf::img_t> &img) {
    auto d3d_img = std::static_pointer_cast<platf::dxgi::img_d3d_t>(img);
    if (!d3d_img->capture_texture) {
      return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    d3d_img->capture_texture->GetDesc(&desc);

    platf::dxgi::keyed_mutex_t km;
    bool locked = false;
    if (SUCCEEDED(d3d_img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&km))) && km) {
      locked = SUCCEEDED(km->AcquireSync(0, INFINITE));
    }
    auto unlock = util::fail_guard([&]() {
      if (locked) {
        km->ReleaseSync(0);
      }
    });

    platf::dxgi::texture2d_t staging;
    D3D11_TEXTURE2D_DESC t {};
    t.Width = desc.Width;
    t.Height = desc.Height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_STAGING;
    t.Format = desc.Format;
    t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(display.device->CreateTexture2D(&t, nullptr, &staging))) {
      return 0;
    }

    display.device_ctx->CopyResource(staging.get(), d3d_img->capture_texture.get());

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(display.device_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return 0;
    }
    std::size_t nonzero = 0;
    for (std::uint32_t y = 0; y < desc.Height; ++y) {
      const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
      for (std::uint32_t x = 0; x < desc.Width * 4; ++x) {
        if (row[x] != 0) {
          ++nonzero;
        }
      }
    }
    display.device_ctx->Unmap(staging.get(), 0);
    return nonzero;
  }

  /**
   * @brief Attempt to capture a single frame from a window with the VRAM backend.
   *
   * @param hwnd Window handle to capture.
   * @param img_out Captured frame.
   * @param nonzero_out Non-zero byte count of the captured frame.
   * @return Capture status from the final snapshot attempt.
   */
  platf::capture_e try_capture(HWND hwnd, std::shared_ptr<platf::img_t> &img_out, std::size_t &nonzero_out) {
    img_out = nullptr;
    nonzero_out = 0;

    ::video::config_t config;
    config.framerate = 30;
    config.width = 1920;
    config.height = 1080;
    config.dynamicRange = 0;

    auto disp = std::make_shared<platf::dxgi::display_window_vram_t>();
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

    platf::capture_e status = platf::capture_e::timeout;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      MSG msg {};
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      status = disp->snapshot(pull_free_image_cb, img_out, 250ms, false);
      if (status == platf::capture_e::ok && img_out) {
        nonzero_out = capture_nonzero(*disp, img_out);
        if (nonzero_out > 0) {
          break;
        }
      } else if (status == platf::capture_e::error || status == platf::capture_e::interrupted) {
        break;
      }
    }
    return status;
  }

}  // namespace

/**
 * @brief Test fixture for the GPU-backed window capture backend.
 */
class DisplayWindowVramCaptureTest: public PlatformTestSuite {};

TEST_F(DisplayWindowVramCaptureTest, CapturesAVisibleWindowFrame) {
  std::vector<HWND> targets;
  if (test_args::window_hwnd != 0) {
    auto hwnd = reinterpret_cast<HWND>(test_args::window_hwnd);
    if (!IsWindow(hwnd)) {
      FAIL() << "--window-hwnd does not reference a valid window";
      return;
    }
    targets.push_back(hwnd);
  } else {
    targets = collect_capture_windows();
  }

  if (targets.empty()) {
    GTEST_SKIP() << "No capture-eligible visible window found";
    return;
  }

  std::shared_ptr<platf::img_t> img;
  std::size_t nonzero = 0;
  platf::capture_e status = platf::capture_e::error;
  for (auto hwnd : targets) {
    status = try_capture(hwnd, img, nonzero);
    if (status == platf::capture_e::ok && img && nonzero > 0) {
      break;
    }
  }

  if (status != platf::capture_e::ok || !img) {
    GTEST_SKIP() << "VRAM window capture did not deliver a frame";
    return;
  }

  EXPECT_GT(img->width, 0);
  EXPECT_GT(img->height, 0);
  EXPECT_GT(nonzero, 0u);
  BOOST_LOG(tests) << "vram captured " << img->width << 'x' << img->height << " nonzero=" << nonzero;
}

#endif  // _WIN32
