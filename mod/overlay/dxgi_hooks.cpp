#include "dxgi_hooks.h"

#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

#include "support/log.h"

using Microsoft::WRL::ComPtr;

namespace ft::mod::overlay {

namespace {

// Vtable slots: IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-28, IDXGISwapChain2 29-35, IDXGISwapChain3 36-39.
constexpr std::size_t kSlotPresent = 8;
constexpr std::size_t kSlotPresent1 = 22;
constexpr std::size_t kSlotSetColorSpace1 = 38;

// How far into the swap chain object to look for the fields we need, and how many matching
// offsets to keep (each is validated on use, so a stale or coincidental one is harmless).
constexpr std::size_t kScanBytes = 0x1000;
constexpr std::size_t kMaxOffsets = 4;

// Largest defined DXGI_COLOR_SPACE_TYPE: anything above is not a colour space field.
constexpr UINT kMaxColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020;

// Private data key under which the SetColorSpace1 hook stores the colour space.
constexpr GUID kColorSpaceGuid = {0x7d3f5a2e, 0x9c41, 0x4b8e, {0xa6, 0xd2, 0x3e, 0x5f, 0x1c, 0x8b, 0x9a, 0x47}};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

struct Offsets {
  std::array<std::size_t, kMaxOffsets> at{};
  std::size_t count = 0;
};

// Written by installDxgiHooks() before the vtable slots are patched (the interlocked slot
// write publishes them), read-only afterwards.
struct HookState {
  std::mutex installMutex;
  bool installed = false;
  PresentCallback onPresent = nullptr;
  PresentFn present = nullptr;
  Present1Fn present1 = nullptr;
  SetColorSpace1Fn setColorSpace1 = nullptr;
  Offsets queueOffsets;
  const void* queueVtable = nullptr;
  Offsets colorSpaceOffsets;
};

HookState g;

// Re-entrancy guard: a Present issued from inside Present (another overlay, a nested swap
// chain) goes straight to the original.
thread_local bool t_inPresent = false;

// Reads memory we do not own (the game's swap chain object); false instead of crashing if the
// address is not readable. No C++ objects here: __try forbids unwinding.
bool readPointer(const void* base, std::size_t offset, void** out) {
  __try {
    *out = *reinterpret_cast<void* const*>(static_cast<const std::byte*>(base) + offset);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool readU32(const void* base, std::size_t offset, std::uint32_t* out) {
  __try {
    *out = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(base) + offset);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool drawsOn(UINT presentFlags) {
  // TEST only asks for occlusion status; DO_NOT_SEQUENCE re-shows a frame we already drew on.
  return (presentFlags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE)) == 0;
}

// The guard is reset in __finally so that an exception the game handles above Present (C++ or
// structured) cannot leave it set. No C++ objects in these functions.
HRESULT STDMETHODCALLTYPE hookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  if (t_inPresent) return g.present(swapChain, syncInterval, flags);
  t_inPresent = true;
  __try {
    if (drawsOn(flags)) g.onPresent(swapChain);
    return g.present(swapChain, syncInterval, flags);
  } __finally {
    t_inPresent = false;
  }
}

HRESULT STDMETHODCALLTYPE hookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
  if (t_inPresent) return g.present1(swapChain, syncInterval, flags, params);
  t_inPresent = true;
  __try {
    if (drawsOn(flags)) g.onPresent(swapChain);
    return g.present1(swapChain, syncInterval, flags, params);
  } __finally {
    t_inPresent = false;
  }
}

void recordColorSpace(IDXGISwapChain3* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace) {
  UINT previous = 0;
  UINT size = sizeof previous;
  const bool known = SUCCEEDED(swapChain->GetPrivateData(kColorSpaceGuid, &size, &previous));
  const UINT value = static_cast<UINT>(colorSpace);
  swapChain->SetPrivateData(kColorSpaceGuid, sizeof value, &value);
  if (known && previous == value) return;
  try {
    logf("overlay: SetColorSpace1({}) on swap chain {}", value, static_cast<const void*>(swapChain));
  } catch (...) {  // nothing may escape into the game
  }
}

// The authoritative source when it fires; discovery (below) covers calls made before install.
HRESULT STDMETHODCALLTYPE hookSetColorSpace1(IDXGISwapChain3* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace) {
  const HRESULT hr = g.setColorSpace1(swapChain, colorSpace);
  if (SUCCEEDED(hr)) recordColorSpace(swapChain, colorSpace);
  return hr;
}

// Replaces one vtable entry (read-only data) and returns the previous one in `original`.
template <class Fn>
bool patchSlot(void** vtable, std::size_t slot, Fn hook, Fn* original, const char* name) {
  void** entry = vtable + slot;
  if (*entry == reinterpret_cast<void*>(hook)) {
    logf("overlay: {} already hooked", name);
    return true;
  }
  DWORD oldProtect = 0;
  if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
    logf("overlay: VirtualProtect on the {} slot failed (error {})", name, GetLastError());
    return false;
  }
  *original = reinterpret_cast<Fn>(*entry);
  // Publishes `original` before the hook becomes reachable; the game may be presenting now.
  InterlockedExchangePointer(entry, reinterpret_cast<void*>(hook));
  VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
  logf("overlay: hooked {} (original at {})", name, describeAddress(reinterpret_cast<const void*>(*original)));
  return true;
}

// Hidden 64x64 window the dummy swap chain is created for.
class DummyWindow {
 public:
  DummyWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    registered_ = RegisterClassExW(&wc) != 0;
    hwnd_ = CreateWindowExW(0, kClassName, L"AC8 G-Meter DXGI probe", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                            wc.hInstance, nullptr);
  }
  ~DummyWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (registered_) UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
  }
  DummyWindow(const DummyWindow&) = delete;
  DummyWindow& operator=(const DummyWindow&) = delete;
  HWND hwnd() const { return hwnd_; }

 private:
  static constexpr const wchar_t* kClassName = L"AC8GMeterDxgiProbe";
  bool registered_ = false;
  HWND hwnd_ = nullptr;
};

// Records the offsets at which the swap chain object stores the queue it was created with.
void scanQueueOffsets(const void* swapChain, const void* queue) {
  g.queueOffsets = {};
  for (std::size_t offset = 0; offset < kScanBytes && g.queueOffsets.count < kMaxOffsets; offset += sizeof(void*)) {
    void* value = nullptr;
    if (!readPointer(swapChain, offset, &value)) break;  // end of readable memory
    if (value == queue) {
      g.queueOffsets.at[g.queueOffsets.count++] = offset;
      logf("overlay: present queue found at swap chain +0x{:X}", offset);
    }
  }
}

// Sets alternating colour spaces on the (R10G10B10A2) dummy and keeps the 32-bit fields that
// hold exactly the value just set every time. UE sets its colour space when it creates or
// resizes the swap chain, usually before our DLL is loaded, so the hook alone would miss it.
void scanColorSpaceOffsets(IDXGISwapChain3* swapChain, const void* base) {
  g.colorSpaceOffsets = {};
  UINT support = 0;
  if (FAILED(swapChain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support)) ||
      !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
    logf("overlay: ST.2084 not supported on the dummy swap chain; colour space discovery skipped");
    return;
  }
  constexpr DXGI_COLOR_SPACE_TYPE kSequence[] = {
      DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
      DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
  std::vector<bool> candidate(kScanBytes / sizeof(std::uint32_t), true);
  for (const DXGI_COLOR_SPACE_TYPE colorSpace : kSequence) {
    if (FAILED(swapChain->SetColorSpace1(colorSpace))) {
      logf("overlay: SetColorSpace1 failed on the dummy swap chain; colour space discovery skipped");
      return;
    }
    for (std::size_t i = 0; i < candidate.size(); ++i) {
      std::uint32_t value = 0;
      if (candidate[i] && (!readU32(base, i * sizeof value, &value) || value != static_cast<UINT>(colorSpace)))
        candidate[i] = false;
    }
  }
  for (std::size_t i = 0; i < candidate.size() && g.colorSpaceOffsets.count < kMaxOffsets; ++i) {
    if (!candidate[i]) continue;
    g.colorSpaceOffsets.at[g.colorSpaceOffsets.count++] = i * sizeof(std::uint32_t);
    logf("overlay: colour space found at swap chain +0x{:X}", i * sizeof(std::uint32_t));
  }
  if (g.colorSpaceOffsets.count == 0) logf("overlay: colour space field not found in the swap chain");
}

bool installLocked() {
  DummyWindow window;
  if (!window.hwnd()) {
    logf("overlay: creating the dummy window failed (error {})", GetLastError());
    return false;
  }
  ComPtr<ID3D12Device> device;
  HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr)) {
    logf("overlay: D3D12CreateDevice failed (0x{:08X})", static_cast<unsigned>(hr));
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
  if (FAILED(hr)) {
    logf("overlay: CreateCommandQueue failed (0x{:08X})", static_cast<unsigned>(hr));
    return false;
  }
  ComPtr<IDXGIFactory2> factory;
  hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    logf("overlay: CreateDXGIFactory2 failed (0x{:08X})", static_cast<unsigned>(hr));
    return false;
  }
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = 64;
  sd.Height = 64;
  sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;  // a format that accepts ST.2084, for colour space discovery
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swapChain;
  hr = factory->CreateSwapChainForHwnd(queue.Get(), window.hwnd(), &sd, nullptr, nullptr, &swapChain);
  if (FAILED(hr)) {  // the hooks need any swap chain; colour space discovery then finds nothing
    logf("overlay: 10-bit dummy swap chain failed (0x{:08X}); retrying with 8 bits", static_cast<unsigned>(hr));
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    hr = factory->CreateSwapChainForHwnd(queue.Get(), window.hwnd(), &sd, nullptr, nullptr, &swapChain);
  }
  if (FAILED(hr)) {
    logf("overlay: creating the dummy swap chain failed (0x{:08X})", static_cast<unsigned>(hr));
    return false;
  }
  ComPtr<IDXGISwapChain3> swapChain3;
  if (FAILED(swapChain.As(&swapChain3))) {
    logf("overlay: IDXGISwapChain3 not supported");
    return false;
  }

  scanQueueOffsets(swapChain.Get(), queue.Get());
  if (g.queueOffsets.count == 0) {
    logf("overlay: the swap chain's command queue pointer was not found; not hooking");
    return false;
  }
  g.queueVtable = *reinterpret_cast<void* const*>(queue.Get());
  scanColorSpaceOffsets(swapChain3.Get(), swapChain.Get());  // optional: HDR10 then needs the hook

  // IDXGISwapChain1 and IDXGISwapChain3 normally share one vtable; read each slot through the
  // interface it belongs to in case they do not. Present first: without it nothing works, so
  // nothing else is patched if it fails.
  void** vtable1 = *reinterpret_cast<void***>(swapChain.Get());
  void** vtable3 = *reinterpret_cast<void***>(swapChain3.Get());
  if (!patchSlot(vtable1, kSlotPresent, &hookPresent, &g.present, "IDXGISwapChain::Present")) return false;
  if (!patchSlot(vtable1, kSlotPresent1, &hookPresent1, &g.present1, "IDXGISwapChain1::Present1"))
    logf("overlay: Present1 not hooked; games presenting with Present1 get no overlay");
  if (!patchSlot(vtable3, kSlotSetColorSpace1, &hookSetColorSpace1, &g.setColorSpace1,
                 "IDXGISwapChain3::SetColorSpace1"))
    logf("overlay: SetColorSpace1 not hooked; relying on colour space discovery");
  return true;
  // The dummy objects are released here; the shared vtable stays patched.
}

bool readColorSpaceField(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE* out) {
  if (g.colorSpaceOffsets.count == 0) return false;
  std::uint32_t first = 0;
  for (std::size_t i = 0; i < g.colorSpaceOffsets.count; ++i) {
    std::uint32_t value = 0;
    if (!readU32(swapChain, g.colorSpaceOffsets.at[i], &value) || value > kMaxColorSpace) return false;
    if (i == 0) first = value;
    if (value != first) return false;  // copies disagree: not the field we think it is
  }
  *out = static_cast<DXGI_COLOR_SPACE_TYPE>(first);
  return true;
}

}  // namespace

bool installDxgiHooks(PresentCallback onPresent) {
  std::lock_guard lock(g.installMutex);
  if (g.installed) return true;
  g.onPresent = onPresent;
  g.installed = installLocked();
  return g.installed;
}

ID3D12CommandQueue* findPresentQueue(IDXGISwapChain* swapChain, ID3D12Device* device) {
  for (std::size_t i = 0; i < g.queueOffsets.count; ++i) {
    void* candidate = nullptr;
    void* vtable = nullptr;
    if (!readPointer(swapChain, g.queueOffsets.at[i], &candidate) || !candidate) continue;
    if (!readPointer(candidate, 0, &vtable) || vtable != g.queueVtable) continue;
    auto* queue = static_cast<ID3D12CommandQueue*>(candidate);
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device) continue;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) continue;
    return queue;
  }
  return nullptr;
}

bool swapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE* out) {
  UINT value = 0;
  UINT size = sizeof value;
  if (SUCCEEDED(swapChain->GetPrivateData(kColorSpaceGuid, &size, &value)) && size == sizeof value) {
    *out = static_cast<DXGI_COLOR_SPACE_TYPE>(value);
    return true;
  }
  return readColorSpaceField(swapChain, out);
}

std::string describeAddress(const void* address) noexcept {
  try {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
      return std::format("{}", address);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const std::u8string name = std::filesystem::path(path).filename().u8string();
    const auto offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
    return std::format("{}+0x{:X}", std::string(name.begin(), name.end()), offset);
  } catch (...) {
    return {};
  }
}

}  // namespace ft::mod::overlay
