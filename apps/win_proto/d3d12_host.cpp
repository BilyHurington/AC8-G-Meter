#include "d3d12_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_dx12.h>

using Microsoft::WRL::ComPtr;

namespace ft {

namespace {

// BGRA: the screenshot read-back is written to PNG without swizzling.
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

}  // namespace

void D3D12Host::SrvAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
  if (free.empty()) {  // the backend has no failure path; 64 slots are far more than it uses
    std::fprintf(stderr, "D3D12Host: SRV descriptor heap exhausted\n");
    std::abort();
  }
  const UINT index = free.back();
  free.pop_back();
  cpu->ptr = cpuStart.ptr + static_cast<SIZE_T>(index) * increment;
  gpu->ptr = gpuStart.ptr + static_cast<UINT64>(index) * increment;
}

void D3D12Host::SrvAllocator::release(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
  free.push_back(static_cast<UINT>((cpu.ptr - cpuStart.ptr) / increment));
}

D3D12Host::~D3D12Host() {
  if (queue_ && fence_ && fenceEvent_) waitIdle();
  releaseRenderTargets();
  if (swapChain_) swapChain_->SetFullscreenState(FALSE, nullptr);
  if (frameLatencyWaitable_) CloseHandle(frameLatencyWaitable_);
  if (fenceEvent_) CloseHandle(fenceEvent_);
}

bool D3D12Host::create(HWND hwnd) {
#if defined(_DEBUG)
  if (ComPtr<ID3D12Debug> debug; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) return false;

  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = kBackBuffers;
  if (FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)))) return false;
  const UINT rtvIncrement = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < kBackBuffers; ++i) {
    rtvHandles_[i] = rtv;
    rtv.ptr += rtvIncrement;
  }

  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = kSrvHeapSize;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_)))) return false;
  srvAlloc_.cpuStart = srvHeap_->GetCPUDescriptorHandleForHeapStart();
  srvAlloc_.gpuStart = srvHeap_->GetGPUDescriptorHandleForHeapStart();
  srvAlloc_.increment = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (UINT i = kSrvHeapSize; i > 0; --i) srvAlloc_.free.push_back(i - 1);

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)))) return false;
  for (FrameContext& f : frames_)
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) return false;
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commandList_))) ||
      FAILED(commandList_->Close()))
    return false;
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fenceEvent_) return false;

  ComPtr<IDXGIFactory5> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  BOOL allowTearing = FALSE;
  tearingSupported_ = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                             sizeof allowTearing)) &&
                      allowTearing;

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.BufferCount = kBackBuffers;
  sd.Format = kBackBufferFormat;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.SampleDesc.Count = 1;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (tearingSupported_) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  ComPtr<IDXGISwapChain1> swapChain1;
  if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &swapChain1)) ||
      FAILED(swapChain1.As(&swapChain_)))
    return false;
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  swapChain_->SetMaximumFrameLatency(kBackBuffers);
  frameLatencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();

  createRenderTargets();
  return true;
}

bool D3D12Host::initImGuiBackend() {
  ImGui_ImplDX12_InitInfo info;
  info.Device = device_.Get();
  info.CommandQueue = queue_.Get();
  info.NumFramesInFlight = static_cast<int>(kFramesInFlight);
  info.RTVFormat = kBackBufferFormat;
  info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  info.UserData = &srvAlloc_;
  info.SrvDescriptorHeap = srvHeap_.Get();
  info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    static_cast<SrvAllocator*>(i->UserData)->alloc(cpu, gpu);
  };
  info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE) { static_cast<SrvAllocator*>(i->UserData)->release(cpu); };
  imguiInitialised_ = ImGui_ImplDX12_Init(&info);
  // Init cannot fail; the root signature, shaders and PSO are created lazily in the first
  // NewFrame behind an assert. Create them now so a failure is reported here.
  return imguiInitialised_ && ImGui_ImplDX12_CreateDeviceObjects();
}

void D3D12Host::shutdownImGuiBackend() {
  if (!imguiInitialised_) return;
  waitIdle();  // the backend releases buffers the GPU may still read
  ImGui_ImplDX12_Shutdown();
  imguiInitialised_ = false;
}

void D3D12Host::createRenderTargets() {
  for (UINT i = 0; i < kBackBuffers; ++i) {
    swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
    device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtvHandles_[i]);
  }
}

void D3D12Host::releaseRenderTargets() {
  if (queue_ && fence_ && fenceEvent_) waitIdle();
  for (auto& b : backBuffers_) b.Reset();
}

void D3D12Host::resize(UINT width, UINT height) {
  if (!swapChain_ || width == 0 || height == 0) return;
  releaseRenderTargets();
  DXGI_SWAP_CHAIN_DESC1 desc{};
  swapChain_->GetDesc1(&desc);
  const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, desc.Format, desc.Flags);
  if (FAILED(hr)) {  // no back buffers to render into: nothing sensible left to do
    std::fprintf(stderr, "D3D12Host: ResizeBuffers failed (HRESULT 0x%08lX)\n", static_cast<unsigned long>(hr));
    std::abort();
  }
  createRenderTargets();
}

bool D3D12Host::occluded() {
  if (occluded_ && swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return true;
  occluded_ = false;
  return false;
}

void D3D12Host::waitIdle() {
  queue_->Signal(fence_.Get(), ++lastSignaled_);
  fence_->SetEventOnCompletion(lastSignaled_, fenceEvent_);
  WaitForSingleObject(fenceEvent_, INFINITE);
}

D3D12Host::FrameContext& D3D12Host::waitForNextFrameContext() {
  FrameContext& f = frames_[frameIndex_ % kFramesInFlight];
  if (fence_->GetCompletedValue() < f.fenceValue) {
    fence_->SetEventOnCompletion(f.fenceValue, fenceEvent_);
    const HANDLE waitables[] = {frameLatencyWaitable_, fenceEvent_};
    WaitForMultipleObjects(2, waitables, TRUE, INFINITE);
  } else {
    WaitForSingleObject(frameLatencyWaitable_, INFINITE);
  }
  return f;
}

void D3D12Host::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  commandList_->ResourceBarrier(1, &b);
}

D3D12Host::Readback D3D12Host::copyBackBuffer(ID3D12Resource* backBuffer) {
  Readback r;
  const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
  // totalBytes: the last row is not padded to the row pitch.
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &r.totalBytes);
  r.width = static_cast<UINT>(desc.Width);
  r.height = desc.Height;

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buf{};
  buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf.Width = r.totalBytes;
  buf.Height = 1;
  buf.DepthOrArraySize = 1;
  buf.MipLevels = 1;
  buf.SampleDesc.Count = 1;
  buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&r.buffer))))
    return {};

  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = r.buffer.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = r.footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = backBuffer;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  commandList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  return r;
}

bool D3D12Host::Readback::read(CapturedImage& out) const {
  out.bgra.clear();
  if (!buffer) return false;
  const SIZE_T offset = static_cast<SIZE_T>(footprint.Offset);
  const D3D12_RANGE readRange{offset, offset + static_cast<SIZE_T>(totalBytes)};
  void* mapped = nullptr;
  if (FAILED(buffer->Map(0, &readRange, &mapped))) return false;
  out.width = width;
  out.height = height;
  const UINT rowBytes = width * 4;
  out.bgra.resize(static_cast<std::size_t>(rowBytes) * height);
  const auto* src = static_cast<const std::uint8_t*>(mapped) + offset;
  for (UINT y = 0; y < height; ++y) {
    std::uint8_t* row = out.bgra.data() + static_cast<std::size_t>(y) * rowBytes;
    std::memcpy(row, src + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, rowBytes);
    for (UINT x = 0; x < width; ++x) row[x * 4 + 3] = 0xFF;
  }
  const D3D12_RANGE noWrite{0, 0};
  buffer->Unmap(0, &noWrite);
  return true;
}

void D3D12Host::renderAndPresent(ImDrawData* drawData, bool vsync, CapturedImage* capture) {
  FrameContext& frame = waitForNextFrameContext();
  const UINT index = swapChain_->GetCurrentBackBufferIndex();
  ID3D12Resource* backBuffer = backBuffers_[index].Get();

  frame.allocator->Reset();
  commandList_->Reset(frame.allocator.Get(), nullptr);
  transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  commandList_->ClearRenderTargetView(rtvHandles_[index], black, 0, nullptr);
  commandList_->OMSetRenderTargets(1, &rtvHandles_[index], FALSE, nullptr);
  ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
  commandList_->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(drawData, commandList_.Get());
  Readback readback;
  if (capture) {
    transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    readback = copyBackBuffer(backBuffer);
    transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  } else {
    transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  }
  commandList_->Close();
  ID3D12CommandList* lists[] = {commandList_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  queue_->Signal(fence_.Get(), ++lastSignaled_);
  frame.fenceValue = lastSignaled_;

  if (capture) {
    waitIdle();
    readback.read(*capture);  // leaves capture->bgra empty on failure
  }

  const UINT flags = !vsync && tearingSupported_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
  occluded_ = swapChain_->Present(vsync ? 1 : 0, flags) == DXGI_STATUS_OCCLUDED;
  ++frameIndex_;
}

}  // namespace ft
