// Present-hook side of the overlay: picks the game's swap chain, owns the ImGui context and
// backends, runs one ImGui frame per Present and hands the draw data to the renderer. Every
// frame runs under a structured-exception guard; any fault turns the overlay off for good
// and the game keeps presenting through the original functions.
#include "overlay.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "dxgi_hooks.h"
#include "input.h"
#include "overlay_state.h"
#include "renderer.h"
#include "support/log.h"

using Microsoft::WRL::ComPtr;

namespace ft::mod {

namespace overlay {

SharedState& shared() {
  static SharedState& s = *new SharedState;  // never destroyed: hooks may run during process exit
  return s;
}

}  // namespace overlay

namespace {

using namespace overlay;

// A newly presenting window replaces the current one only after the current one has not
// presented for this long (e.g. the game recreated its window), so a second visible swap
// chain (a launcher, a helper) cannot make the overlay flip between windows.
constexpr double kWindowSwitchIdleSeconds = 1.0;

enum class InstallState { NotStarted, Installing, Installed, Failed };

struct FaultInfo {
  DWORD code = 0;
  const void* address = nullptr;
};

// Present-thread state. Present can come from more than one thread (frame generation), so
// `busy` lets one thread at a time in; a Present that finds it taken is not drawn on.
struct PresentState {
  std::atomic<InstallState> install{InstallState::NotStarted};
  std::atomic<bool> busy{false};

  Renderer renderer;
  bool contextCreated = false;
  bool clientInitialised = false;
  HWND window = nullptr;         // the game window we draw for
  HWND backendWindow = nullptr;  // the window imgui_impl_win32 is initialised on
  double lastWindowPresent = 0.0;
  bool captured = false;  // client wanted input in the last frame
  const void* rejectedLogged = nullptr;  // last swap chain logged as not usable
  bool colorSpaceUnknownLogged = false;
};

PresentState& state() {
  static PresentState& s = *new PresentState;  // never destroyed, like shared()
  return s;
}

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void disableWithReason(const char* reason) {
  shared().disabled.store(true, std::memory_order_release);
  logf("overlay: disabled: {}", reason);
}

// Visible, not minimised, owned by this process, and either the window in use or a candidate
// to replace one that stopped presenting. The window is only claimed (claimWindow) once its
// swap chain has passed the D3D12 device and queue checks, so a D3D11 or splash swap chain
// presenting first cannot lock the game out.
bool eligibleWindow(HWND hwnd, double now) {
  const PresentState& p = state();
  if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return false;
  return hwnd == p.window || !p.window || now - p.lastWindowPresent >= kWindowSwitchIdleSeconds;
}

void claimWindow(HWND hwnd, double now) {
  PresentState& p = state();
  if (hwnd != p.window) {
    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, 64);
    char narrow[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, className, -1, narrow, static_cast<int>(sizeof narrow), nullptr, nullptr);
    logf("overlay: drawing into window {} (class \"{}\")", static_cast<const void*>(hwnd), narrow);
    p.window = hwnd;
  }
  p.lastWindowPresent = now;
}

// ImGui context, renderer on `device`, Win32 backend + input on `hwnd`, client init: each
// (re)done when missing or changed.
bool ensureImGui(HWND hwnd, ID3D12Device* device) {
  PresentState& p = state();
  SharedState& s = shared();
  if (!p.contextCreated) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the client persists its own settings
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // the game owns the OS cursor
    p.contextCreated = true;
    logf("overlay: ImGui {} context created", ImGui::GetVersion());
  }
  if (p.renderer.device() != device) {
    if (p.renderer.device()) logf("overlay: the game's D3D12 device changed; recreating the renderer");
    if (!p.renderer.init(device)) return false;
  }
  if (p.backendWindow != hwnd) {
    std::unique_lock lock(s.imguiMutex, std::defer_lock);
    if (!lock.try_lock_for(kImGuiLockTimeout)) return true;  // retried next frame
    s.inputWindow.store(nullptr, std::memory_order_release);  // stop feeding the old window
    if (p.backendWindow) ImGui_ImplWin32_Shutdown();
    p.backendWindow = nullptr;
    if (!ImGui_ImplWin32_Init(hwnd)) {
      logf("overlay: ImGui_ImplWin32_Init failed");
      return false;
    }
    // The IME hook would call Imm* on the game window from this (render) thread, which can
    // send messages to the window thread while it waits for imguiMutex.
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
    p.backendWindow = hwnd;
  }
  if (!p.clientInitialised) {
    std::unique_lock lock(s.imguiMutex, std::defer_lock);
    if (!lock.try_lock_for(kImGuiLockTimeout)) return true;
    if (s.disabled.load(std::memory_order_acquire)) return true;
    s.client->onImGuiInit();
    p.clientInitialised = true;
    logf("overlay: client initialised");
  }
  if (s.inputWindow.load(std::memory_order_acquire) != hwnd) {
    if (subclassWindow(hwnd)) s.inputWindow.store(hwnd, std::memory_order_release);
  } else {
    checkSubclass(hwnd);  // another mod may have restored the procedure we replaced
  }
  return true;
}

// One ImGui frame from the client, under the input lock. `scale`: back-buffer pixels per
// client pixel. False if skipped.
bool buildImGuiFrame(HWND hwnd, ImVec2 size, ImVec2 scale) {
  SharedState& s = shared();
  PresentState& p = state();
  std::unique_lock lock(s.imguiMutex, std::defer_lock);
  if (!lock.try_lock_for(kImGuiLockTimeout) || s.disabled.load(std::memory_order_acquire)) return false;
  ImGuiIO& io = ImGui::GetIO();
  const bool captured = s.client->wantsInput();
  if (takeDroppedRelease()) {  // a key-up or focus loss could not be delivered: nothing may stick
    io.ClearInputKeys();
    io.ClearInputMouse();
  }
  if (captured != p.captured) {  // mouse buttons start clean in either mode
    for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) io.AddMouseButtonEvent(button, false);
    p.captured = captured;
  }
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  io.DisplaySize = size;  // back buffer, not client area
  if (io.DeltaTime <= 0.0f) io.DeltaTime = 1e-6f;
  // Mouse position, polled: the window procedure forwards mouse messages only while capturing,
  // and this also replaces imgui_impl_win32's unscaled fallback position.
  POINT cursor{};
  if (GetForegroundWindow() == hwnd && GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor))
    io.AddMousePosEvent(static_cast<float>(cursor.x) * scale.x, static_cast<float>(cursor.y) * scale.y);
  else if (!captured)
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  io.MouseDrawCursor = captured;  // the game hides the OS cursor in flight
  ImGui::NewFrame();
  s.client->onFrame(size.x, size.y, nowSeconds());
  ImGui::Render();
  return true;
}

void drawFrame(IDXGISwapChain* swapChain) {
  PresentState& p = state();
  ComPtr<IDXGISwapChain3> swapChain3;
  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || FAILED(swapChain->GetDesc(&desc))) return;
  const HWND hwnd = desc.OutputWindow;
  const double now = nowSeconds();
  if (!eligibleWindow(hwnd, now)) return;

  ComPtr<ID3D12Device> device;
  if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
    if (p.rejectedLogged != swapChain)
      logf("overlay: swap chain {} (window {}) is not D3D12; ignored", static_cast<const void*>(swapChain),
           static_cast<const void*>(hwnd));
    p.rejectedLogged = swapChain;
    return;
  }
  ID3D12CommandQueue* queue = findPresentQueue(swapChain, device.Get());
  if (!queue) {
    if (p.rejectedLogged != swapChain)
      logf("overlay: no validated command queue for swap chain {}; ignored", static_cast<const void*>(swapChain));
    p.rejectedLogged = swapChain;
    return;
  }
  claimWindow(hwnd, now);

  if (!ensureImGui(hwnd, device.Get())) {
    disableWithReason("initialisation failed");
    return;
  }
  if (!p.clientInitialised || p.backendWindow != hwnd) return;

  const ImVec2 size(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
  RECT client{};
  ImVec2 scale(1.0f, 1.0f);
  if (GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0)
    scale = ImVec2(size.x / static_cast<float>(client.right), size.y / static_cast<float>(client.bottom));
  setMouseScale(scale.x, scale.y);
  if (!buildImGuiFrame(hwnd, size, scale)) return;

  DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  const bool colorSpaceKnown = swapChainColorSpace(swapChain, &colorSpace);
  if (!colorSpaceKnown && desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && !p.colorSpaceUnknownLogged) {
    logf("overlay: colour space unknown, assuming SDR");
    p.colorSpaceUnknownLogged = true;
  }
  const bool hdr10 = colorSpaceKnown && colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  // Outside the input lock: the window thread only queues IO events, which the draw data and
  // the texture list do not depend on.
  if (!p.renderer.render(swapChain3.Get(), queue, ImGui::GetDrawData(), hdr10)) disableWithReason("rendering failed");
}
// Not noexcept: a structured exception must be able to pass through to guardedDrawFrame.
void drawFrameNoThrow(IDXGISwapChain* swapChain) {
  try {
    drawFrame(swapChain);
  } catch (const std::exception& e) {
    shared().disabled.store(true, std::memory_order_release);
    try {
      logf("overlay: disabled after exception: {}", e.what());
    } catch (...) {
    }
  } catch (...) {
    shared().disabled.store(true, std::memory_order_release);
  }
}

int captureFault(const EXCEPTION_POINTERS* info, FaultInfo* fault) {
  fault->code = info->ExceptionRecord->ExceptionCode;
  fault->address = info->ExceptionRecord->ExceptionAddress;
  return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects here: __try forbids unwinding.
bool guardedDrawFrame(IDXGISwapChain* swapChain, FaultInfo* fault) {
  __try {
    drawFrameNoThrow(swapChain);
    return true;
  } __except (captureFault(GetExceptionInformation(), fault)) {
    return false;
  }
}

bool guardedReleaseAfterFault() {
  __try {
    state().renderer.releaseAfterFault();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void onPresent(IDXGISwapChain* swapChain) {
  SharedState& s = shared();
  PresentState& p = state();
  if (s.disabled.load(std::memory_order_acquire)) return;
  if (p.busy.exchange(true, std::memory_order_acquire)) return;
  FaultInfo fault;
  if (!guardedDrawFrame(swapChain, &fault)) {
    // Whatever was half done stays that way (locks included): nothing touches it again.
    s.disabled.store(true, std::memory_order_release);
    guardedReleaseAfterFault();
    try {  // nothing may escape into the game's Present
      logf("overlay: disabled after structured exception 0x{:08X} at {}", fault.code, describeAddress(fault.address));
    } catch (...) {
    }
  }
  p.busy.store(false, std::memory_order_release);
}

// Our module stays loaded for the life of the process: the patched vtable points into it.
void pinModule() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(&pinModule), &module))
    logf("overlay: module pinned");
  else
    logf("overlay: pinning the module failed (error {})", GetLastError());
}

void installThread() {
  bool ok = false;
  try {  // an exception leaving a thread function ends the process
    pinModule();
    ok = installDxgiHooks(&onPresent);
    if (ok)
      logf("overlay: hooks installed");
    else
      logf("overlay: hook installation failed; no overlay this session");
  } catch (...) {
    ok = false;
  }
  state().install.store(ok ? InstallState::Installed : InstallState::Failed, std::memory_order_release);
}

}  // namespace

bool startOverlay(OverlayClient& client) {
  PresentState& p = state();
  InstallState expected = InstallState::NotStarted;
  if (!p.install.compare_exchange_strong(expected, InstallState::Installing)) {
    logf("overlay: startOverlay called again; ignored");
    return expected != InstallState::Failed;
  }
  shared().client = &client;  // before any hook can run
  try {
    std::thread(installThread).detach();
  } catch (const std::system_error& e) {
    logf("overlay: starting the install thread failed: {}", e.what());
    p.install.store(InstallState::Failed, std::memory_order_release);
    return false;
  }
  return true;
}

void disableOverlay() {
  SharedState& s = shared();
  s.disabled.store(true, std::memory_order_release);
  // A frame or window message that entered before the flag was set may still be calling the
  // client; both do so only while holding imguiMutex (recursive: fine from inside the client).
  std::unique_lock lock(s.imguiMutex, std::defer_lock);
  if (!lock.try_lock_for(std::chrono::seconds(1))) logf("overlay: disableOverlay: timed out waiting for the client");
  logf("overlay: disabled by request");
}

bool overlayInstalled() {
  return state().install.load(std::memory_order_acquire) == InstallState::Installed;
}

}  // namespace ft::mod
