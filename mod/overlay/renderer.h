#pragma once
// Draws ImGui's draw data onto the game's back buffer from inside Present.
//
// ImGui renders into a private RGBA8 "canvas" the size of the back buffer (premultiplied
// alpha, sRGB-encoded like any ImGui output). A fullscreen-triangle pass then composites the
// canvas onto the back buffer, encoding it for the back buffer's format and colour space:
//   B8G8R8A8 / R8G8B8A8 / R10G10B10A2 (SDR)  -> passthrough
//   R16G16B16A16_FLOAT (scRGB)                -> linear, paper white / 80 nits
//   R10G10B10A2 + ST.2084 colour space (HDR10) -> Rec.2020 primaries, PQ
// so the overlay looks the same in SDR and HDR and imgui_impl_dx12 needs only one PSO.
//
// The renderer owns every D3D12 object it uses (created on the game's device) and the
// imgui_impl_dx12 backend. It holds no reference to the game's back buffers between frames
// (the game's ResizeBuffers would fail otherwise): the current one is fetched, drawn into and
// released within render(). Work is submitted on the game's present queue, so it is ordered
// before the Present that follows.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <vector>

struct ImDrawData;

namespace ft::mod::overlay {

class Renderer {
 public:
  Renderer() = default;
  ~Renderer() { shutdown(); }
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  ID3D12Device* device() const { return device_.Get(); }

  // Creates the device objects and initialises imgui_impl_dx12 (the ImGui context must be
  // current), after shutting down any previous device. Logs and returns false on failure.
  bool init(ID3D12Device* device);
  // Waits for in-flight overlay work, shuts the ImGui backend down, releases everything.
  // If the GPU does not finish in time, the objects it may still use are leaked instead and
  // false is returned: the overlay must then stop.
  bool shutdown();

  // Draws `drawData` onto the swap chain's current back buffer, submitting on `queue`.
  // `hdr10`: the swap chain's colour space is ST.2084 (only matters for R10G10B10A2).
  // Returns false on an error after which the overlay should stop (logged).
  bool render(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue, ImDrawData* drawData, bool hdr10);

  // Releases the back buffer render() may hold; also after a structured exception inside
  // render(), so the game can still resize its swap chain.
  void releaseAfterFault();

 private:
  // Generous because frame generation presents twice per rendered frame and the game may
  // queue several frames ahead: waiting on our own ring must never throttle the game.
  // imgui_impl_dx12 gets the same count, so its vertex-buffer ring and ours stay in step.
  static constexpr UINT kFramesInFlight = 8;
  static constexpr UINT kSrvHeapSize = 64;  // slot 0: canvas; the rest for ImGui textures
  static constexpr DXGI_FORMAT kCanvasFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  enum class Encoding : UINT { Sdr = 0, ScRgb = 1, Hdr10 = 2 };  // values match the shader

  struct Frame {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fenceValue = 0;
  };

  // Free list of the shader-visible heap's slots for ImGui's textures (InitInfo::UserData).
  struct SrvAllocator {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
    UINT increment = 0;
    std::vector<UINT> free;
    bool exhaustedLogged = false;
    void alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    void release(D3D12_CPU_DESCRIPTOR_HANDLE cpu);
  };

  bool createDeviceObjects();
  bool createCompositeShaders();
  bool initImGuiBackend();
  bool ensureCanvas(UINT width, UINT height);
  bool ensureCompositePso(DXGI_FORMAT format);
  bool waitForFence(UINT64 value);  // bounded: false (logged) if the GPU does not get there
  void abandon();                   // leaks every object (the GPU may still use them)
  bool waitIdle() { return waitForFence(lastSignaled_); }
  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  // Own queue for imgui_impl_dx12's texture uploads: the backend keeps the queue pointer it
  // was given, and the game's queue may go away with its swap chain.
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> uploadQueue_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  UINT64 lastSignaled_ = 0;
  Frame frames_[kFramesInFlight];
  UINT64 frameCount_ = 0;

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;  // 0: canvas, 1: back buffer (per frame)
  D3D12_CPU_DESCRIPTOR_HANDLE canvasRtv_{};
  D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv_{};
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
  SrvAllocator srvAlloc_;

  Microsoft::WRL::ComPtr<ID3D12Resource> canvas_;  // PIXEL_SHADER_RESOURCE between frames
  UINT canvasWidth_ = 0, canvasHeight_ = 0;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> compositeRootSignature_;
  Microsoft::WRL::ComPtr<ID3DBlob> compositeVs_, compositePs_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compositePso_;
  DXGI_FORMAT compositeFormat_ = DXGI_FORMAT_UNKNOWN;

  bool imguiBackend_ = false;
  DXGI_FORMAT unsupportedFormatLogged_ = DXGI_FORMAT_UNKNOWN;
  bool notRenderTargetLogged_ = false;
  int loggedEncoding_ = -1;
  ID3D12Resource* backBuffer_ = nullptr;  // only during render(); raw so a fault can release it
};

}  // namespace ft::mod::overlay
