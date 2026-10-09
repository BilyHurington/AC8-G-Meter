// Offline test of the in-game overlay (mod/overlay) without the game.
//
// A Win32 window with its own D3D12 device, DIRECT queue and flip-model swap chain stands in
// for AC8: every frame it clears the back buffer to a known colour and presents (Present or
// Present1), keeping up to 3 frames in flight like a game. ft::mod::startOverlay() hooks
// Present exactly as in the game (the swap chain already exists when the hooks are
// installed), and a test client draws known shapes on ImGui's background draw list. No ImGui
// code of its own: the overlay owns ImGui. During "flight" the game holds the mouse capture.
//
// Usage: overlay_harness [--format bgra8|rgba8|rgb10a2|fp16] [--hdr10 | --hdr10-early]
//                        [--frames N] [--size WxH] [--window WxH] [--resize-at N] [--present1]
//                        [--render-thread] [--decoys] [--unsubclass-at N]
//                        [--fault-at N | --disable-at N] [--check]
//   --hdr10        SetColorSpace1(ST.2084) once the hooks are in (rgb10a2 only)
//   --hdr10-early  the same before startOverlay(), as in the game (the hook never sees it)
//   --size         back-buffer size; --window: client-area size if different (mouse scaling)
//   --resize-at    ResizeBuffers to 3/4 size before frame N; fails if the overlay still holds
//                  a back-buffer reference
//   --render-thread  present from a second thread while the main thread runs the window
//   --decoys       also present, before the game each frame, a visible D3D11 window and a
//                  hidden D3D12 swap chain, and a DXGI_PRESENT_TEST on the game's swap chain:
//                  none of them may be drawn on or called back for
//   --unsubclass-at  restore the game's window procedure before frame N (another mod undoing
//                  its own subclass): the overlay must notice and subclass again
//   --fault-at     the client raises an access violation in frame N: the overlay must turn
//                  itself off and the game keep running
//   --disable-at   ft::mod::disableOverlay() before frame N
//   --check        exit 0 only if the overlay works: the client was initialised once and
//                  called every frame with the back-buffer size; the shapes have the expected
//                  colours in the presented image (encoded for the format and colour space,
//                  alpha blended over the game image) and the rest is untouched; Ctrl+key
//                  chords reached ImGui whether or not it captured input; mouse clicks reached
//                  ImGui (at the scaled position) only while capturing; the game saw key-downs
//                  and clicks only while not capturing, key-ups and button-ups always, and
//                  kept its mouse capture. With --fault-at / --disable-at (N < 10): the client
//                  is not called after that frame, nothing is drawn, and all input reaches
//                  the game.
//
// Read-back: the overlay composites inside Present, so the harness copies the buffer it has
// just presented, on its own queue (after the overlay's work), once Present returns. The swap
// chain is FLIP_SEQUENTIAL because a presented buffer then keeps its contents (FLIP_DISCARD
// leaves them undefined); the overlay does not depend on the swap effect. This keeps
// test-only code out of the overlay.
//
// Log: overlay_harness_<variant>.log next to the executable (the overlay logs there too).
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#include "overlay/overlay.h"
#include "support/log.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// ---- What the test client draws, in back-buffer pixels -------------------------------------

constexpr ImVec2 kOpaqueMin{40, 40}, kOpaqueMax{240, 140};  // opaque rectangle
constexpr ImVec2 kHalfMin{40, 160}, kHalfMax{240, 260};     // 50 % alpha rectangle
constexpr int kOpaqueColor[3] = {230, 40, 160};             // sRGB, 8 bit
constexpr int kHalfColor[3] = {40, 200, 90};
constexpr int kHalfAlpha = 128;
constexpr UINT kMinWidth = 480, kMinHeight = 400;  // shapes stay inside after a 3/4 resize

// Input probes: frame at which the game takes the mouse capture ("flight"), at which the
// uncaptured / captured Ctrl+key and click are posted, and at which capture ends.
constexpr int kFlightStart = 5;
constexpr int kProbeUncaptured = 10;
constexpr int kProbeCaptured = 20;
constexpr int kProbeEnd = 30;
constexpr int kMinCheckFrames = 40;
constexpr int kProbeX = 300, kProbeY = 200;  // client-area pixels

// Messages the harness sends itself, handled by the game's window procedure on the window
// thread (keyboard state and mouse capture are per thread).
constexpr UINT kMsgTakeCapture = WM_APP + 1;
constexpr UINT kMsgSetCtrl = WM_APP + 2;     // wParam: 1 down, 0 up
constexpr UINT kMsgProbeDone = WM_APP + 3;   // wParam: probe index

struct Rgb {
  double r = 0, g = 0, b = 0;
};

// The "game" image, in the back buffer's own encoding (sRGB-ish, scRGB or PQ values).
constexpr Rgb kClear{0.10, 0.20, 0.30};
constexpr double kPaperWhiteNits = 200.0;  // must match mod/overlay/renderer.cpp

enum class Encoding { Sdr, ScRgb, Hdr10 };

struct Options {
  std::string formatName = "bgra8";
  DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
  bool hdr10 = false;
  bool hdr10Early = false;
  int frames = 60;
  UINT width = 1280, height = 720;              // back buffer
  UINT windowWidth = 0, windowHeight = 0;       // client area (0: same as the back buffer)
  int resizeAt = -1;
  bool present1 = false;
  bool renderThread = false;
  bool decoys = false;
  int unsubclassAt = -1;
  int faultAt = -1;
  int disableAt = -1;
  bool check = false;
  // Frame from which the overlay must be off (-1: never).
  int stopFrame() const { return faultAt >= 0 ? faultAt + 1 : disableAt; }
};

// ---- Logging -------------------------------------------------------------------------------

template <class... Args>
void report(std::format_string<Args...> fmt, Args&&... args) {
  const std::string line = std::format(fmt, std::forward<Args>(args)...);
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
  ft::mod::logLine("harness: " + line);
}

fs::path executableDir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return fs::current_path();
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).parent_path();
    }
    buf.resize(buf.size() * 2);  // truncated: long path
  }
}

// ---- Command line --------------------------------------------------------------------------

bool parseInt(std::string_view s, int& out) {
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && end == s.data() + s.size();
}

bool parseSize(std::string_view s, UINT& width, UINT& height) {
  const std::size_t x = s.find('x');
  int w = 0, h = 0;
  if (x == std::string_view::npos || !parseInt(s.substr(0, x), w) || !parseInt(s.substr(x + 1), h)) return false;
  if (w < 64 || h < 64) return false;
  width = static_cast<UINT>(w);
  height = static_cast<UINT>(h);
  return true;
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == "--format" && hasValue) {
      o.formatName = argv[++i];
      if (o.formatName == "bgra8") o.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      else if (o.formatName == "rgba8") o.format = DXGI_FORMAT_R8G8B8A8_UNORM;
      else if (o.formatName == "rgb10a2") o.format = DXGI_FORMAT_R10G10B10A2_UNORM;
      else if (o.formatName == "fp16") o.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      else return false;
    } else if (arg == "--hdr10") {
      o.hdr10 = true;
    } else if (arg == "--hdr10-early") {
      o.hdr10 = o.hdr10Early = true;
    } else if (arg == "--frames" && hasValue) {
      if (!parseInt(argv[++i], o.frames) || o.frames < 1) return false;
    } else if (arg == "--size" && hasValue) {
      if (!parseSize(argv[++i], o.width, o.height) || o.width < kMinWidth || o.height < kMinHeight) return false;
    } else if (arg == "--window" && hasValue) {
      if (!parseSize(argv[++i], o.windowWidth, o.windowHeight)) return false;
    } else if (arg == "--resize-at" && hasValue) {
      if (!parseInt(argv[++i], o.resizeAt) || o.resizeAt < 1) return false;
    } else if (arg == "--unsubclass-at" && hasValue) {
      if (!parseInt(argv[++i], o.unsubclassAt) || o.unsubclassAt < 1) return false;
    } else if (arg == "--fault-at" && hasValue) {
      if (!parseInt(argv[++i], o.faultAt) || o.faultAt < 0) return false;
    } else if (arg == "--disable-at" && hasValue) {
      if (!parseInt(argv[++i], o.disableAt) || o.disableAt < 0) return false;
    } else if (arg == "--present1") {
      o.present1 = true;
    } else if (arg == "--render-thread") {
      o.renderThread = true;
    } else if (arg == "--decoys") {
      o.decoys = true;
    } else if (arg == "--check") {
      o.check = true;
    } else {
      return false;
    }
  }
  if (o.windowWidth == 0) {
    o.windowWidth = o.width;
    o.windowHeight = o.height;
  }
  if (o.hdr10 && o.format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
  if (o.check && o.frames < kMinCheckFrames) return false;
  if (o.resizeAt >= o.frames) return false;
  if (o.faultAt >= 0 && o.disableAt >= 0) return false;
  if (o.check && o.stopFrame() >= kProbeUncaptured) return false;   // stop before the input probes
  if (o.check && o.unsubclassAt >= kProbeUncaptured) return false;  // re-subclass before them
  return true;
}

// ---- Expected colours ----------------------------------------------------------------------

double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

double pqEncode(double y) {  // ST 2084 inverse EOTF, y in units of 10000 nits
  const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const double p = std::pow(std::clamp(y, 0.0, 1.0), m1);
  return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// An 8-bit sRGB ImGui colour as the overlay should write it to the back buffer.
Rgb encode(Encoding e, const int srgb[3]) {
  const Rgb c{srgb[0] / 255.0, srgb[1] / 255.0, srgb[2] / 255.0};
  if (e == Encoding::Sdr) return c;
  const Rgb lin{srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)};
  if (e == Encoding::ScRgb) {
    const double s = kPaperWhiteNits / 80.0;
    return {lin.r * s, lin.g * s, lin.b * s};
  }
  const double s = kPaperWhiteNits / 10000.0;  // Rec.709 -> Rec.2020 primaries, then PQ
  return {pqEncode((0.6274040 * lin.r + 0.3292820 * lin.g + 0.0433136 * lin.b) * s),
          pqEncode((0.0690970 * lin.r + 0.9195400 * lin.g + 0.0113612 * lin.b) * s),
          pqEncode((0.0163916 * lin.r + 0.0880132 * lin.g + 0.8955950 * lin.b) * s)};
}

Rgb over(Rgb src, double alpha, Rgb dst) {
  const double keep = 1 - alpha;
  return {src.r * alpha + dst.r * keep, src.g * alpha + dst.g * keep, src.b * alpha + dst.b * keep};
}

float halfToFloat(std::uint16_t h) {
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  float v = 0.0f;
  if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
  else if (exponent == 31) v = mantissa ? NAN : INFINITY;
  else v = std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
  return (h & 0x8000) ? -v : v;
}

UINT bytesPerPixel(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }

Rgb decodePixel(const std::uint8_t* p, DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return {p[2] / 255.0, p[1] / 255.0, p[0] / 255.0};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {p[0] / 255.0, p[1] / 255.0, p[2] / 255.0};
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
      std::uint32_t v = 0;
      std::memcpy(&v, p, sizeof v);
      return {(v & 0x3FF) / 1023.0, ((v >> 10) & 0x3FF) / 1023.0, ((v >> 20) & 0x3FF) / 1023.0};
    }
    default: {
      std::uint16_t h[4] = {};
      std::memcpy(h, p, sizeof h);
      return {halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2])};
    }
  }
}

// ---- Test client ---------------------------------------------------------------------------

class TestClient final : public ft::mod::OverlayClient {
 public:
  void onImGuiInit() override { ++initCalls; }

  void onFrame(float width, float height, double) override {
    if (frameCalls++ == faultAt) {
      wantsInputCallsAtStop = wantsInputCalls.load();
      RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    lastWidth = width;
    lastHeight = height;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(kOpaqueMin, kOpaqueMax, IM_COL32(kOpaqueColor[0], kOpaqueColor[1], kOpaqueColor[2], 255));
    dl->AddRectFilled(kHalfMin, kHalfMax, IM_COL32(kHalfColor[0], kHalfColor[1], kHalfColor[2], kHalfAlpha));
    dl->AddText(ImVec2(40, 272), IM_COL32_WHITE, "AC8 G-Meter overlay harness");  // exercises the font atlas
    // Hotkeys as ft::TelemetryUi detects them: an ImGui key press with io.KeyCtrl.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false) && io.KeyCtrl) ++ctrlF7;
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false) && io.KeyCtrl) ++ctrlF8;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      ++clicks;
      clickPos = io.MouseClickedPos[ImGuiMouseButton_Left];
    }
  }

  bool wantsInput() const override {
    ++wantsInputCalls;
    return capture.load();
  }

  std::atomic<bool> capture{false};
  int faultAt = -1;
  mutable std::atomic<int> wantsInputCalls{0};
  int wantsInputCallsAtStop = -1;
  int initCalls = 0, frameCalls = 0, ctrlF7 = 0, ctrlF8 = 0, clicks = 0;
  ImVec2 clickPos{-1, -1};
  float lastWidth = 0, lastHeight = 0;
};

// ---- The "game" ----------------------------------------------------------------------------

// What the game's own window procedure received (the overlay subclasses it).
struct GameInput {
  int f7Down = 0, f8Down = 0, f8Up = 0, buttonDown = 0, buttonUp = 0;
  bool captureHeld[2] = {false, false};  // after each probe
};
GameInput g_gameInput;
HANDLE g_probeDone = nullptr;  // set when a probe's messages have all been processed

void setCtrlState(bool down) {
  BYTE keys[256] = {};
  GetKeyboardState(keys);
  keys[VK_CONTROL] = keys[VK_LCONTROL] = down ? 0x80 : 0;
  SetKeyboardState(keys);
}

LRESULT CALLBACK gameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_KEYDOWN:
      if (wParam == VK_F7) ++g_gameInput.f7Down;
      if (wParam == VK_F8) ++g_gameInput.f8Down;
      return 0;
    case WM_KEYUP:
      if (wParam == VK_F8) ++g_gameInput.f8Up;
      return 0;
    case WM_LBUTTONDOWN: ++g_gameInput.buttonDown; return 0;
    case WM_LBUTTONUP: ++g_gameInput.buttonUp; return 0;  // the game keeps its capture in flight
    case kMsgTakeCapture: SetCapture(hwnd); return 0;
    case kMsgSetCtrl: setCtrlState(wParam != 0); return 0;
    case kMsgProbeDone:
      g_gameInput.captureHeld[wParam & 1] = GetCapture() == hwnd;
      SetEvent(g_probeDone);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

HWND createWindow(const wchar_t* title, UINT width, UINT height, bool show) {
  static const ATOM windowClass = [] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = gameWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OverlayHarnessGame";
    return RegisterClassExW(&wc);
  }();
  RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
  HWND hwnd = CreateWindowExW(0, MAKEINTATOM(windowClass), title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (hwnd && show) ShowWindow(hwnd, SW_SHOWNOACTIVATE);  // the overlay ignores hidden windows; no focus theft
  return hwnd;
}

bool pumpMessages() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) return false;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return true;
}

class Game {
 public:
  ~Game() {
    if (fenceEvent_) CloseHandle(fenceEvent_);
  }

  bool create(HWND hwnd, const Options& o) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) return false;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)))) return false;
    for (Frame& f : frames_)
      if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
        return false;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list_))) ||
        FAILED(list_->Close()) || FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
      return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    if (!fenceEvent_ || FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)))) return false;
    swapChain_ = createSwapChain(hwnd, o.width, o.height, o.format, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    return swapChain_ != nullptr;
  }

  // A flip-model swap chain on this game's queue (the harness's own, or a decoy).
  ComPtr<IDXGISwapChain3> createSwapChain(HWND hwnd, UINT width, UINT height, DXGI_FORMAT format,
                                          DXGI_SWAP_EFFECT effect) {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return nullptr;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = format;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = effect;
    ComPtr<IDXGISwapChain1> swapChain1;
    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &swapChain1)) ||
        FAILED(swapChain1.As(&swapChain3)))
      return nullptr;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return swapChain3;
  }

  IDXGISwapChain3* swapChain() const { return swapChain_.Get(); }

  // Clears the current back buffer to kClear and presents it; returns its index. Waits only
  // for the frame that used this command allocator 3 frames ago. Holds no back-buffer
  // reference afterwards.
  UINT renderAndPresent(bool present1) {
    Frame& frame = frames_[frameIndex_++ % kFramesInFlight];
    waitFor(frame.fenceValue);
    const UINT index = swapChain_->GetCurrentBackBufferIndex();
    {
      ComPtr<ID3D12Resource> backBuffer;
      swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffer));
      const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
      device_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv);
      frame.allocator->Reset();
      list_->Reset(frame.allocator.Get(), nullptr);
      transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float clear[4] = {static_cast<float>(kClear.r), static_cast<float>(kClear.g),
                              static_cast<float>(kClear.b), 1.0f};
      list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
      transition(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
      list_->Close();
      ID3D12CommandList* lists[] = {list_.Get()};
      queue_->ExecuteCommandLists(1, lists);
    }
    if (present1) {
      const DXGI_PRESENT_PARAMETERS params{};
      swapChain_->Present1(0, 0, &params);
    } else {
      swapChain_->Present(0, 0);
    }
    queue_->Signal(fence_.Get(), ++fenceValue_);
    frame.fenceValue = fenceValue_;
    return index;
  }

  bool resize(UINT width, UINT height) {
    waitIdle();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
      report("ResizeBuffers failed (0x{:08X}): a back buffer is still referenced?", static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
  }

  void waitIdle() {
    queue_->Signal(fence_.Get(), ++fenceValue_);
    waitFor(fenceValue_);
  }

  // Copies back buffer `index` (already presented) into `pixels`, rows `rowPitch` bytes apart.
  bool readBack(UINT index, std::vector<std::uint8_t>& pixels, UINT& rowPitch, UINT& width, UINT& height) {
    waitIdle();
    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffer)))) return false;
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = totalBytes;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&readback))))
      return false;
    ID3D12CommandAllocator* allocator = frames_[0].allocator.Get();  // idle: any will do
    allocator->Reset();
    list_->Reset(allocator, nullptr);
    transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = backBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitIdle();

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    if (FAILED(readback->Map(0, &readRange, &mapped))) return false;
    pixels.assign(static_cast<const std::uint8_t*>(mapped), static_cast<const std::uint8_t*>(mapped) + totalBytes);
    const D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);
    rowPitch = footprint.Footprint.RowPitch;
    width = static_cast<UINT>(desc.Width);
    height = desc.Height;
    return true;
  }

 private:
  static constexpr UINT kFramesInFlight = 3;

  struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fenceValue = 0;
  };

  void waitFor(UINT64 value) {
    if (fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, fenceEvent_);
    WaitForSingleObject(fenceEvent_, INFINITE);
  }

  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list_->ResourceBarrier(1, &b);
  }

  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  Frame frames_[kFramesInFlight];
  UINT64 frameIndex_ = 0;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12DescriptorHeap> rtvHeap_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  UINT64 fenceValue_ = 0;
  ComPtr<IDXGISwapChain3> swapChain_;
};

// Swap chains the overlay must leave alone: a visible D3D11 window (like UE4SS's GUI or a
// splash screen) and a hidden D3D12 one (a helper), both presenting before the game.
class Decoys {
 public:
  bool create(Game& game) {
    d3d11Window_ = createWindow(L"Overlay harness decoy (D3D11)", 320, 240, true);
    hiddenWindow_ = createWindow(L"Overlay harness decoy (hidden)", 320, 240, false);
    if (!d3d11Window_ || !hiddenWindow_) return false;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device11_, nullptr, nullptr)))
      return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 320;
    sd.Height = 240;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device11_.Get(), d3d11Window_, &sd, nullptr, nullptr, &swapChain11_)))
      return false;
    hidden12_ =
        game.createSwapChain(hiddenWindow_, 320, 240, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    return hidden12_ != nullptr;
  }

  void present(Game& game) {
    swapChain11_->Present(0, 0);
    hidden12_->Present(0, 0);
    game.swapChain()->Present(0, DXGI_PRESENT_TEST);
  }

 private:
  HWND d3d11Window_ = nullptr, hiddenWindow_ = nullptr;
  ComPtr<ID3D11Device> device11_;
  ComPtr<IDXGISwapChain1> swapChain11_;
  ComPtr<IDXGISwapChain3> hidden12_;
};

// ---- Input probes --------------------------------------------------------------------------

// Posts the game's capture, the Ctrl+key and click probes for `frame`. With a separate render
// thread, waits until the window thread has processed them, so that the capture state set
// here is the one the window procedure sees.
void postInputProbes(HWND hwnd, int frame, TestClient& client, bool waitForWindowThread) {
  constexpr LPARAM kKeyDown = 1;                               // repeat count 1
  constexpr LPARAM kKeyUp = static_cast<LPARAM>(0xC0000001u);  // + previous state, transition
  const LPARAM point = MAKELPARAM(kProbeX, kProbeY);
  if (frame == kFlightStart) PostMessageW(hwnd, kMsgTakeCapture, 0, 0);
  if (frame == kProbeUncaptured || frame == kProbeCaptured) {
    const bool captured = frame == kProbeCaptured;
    client.capture = captured;
    const WPARAM key = captured ? VK_F8 : VK_F7;
    // Take the capture again right before the clicks: other windows activating meanwhile (e.g.
    // parallel test runs) can make the system release it, which is not what is tested here.
    PostMessageW(hwnd, kMsgTakeCapture, 0, 0);
    // Ctrl is set in the window thread's keyboard state, which imgui_impl_win32 reads with
    // GetKeyState (posting WM_KEYDOWN alone does not change it), then released by a key-up.
    PostMessageW(hwnd, kMsgSetCtrl, 1, 0);
    PostMessageW(hwnd, WM_KEYDOWN, key, kKeyDown);
    PostMessageW(hwnd, WM_KEYUP, key, kKeyUp);
    PostMessageW(hwnd, kMsgSetCtrl, 0, 0);
    PostMessageW(hwnd, WM_KEYUP, VK_CONTROL, kKeyUp);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, point);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, point);
    PostMessageW(hwnd, kMsgProbeDone, captured ? 1 : 0, 0);
    if (waitForWindowThread) WaitForSingleObject(g_probeDone, 5000);
  } else if (frame == kProbeEnd) {
    client.capture = false;
  }
}

// ---- Checks --------------------------------------------------------------------------------

class Checker {
 public:
  void expect(bool ok, const std::string& what) {
    report("{} {}", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures_;
  }
  void expectCount(int actual, int expected, const char* what) {
    expect(actual == expected, std::format("{}: {} (expected {})", what, actual, expected));
  }
  int failures() const { return failures_; }

 private:
  int failures_ = 0;
};

struct Image {
  std::vector<std::uint8_t> pixels;
  UINT rowPitch = 0, width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Rgb at(UINT x, UINT y) const {
    const std::size_t offset =
        static_cast<std::size_t>(y) * rowPitch + static_cast<std::size_t>(x) * bytesPerPixel(format);
    return decodePixel(pixels.data() + offset, format);
  }
};

void checkPixel(Checker& c, const Image& img, UINT x, UINT y, Rgb expected, double tolerance, const char* what) {
  const Rgb got = img.at(x, y);
  const bool ok = std::abs(got.r - expected.r) <= tolerance && std::abs(got.g - expected.g) <= tolerance &&
                  std::abs(got.b - expected.b) <= tolerance;
  c.expect(ok, std::format("{} at ({}, {}): ({:.4f}, {:.4f}, {:.4f}), expected ({:.4f}, {:.4f}, {:.4f}) +- {:.4f}",
                           what, x, y, got.r, got.g, got.b, expected.r, expected.g, expected.b, tolerance));
}

int runChecks(const Options& o, Encoding encoding, const TestClient& client, const Image& img) {
  Checker c;
  const auto center = [](ImVec2 a, ImVec2 b) {
    return std::pair<UINT, UINT>{static_cast<UINT>((a.x + b.x) / 2), static_cast<UINT>((a.y + b.y) / 2)};
  };
  const auto [ox, oy] = center(kOpaqueMin, kOpaqueMax);
  const auto [hx, hy] = center(kHalfMin, kHalfMax);
  c.expectCount(client.initCalls, 1, "onImGuiInit calls");
  checkPixel(c, img, img.width - 20, img.height - 20, kClear, 1.0 / 255.0, "untouched background");
  c.expect(g_gameInput.captureHeld[0], "game kept its mouse capture through the uncaptured click");

  if (o.stopFrame() >= 0) {  // overlay turned off before the input probes
    c.expectCount(client.frameCalls, o.stopFrame(), "onFrame calls (none once stopped)");
    c.expectCount(client.wantsInputCalls.load(), client.wantsInputCallsAtStop, "wantsInput calls (none once stopped)");
    checkPixel(c, img, ox, oy, kClear, 1.0 / 255.0, "nothing drawn once stopped");
    c.expectCount(client.ctrlF7 + client.ctrlF8 + client.clicks, 0, "ImGui input events once stopped");
    c.expectCount(g_gameInput.f7Down + g_gameInput.f8Down, 2, "game saw both key downs");
    c.expectCount(g_gameInput.buttonDown, 2, "game saw both button downs");
    return c.failures();
  }

  c.expectCount(client.frameCalls, o.frames, "onFrame calls");
  c.expect(client.lastWidth == static_cast<float>(img.width) && client.lastHeight == static_cast<float>(img.height),
           std::format("onFrame size {}x{} == back buffer {}x{}", client.lastWidth, client.lastHeight, img.width,
                       img.height));
  // Tolerances: 8-bit canvas quantisation (amplified a little by un-premultiplying the 50 %
  // shape before HDR encoding), plus the back buffer's own precision.
  const double tolerance = encoding == Encoding::Sdr ? 3.0 / 255.0 : encoding == Encoding::Hdr10 ? 0.012 : 0.03;
  checkPixel(c, img, ox, oy, encode(encoding, kOpaqueColor), tolerance, "opaque shape");
  checkPixel(c, img, hx, hy, over(encode(encoding, kHalfColor), kHalfAlpha / 255.0, kClear), tolerance, "50% shape");

  c.expectCount(client.ctrlF7, 1, "ImGui saw Ctrl+F7 (not capturing)");
  c.expectCount(client.ctrlF8, 1, "ImGui saw Ctrl+F8 (capturing)");
  c.expectCount(client.clicks, 1, "ImGui saw clicks (only while capturing)");
  // Back-buffer pixels: the probe is in client pixels of a window that may be smaller.
  const float expectX = static_cast<float>(kProbeX) * static_cast<float>(o.width) / static_cast<float>(o.windowWidth);
  const float expectY = static_cast<float>(kProbeY) * static_cast<float>(o.height) / static_cast<float>(o.windowHeight);
  c.expect(std::abs(client.clickPos.x - expectX) <= 1.0f && std::abs(client.clickPos.y - expectY) <= 1.0f,
           std::format("click at ({}, {}) in back-buffer pixels (expected ({}, {}))", client.clickPos.x,
                       client.clickPos.y, expectX, expectY));
  c.expectCount(g_gameInput.f7Down, 1, "game saw F7 down (not capturing)");
  c.expectCount(g_gameInput.f8Down, 0, "game saw F8 down (capturing)");
  c.expectCount(g_gameInput.f8Up, 1, "game saw F8 up (key-ups always pass)");
  c.expectCount(g_gameInput.buttonDown, 1, "game saw button downs (only the uncaptured one)");
  c.expectCount(g_gameInput.buttonUp, 2, "game saw button ups (always pass)");
  c.expect(g_gameInput.captureHeld[1], "game kept its mouse capture through the captured click");
  return c.failures();
}

struct FrameLoopResult {
  bool ok = true;
  UINT presented = 0;  // index of the last presented back buffer
  UINT width = 0, height = 0;
};

// The game's frames (on the main thread, or on a render thread with --render-thread).
FrameLoopResult runFrames(const Options& o, HWND hwnd, Game& game, Decoys* decoys, TestClient& client) {
  FrameLoopResult r;
  r.width = o.width;
  r.height = o.height;
  for (int frame = 0; frame < o.frames; ++frame) {
    if (!o.renderThread && !pumpMessages()) {
      report("window closed");
      r.ok = false;
      return r;
    }
    if (frame == o.resizeAt) {
      r.width = r.width * 3 / 4;
      r.height = r.height * 3 / 4;
      if (!game.resize(r.width, r.height)) {
        r.ok = false;
        return r;
      }
      report("resized the swap chain to {}x{} before frame {}", r.width, r.height, frame);
    }
    if (frame == o.unsubclassAt) {
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&gameWndProc));
      report("restored the game's window procedure before frame {}", frame);
    }
    if (frame == o.disableAt) {
      ft::mod::disableOverlay();
      client.wantsInputCallsAtStop = client.wantsInputCalls.load();
      report("disabled the overlay before frame {}", frame);
    }
    if (o.check) postInputProbes(hwnd, frame, client, o.renderThread);
    if (decoys) decoys->present(game);
    r.presented = game.renderAndPresent(o.present1);
  }
  return r;
}

int run(const Options& o) {
  HWND hwnd = createWindow(L"AC8 G-Meter overlay harness", o.windowWidth, o.windowHeight, true);
  Game game;
  if (!hwnd || !game.create(hwnd, o)) {
    report("creating the window / D3D12 device / swap chain failed");
    return 1;
  }
  Decoys decoys;
  if (o.decoys && !decoys.create(game)) {
    report("creating the decoy swap chains failed");
    return 1;
  }

  Encoding encoding = o.hdr10 ? Encoding::Hdr10 : Encoding::Sdr;
  const auto setColorSpace = [&] {
    if (o.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
      encoding = Encoding::ScRgb;
      game.swapChain()->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    } else if (o.hdr10) {
      const HRESULT hr = game.swapChain()->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
      if (FAILED(hr)) {
        report("SetColorSpace1(ST.2084) failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
      }
    }
    return true;
  };
  if (o.hdr10Early && !setColorSpace()) return 1;

  TestClient client;
  client.faultAt = o.faultAt;
  if (!ft::mod::startOverlay(client)) {
    report("startOverlay failed");
    return 1;
  }
  for (int i = 0; i < 1000 && !ft::mod::overlayInstalled(); ++i) Sleep(10);
  if (!ft::mod::overlayInstalled()) {
    report("the overlay hooks were not installed (see the log)");
    return 1;
  }
  if (!o.hdr10Early && !setColorSpace()) return 1;

  FrameLoopResult result;
  if (o.renderThread) {
    std::atomic<bool> done{false};
    std::thread renderThread([&] {
      result = runFrames(o, hwnd, game, o.decoys ? &decoys : nullptr, client);
      done = true;
    });
    while (!done) {  // the main thread is the window thread
      pumpMessages();
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
    renderThread.join();
  } else {
    result = runFrames(o, hwnd, game, o.decoys ? &decoys : nullptr, client);
  }
  if (!result.ok) return 1;
  pumpMessages();
  report("presented {} frames ({}x{}, {}{}{}{})", o.frames, result.width, result.height, o.formatName,
         o.hdr10 ? (o.hdr10Early ? ", HDR10 set before start" : ", HDR10") : "", o.present1 ? ", Present1" : "",
         o.renderThread ? ", render thread" : "");
  if (!o.check) return 0;

  Image img;
  img.format = o.format;
  if (!game.readBack(result.presented, img.pixels, img.rowPitch, img.width, img.height)) {
    report("reading back the presented image failed");
    return 1;
  }
  const int failures = runChecks(o, encoding, client, img);
  if (failures == 0)
    report("all checks passed");
  else
    report("{} check(s) failed", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: overlay_harness [--format bgra8|rgba8|rgb10a2|fp16] [--hdr10 | --hdr10-early (rgb10a2)]\n"
                 "                       [--frames N] [--size WxH (>= %ux%u)] [--window WxH] [--resize-at N]\n"
                 "                       [--present1] [--render-thread] [--decoys] [--unsubclass-at N]\n"
                 "                       [--fault-at N | --disable-at N] [--check (frames >= %d; stop < %d)]\n",
                 kMinWidth, kMinHeight, kMinCheckFrames, kProbeUncaptured);
    return 2;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // window pixels == client pixels
  g_probeDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::string variant = options.formatName;
  if (options.hdr10) variant += options.hdr10Early ? "_hdr10_early" : "_hdr10";
  if (options.present1) variant += "_present1";
  if (options.resizeAt >= 0) variant += "_resize";
  if (options.windowWidth != options.width || options.windowHeight != options.height) variant += "_scaled";
  if (options.renderThread) variant += "_thread";
  if (options.decoys) variant += "_decoys";
  if (options.unsubclassAt >= 0) variant += "_resubclass";
  if (options.faultAt >= 0) variant += "_fault";
  if (options.disableAt >= 0) variant += "_disable";
  ft::mod::openLog(executableDir() / ("overlay_harness_" + variant + ".log"));
  const int status = run(options);
  ft::mod::closeLog();
  return status;
}
