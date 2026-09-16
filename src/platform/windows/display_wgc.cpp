/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <dxgi1_2.h>

// standard includes
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"
#include "utf_utils.h"

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    /**
     * @brief Create direct3 D11 device from DXGI device.
     *
     * @param dxgiDevice DXGI device.
     * @param graphicsDevice Graphics device.
     * @return Created direct3 D11 device from DXGI device object or status.
     */
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    /**
     * @brief Retrieve a DXGI interface from a WinRT Direct3D object.
     *
     * @param id COM interface ID requested from the WinRT wrapper.
     * @param object Output pointer that receives the requested COM interface.
     * @return HRESULT from the WinRT object's interface query.
     */
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

/**
 * @brief Return the GUID used to request IDirect3DDxgiInterfaceAccess.
 *
 * @return GUID for the WinRT DXGI interface-access helper.
 */
template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (monitor_factory == nullptr || FAILED(status = monitor_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    return finalize_init(display, config);
  }

  /**
   * @brief Initialize capture for a specific top-level window instead of a monitor.
   *
   * @param display Display object or identifier associated with the operation.
   * @param config Configuration values to apply.
   * @param hwnd Win32 window handle to capture.
   * @return 0 on success; nonzero or negative platform status on failure.
   */
  int wgc_capture_t::init_window(display_base_t *display, const ::video::config_t &config, HWND hwnd, bool update_display_size) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    if (!IsWindow(hwnd)) {
      BOOST_LOG(error) << "Window capture: invalid window handle [0x"sv << util::hex((std::uintptr_t) hwnd).to_string_view() << ']';
      return -1;
    }

    // Do not automatically restore or foreground the window: stealing focus
    // would fight other groups and interrupt the user. A minimized window
    // simply yields no WGC frames (capture times out and waits) until the
    // user brings it forward.

    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();

    auto capture_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (capture_factory == nullptr || FAILED(status = capture_factory->CreateForWindow(hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Window capture: failed to create capture item for window [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Use the capture item's authoritative size rather than GetWindowRect,
    // which includes the DWM drop shadow and would not match captured frames.
    // Auxiliary windows must not overwrite the anchor window dimensions.
    if (update_display_size) {
      auto item_size = item.Size();
      display->width = static_cast<int>(item_size.Width);
      display->height = static_cast<int>(item_size.Height);
    }

    return finalize_init(display, config);
  }

  /**
   * @brief Shared capture session setup used by both monitor and window backends.
   *
   * @param display Display object or identifier associated with the operation.
   * @param config Configuration values to apply.
   * @return 0 on success; nonzero or negative platform status on failure.
   */
  int wgc_capture_t::finalize_init(display_base_t *display, const ::video::config_t &config) {
    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);  // 250Hz
      } else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may be capped to 60fps on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }

  /**
   * @brief Initialize the D3D11 device, factory, and adapter for window capture.
   *
   * Window capture has no DXGI output to enumerate, so this shared helper
   * builds the capture device directly, applies COM and DPI awareness, and
   * records the client-requested frame rate. Shared by the RAM and VRAM
   * window backends.
   *
   * @param display Display base to populate.
   * @param config Configuration values to apply.
   * @return 0 on success; nonverbal/negative platform status on failure.
   */
  int init_window_device(display_base_t *display, const ::video::config_t &config) {
    // COM and DPI awareness only need to be applied once per process.
    static std::once_flag once;
    std::call_once(once, []() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);

      DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
      typedef BOOL (*User32_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);

      auto user32 = LoadLibraryA("user32.dll");
      if (user32) {
        auto f = (User32_SetProcessDpiAwarenessContext) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (f) {
          f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        FreeLibrary(user32);
      }
    });

    HRESULT status;

    display->env_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    display->env_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &display->factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Window capture: failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    auto adapter_name = utf_utils::from_utf8(config::video.adapter_name);
    adapter_t::pointer adapter_p;
    for (int x = 0; display->factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
      dxgi::adapter_t adapter_tmp {adapter_p};
      DXGI_ADAPTER_DESC1 adapter_desc;
      adapter_tmp->GetDesc1(&adapter_desc);

      if (!adapter_name.empty() && adapter_desc.Description != adapter_name) {
        continue;
      }

      display->adapter = std::move(adapter_tmp);
      break;
    }

    if (!display->adapter) {
      BOOST_LOG(error) << "Window capture: failed to locate an adapter"sv;
      return -1;
    }

    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    status = display->adapter->QueryInterface(IID_IDXGIAdapter, (void **) &adapter_p);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Window capture: failed to query IDXGIAdapter interface"sv;
      return -1;
    }

    status = D3D11CreateDevice(
      adapter_p,
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &display->device,
      &display->feature_level,
      &display->device_ctx
    );

    adapter_p->Release();

    if (FAILED(status)) {
      BOOST_LOG(error) << "Window capture: failed to create D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    display->client_frame_rate = config.framerate;
    display->client_frame_rate_strict = {0, 0};
    if (config.framerateX100 > 0) {
      const AVRational fps = ::video::framerate_to_rational(config);
      display->client_frame_rate_strict = DXGI_RATIONAL {static_cast<UINT>(fps.num), static_cast<UINT>(fps.den)};
    }

    return 0;
  }

  /**
   * @brief Lowercase ASCII characters in a string.
   *
   * @param value String to normalize.
   * @return Normalized copy with ASCII letters lowercased.
   */
  static std::string to_ascii_lower(std::string value) {
    for (auto &c : value) {
      if (static_cast<unsigned char>(c) < 0x80) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    return value;
  }

  /**
   * @brief Get the ASCII-lowercased class name of a window.
   *
   * @param hwnd Window handle.
   * @return Lowercased class name, or empty when unavailable.
   */
  static std::string window_class_name_lower(HWND hwnd) {
    wchar_t class_name[256] {};
    int len = GetClassNameW(hwnd, class_name, 256);
    if (len == 0) {
      return {};
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(len));
    for (auto *p = class_name; *p; ++p) {
      result.push_back(*p < 0x80 ? static_cast<char>(std::tolower(static_cast<unsigned char>(*p))) : '?');
    }
    return result;
  }

  window_capture_set_t::window_capture_set_t(display_base_t *display, const ::video::config_t &config, HWND anchor, const std::vector<std::string> &aux_exclude):
      display_ {display},
      anchor_ {anchor},
      aux_exclude_ {aux_exclude},
      config_ {config} {
    DWORD process_id = 0;
    GetWindowThreadProcessId(anchor_, &process_id);
    pid_ = process_id;
  }

  HWND window_capture_set_t::anchor() const {
    return anchor_;
  }

  std::size_t window_capture_set_t::size() const {
    return windows_.size();
  }

  bool window_capture_set_t::empty() const {
    return windows_.empty();
  }

  bool window_capture_set_t::should_capture(HWND candidate) const {
    if (candidate == nullptr || candidate == anchor_) {
      return false;
    }
    DWORD window_pid = 0;
    GetWindowThreadProcessId(candidate, &window_pid);
    if (window_pid != pid_) {
      return false;
    }
    if (!IsWindowVisible(candidate) || IsIconic(candidate)) {
      return false;
    }
    auto class_name = window_class_name_lower(candidate);
    for (const auto &excluded : aux_exclude_) {
      if (class_name == to_ascii_lower(excluded)) {
        return false;
      }
    }
    return true;
  }

  int window_capture_set_t::attach_window(HWND candidate) {
    if (windows_.find(candidate) != windows_.end()) {
      return 0;
    }
    // Keep the item on the heap: the WGC frame callback captures a pointer to
    // its `wgc_capture_t`, so the object must never be relocated.
    auto item = std::make_unique<window_item_t>();
    if (item->wgc.init_window(display_, config_, candidate, false)) {
      BOOST_LOG(warning) << "Window capture: failed to attach popup 0x"sv << util::hex((std::uintptr_t) candidate).to_string_view();
      return -1;
    }
    GetWindowRect(candidate, &item->rect);
    windows_.emplace(candidate, std::move(item));
    BOOST_LOG(info) << "Window capture: attached process window 0x"sv << util::hex((std::uintptr_t) candidate).to_string_view();
    return 0;
  }

  void window_capture_set_t::detach_window(HWND candidate) {
    windows_.erase(candidate);
    BOOST_LOG(info) << "Window capture: detached process window 0x"sv << util::hex((std::uintptr_t) candidate).to_string_view();
  }

  int window_capture_set_t::refresh() {
    if (!IsWindow(anchor_)) {
      return -1;
    }

    struct enum_ctx_t {
      std::uint32_t pid;  ///< Process ID to match.
      std::vector<HWND> present;  ///< Visible top-level windows of the process.
    };
    enum_ctx_t ctx {pid_, {}};
    EnumWindows([](HWND window, LPARAM lparam) -> BOOL {
      auto *data = reinterpret_cast<enum_ctx_t *>(lparam);
      DWORD window_pid = 0;
      GetWindowThreadProcessId(window, &window_pid);
      if (window_pid == data->pid) {
        data->present.push_back(window);
      }
      return TRUE;
    },
      reinterpret_cast<LPARAM>(&ctx));

    for (auto window : ctx.present) {
      if (should_capture(window)) {
        attach_window(window);
      }
    }

    for (auto it = windows_.begin(); it != windows_.end();) {
      const bool still_present = std::find(ctx.present.begin(), ctx.present.end(), it->first) != ctx.present.end();
      if (!still_present || !should_capture(it->first)) {
        it = windows_.erase(it);
      } else {
        ++it;
      }
    }
    return 0;
  }

  void window_capture_set_t::window_position(const window_item_t &item, int anchor_width, int anchor_height, int &out_x, int &out_y, int &out_w, int &out_h) const {
    RECT anchor_rect {};
    GetWindowRect(anchor_, &anchor_rect);
    const double scale_x = static_cast<double>(anchor_width) / static_cast<double>(std::max(1L, anchor_rect.right - anchor_rect.left));
    const double scale_y = static_cast<double>(anchor_height) / static_cast<double>(std::max(1L, anchor_rect.bottom - anchor_rect.top));
    out_x = static_cast<int>(std::llround((static_cast<double>(item.rect.left) - anchor_rect.left) * scale_x));
    out_y = static_cast<int>(std::llround((static_cast<double>(item.rect.top) - anchor_rect.top) * scale_y));
    out_w = static_cast<int>(std::llround(static_cast<double>(item.rect.right - item.rect.left) * scale_x));
    out_h = static_cast<int>(std::llround(static_cast<double>(item.rect.bottom - item.rect.top) * scale_y));
  }

  int window_capture_set_t::update_window_cache(window_item_t &item, ID3D11Texture2D *src, const D3D11_TEXTURE2D_DESC &src_desc) {
    // GPU cache used by the GPU composition path.
    if (!item.texture) {
      D3D11_TEXTURE2D_DESC t {};
      t.Width = src_desc.Width;
      t.Height = src_desc.Height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = src_desc.Format;
      t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      auto status = display_->device->CreateTexture2D(&t, nullptr, &item.texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Window capture: failed to create popup texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }
    D3D11_TEXTURE2D_DESC cache_desc;
    item.texture->GetDesc(&cache_desc);
    if (cache_desc.Width != src_desc.Width || cache_desc.Height != src_desc.Height || cache_desc.Format != src_desc.Format) {
      item.texture.reset();
      item.srv.reset();
      return -1;
    }
    display_->device_ctx->CopyResource(item.texture.get(), src);
    if (!item.srv) {
      auto status = display_->device->CreateShaderResourceView(item.texture.get(), nullptr, &item.srv);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Window capture: failed to create popup SRV [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    // CPU staging cache used by the CPU fallback composition.
    if (!item.staging) {
      D3D11_TEXTURE2D_DESC t {};
      t.Width = src_desc.Width;
      t.Height = src_desc.Height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = src_desc.Format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      auto status = display_->device->CreateTexture2D(&t, nullptr, &item.staging);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Window capture: failed to create popup staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }
    D3D11_TEXTURE2D_DESC staging_desc;
    item.staging->GetDesc(&staging_desc);
    if (staging_desc.Width != src_desc.Width || staging_desc.Height != src_desc.Height || staging_desc.Format != src_desc.Format) {
      item.staging.reset();
      return -1;
    }
    display_->device_ctx->CopyResource(item.staging.get(), src);
    return 0;
  }

  int window_capture_set_t::capture_popups(bool cursor_visible, std::chrono::milliseconds timeout) {
    for (auto &[window, item] : windows_) {
      GetWindowRect(window, &item->rect);

      texture2d_t popup_src;
      uint64_t popup_qpc;
      item->wgc.set_cursor_visible(cursor_visible);
      const auto popup_timeout = item->has_frame ? std::chrono::milliseconds(8) : std::min(timeout, std::chrono::milliseconds(100));
      auto popup_status = item->wgc.next_frame(popup_timeout, &popup_src, popup_qpc);
      if (popup_status == capture_e::ok) {
        D3D11_TEXTURE2D_DESC popup_desc;
        popup_src->GetDesc(&popup_desc);
        if (update_window_cache(*item, popup_src.get(), popup_desc) == 0) {
          item->has_frame = true;
        }
        item->wgc.release_frame();
      }
    }
    return 0;
  }

  int window_capture_set_t::composite_gpu(ID3D11RenderTargetView *rt, int anchor_width, int anchor_height, vs_t &vs, ps_t &ps, blend_t &blend, sampler_state_t &sampler) {
    display_->device_ctx->OMSetRenderTargets(1, &rt, nullptr);
    display_->device_ctx->VSSetShader(vs.get(), nullptr, 0);
    display_->device_ctx->PSSetShader(ps.get(), nullptr, 0);
    display_->device_ctx->OMSetBlendState(blend.get(), nullptr, 0xFFFFFFFFu);
    display_->device_ctx->PSSetSamplers(0, 1, &sampler);

    for (auto &[window, item] : windows_) {
      if (!item->has_frame || !item->srv) {
        continue;
      }
      int x = 0;
      int y = 0;
      int w = 0;
      int h = 0;
      window_position(*item, anchor_width, anchor_height, x, y, w, h);
      D3D11_VIEWPORT viewport {static_cast<FLOAT>(x), static_cast<FLOAT>(y), static_cast<FLOAT>(w), static_cast<FLOAT>(h), 0.0f, 1.0f};
      display_->device_ctx->PSSetShaderResources(0, 1, &item->srv);
      display_->device_ctx->RSSetViewports(1, &viewport);
      display_->device_ctx->Draw(3, 0);
    }

    ID3D11ShaderResourceView *empty_srv = nullptr;
    display_->device_ctx->PSSetShaderResources(0, 1, &empty_srv);
    return 0;
  }

  int window_capture_set_t::composite_cpu(std::uint8_t *frame, std::size_t row_pitch, std::size_t pixel_pitch, int anchor_width, int anchor_height) {
    if (!frame || pixel_pitch == 0 || row_pitch == 0) {
      return -1;
    }

    for (auto &[window, item] : windows_) {
      if (!item->has_frame || !item->staging) {
        continue;
      }
      D3D11_TEXTURE2D_DESC staging_desc;
      item->staging->GetDesc(&staging_desc);

      D3D11_MAPPED_SUBRESOURCE mapped {};
      if (FAILED(display_->device_ctx->Map(item->staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        BOOST_LOG(error) << "Window capture: failed to map popup staging texture"sv;
        continue;
      }
      auto unmap = util::fail_guard([&]() {
        display_->device_ctx->Unmap(item->staging.get(), 0);
      });

      int dst_x = 0;
      int dst_y = 0;
      int dst_w = 0;
      int dst_h = 0;
      window_position(*item, anchor_width, anchor_height, dst_x, dst_y, dst_w, dst_h);

      // Clip negative offsets so source rows/columns stay in bounds.
      int src_x = 0;
      int src_y = 0;
      if (dst_x < 0) {
        src_x = -dst_x;
        dst_x = 0;
      }
      if (dst_y < 0) {
        src_y = -dst_y;
        dst_y = 0;
      }

      const int src_w = static_cast<int>(staging_desc.Width);
      const int src_h = static_cast<int>(staging_desc.Height);
      const int cols = std::min(src_w - src_x, anchor_width - dst_x);
      const int rows = std::min(src_h - src_y, anchor_height - dst_y);
      if (cols <= 0 || rows <= 0) {
        continue;
      }

      if (pixel_pitch != 4) {
        // Non-BGRA frames are copied verbatim.
        for (int yy = 0; yy < rows; ++yy) {
          const auto *srow = static_cast<const std::uint8_t *>(mapped.pData) +
            static_cast<std::size_t>(yy + src_y) * mapped.RowPitch +
            static_cast<std::size_t>(src_x) * pixel_pitch;
          auto *drow = frame +
            static_cast<std::size_t>(dst_y + yy) * row_pitch +
            static_cast<std::size_t>(dst_x) * pixel_pitch;
          std::copy_n(srow, static_cast<std::size_t>(cols) * pixel_pitch, drow);
        }
        continue;
      }

      // Alpha-composite the popup onto the anchor frame so transparent areas
      // (rounded corners, drop shadows) blend instead of showing as black.
      for (int yy = 0; yy < rows; ++yy) {
        const auto *srow = static_cast<const std::uint8_t *>(mapped.pData) +
          static_cast<std::size_t>(yy + src_y) * mapped.RowPitch +
          static_cast<std::size_t>(src_x) * 4u;
        auto *drow = frame +
          static_cast<std::size_t>(dst_y + yy) * row_pitch +
          static_cast<std::size_t>(dst_x) * 4u;
        for (int xx = 0; xx < cols; ++xx) {
          const auto *s = srow + static_cast<std::size_t>(xx) * 4u;
          auto *d = drow + static_cast<std::size_t>(xx) * 4u;
          const std::uint32_t alpha = s[3];
          if (alpha == 0xFF) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
          } else if (alpha != 0) {
            const std::uint32_t inv = 0xFF - alpha;
            d[0] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(s[0]) * alpha + static_cast<std::uint32_t>(d[0]) * inv) / 0xFF);
            d[1] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(s[1]) * alpha + static_cast<std::uint32_t>(d[1]) * inv) / 0xFF);
            d[2] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(s[2]) * alpha + static_cast<std::uint32_t>(d[2]) * inv) / 0xFF);
            d[3] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(s[3]) * alpha + static_cast<std::uint32_t>(d[3]) * inv) / 0xFF);
          }
        }
      }
    }
    return 0;
  }

  int display_window_t::init(const ::video::config_t &config, const std::string &display_name, HWND hwnd, const std::vector<std::string> &aux_exclude) {
    if (init_window_device(this, config)) {
      return -1;
    }

    if (wgc.init_window(this, config, hwnd)) {
      return -1;
    }

    this->hwnd = hwnd;
    window_set = std::make_unique<window_capture_set_t>(this, config, hwnd, aux_exclude);
    window_set->refresh();

    BOOST_LOG(info) << "Window capture initialized: ["sv << width << 'x' << height << "] hwnd=0x"sv << util::hex((std::uintptr_t) hwnd).to_string_view();
    texture.reset();
    return 0;
  }

  capture_e display_window_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;

    // If the captured window has been destroyed, stop the session instead of
    // re-selecting another window: the captured application is gone.
    if (!IsWindow(hwnd)) {
      BOOST_LOG(warning) << "Window capture: captured window was closed, stopping stream"sv;
      return capture_e::error;
    }
    if (window_set->refresh()) {
      BOOST_LOG(warning) << "Window capture: anchor window was closed, stopping stream"sv;
      return capture_e::error;
    }

    texture2d_t src;
    uint64_t frame_qpc;
    wgc.set_cursor_visible(cursor_visible);
    auto capture_status = wgc.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Window capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Window capture: failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Window capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Window capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Window capture: failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';
      return capture_e::error;
    }

    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Composite each captured process window onto the anchor frame.
    if (!window_set->empty()) {
      window_set->capture_popups(cursor_visible, timeout);
      window_set->composite_cpu(img->data, img->row_pitch, img->pixel_pitch, width, height);
    }

    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_window_t::release_snapshot() {
    return wgc.release_frame();
  }
}  // namespace platf::dxgi
