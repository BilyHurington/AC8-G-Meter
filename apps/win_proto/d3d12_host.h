#pragma once
// Direct3D 12 device + flip-model swap chain for the prototype window, Dear ImGui's DX12
// backend on top of it, and a back-buffer read-back for screenshots. Based on
// imgui/examples/example_win32_directx12. In game the mod draws into the game's own swap
// chain instead; this host only stands in for it.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

struct ImDrawData;

namespace ft {

// One frame as BGRA8, rows tightly packed (width * 4 bytes), alpha forced opaque.
struct CapturedImage {
  UINT width = 0, height = 0;
  std::vector<std::uint8_t> bgra;
};

class D3D12Host {
 public:
  D3D12Host() = default;
  ~D3D12Host();
  D3D12Host(const D3D12Host&) = delete;
  D3D12Host& operator=(const D3D12Host&) = delete;

  bool create(HWND hwnd);  // device, queue, swap chain, render targets
  // After ImGui::CreateContext() / before the first ImGui frame, and before destroying the
  // ImGui context respectively.
  bool initImGuiBackend();
  void shutdownImGuiBackend();

  void resize(UINT width, UINT height);  // WM_SIZE; ignored before create() and for 0 sizes
  // True while the last Present() reported the window occluded (minimised, locked screen)
  // and it still is: skip the frame.
  bool occluded();
  // Draws `drawData` over a black back buffer and presents it. With `capture`, the frame is
  // also copied into it (waits for the GPU).
  void renderAndPresent(ImDrawData* drawData, bool vsync, CapturedImage* capture);
  void waitIdle();  // all submitted GPU work completed

 private:
  static constexpr UINT kFramesInFlight = 2;
  static constexpr UINT kBackBuffers = 2;
  static constexpr UINT kSrvHeapSize = 64;

  struct FrameContext {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fenceValue = 0;
  };

  // Free list of the shader-visible SRV heap's slots, handed to the ImGui backend (textures).
  struct SrvAllocator {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
    UINT increment = 0;
    std::vector<UINT> free;
    void alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    void release(D3D12_CPU_DESCRIPTOR_HANDLE cpu);
  };

  void createRenderTargets();
  void releaseRenderTargets();  // waits for the GPU first
  FrameContext& waitForNextFrameContext();
  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  // Back buffer copied into a CPU-readable buffer (null `buffer` if it could not be created).
  struct Readback {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    UINT width = 0, height = 0;
    bool read(CapturedImage& out) const;  // after the copy completed on the GPU
  };
  Readback copyBackBuffer(ID3D12Resource* backBuffer);  // records the copy into the command list

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
  SrvAllocator srvAlloc_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  UINT64 lastSignaled_ = 0;
  FrameContext frames_[kFramesInFlight];
  UINT frameIndex_ = 0;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
  HANDLE frameLatencyWaitable_ = nullptr;
  bool tearingSupported_ = false;
  bool occluded_ = false;
  Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers_[kBackBuffers];
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles_[kBackBuffers]{};

  bool imguiInitialised_ = false;
};

}  // namespace ft
