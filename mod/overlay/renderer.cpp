#include "renderer.h"

#include <d3dcompiler.h>

#include <bit>
#include <climits>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include "support/log.h"

using Microsoft::WRL::ComPtr;

namespace ft::mod::overlay {

namespace {

// Brightness of the overlay's white in HDR output (scRGB 1.0 = 80 nits). 200 nits sits with
// typical game UI paper white; SDR output is unaffected.
constexpr float kPaperWhiteNits = 200.0f;

// Longest we wait for the GPU before giving up on the overlay (only reached on a hung or
// removed device; normally a wait covers at most a few frames).
constexpr DWORD kGpuTimeoutMs = 2000;

// The canvas holds premultiplied, sRGB-encoded colour (ImGui's blend state on a transparent
// target produces exactly that), so compositing is "src + dst * (1 - src.a)" after encoding.
constexpr char kCompositeHlsl[] = R"hlsl(
Texture2D<float4> canvas : register(t0);
cbuffer Composite : register(b0) {
  uint encoding;        // 0: SDR passthrough, 1: scRGB, 2: HDR10 (PQ, Rec.2020)
  float paperWhiteNits;
};

float4 vsMain(uint id : SV_VertexID) : SV_Position {
  // One triangle covering the viewport: (-1, 1), (3, 1), (-1, -3).
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

float3 srgbToLinear(float3 c) {
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// SMPTE ST 2084 inverse EOTF; input in units of 10000 nits.
float3 pqEncode(float3 y) {
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 p = pow(saturate(y), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float4 psMain(float4 pos : SV_Position) : SV_Target {
  float4 c = canvas.Load(int3(int2(pos.xy), 0));
  if (encoding == 0 || c.a <= 0.0) return c;
  float3 lin = srgbToLinear(saturate(c.rgb / c.a));
  float3 encoded;
  if (encoding == 1) {
    encoded = lin * (paperWhiteNits / 80.0);
  } else {
    const float3x3 rec709To2020 = {
      0.6274040, 0.3292820, 0.0433136,
      0.0690970, 0.9195400, 0.0113612,
      0.0163916, 0.0880132, 0.8955950};
    encoded = pqEncode(mul(rec709To2020, lin) * (paperWhiteNits / 10000.0));
  }
  return float4(encoded * c.a, c.a);
}
)hlsl";

const char* formatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
  }
}

// Uploads only. A WantDestroy texture stays in that state until the backend has not used it for
// NumFramesInFlight frames, so its release is already safe; waiting for it would stall the GPU
// on every one of those frames.
bool hasPendingTextureWork(const ImDrawData* drawData) {
  if (!drawData->Textures) return false;
  for (const ImTextureData* tex : *drawData->Textures)
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) return true;
  return false;
}

ComPtr<ID3DBlob> compileShader(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(kCompositeHlsl, sizeof kCompositeHlsl - 1, "overlay_composite", nullptr, nullptr,
                                entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    logf("overlay: compiling {} failed (0x{:08X}): {}", entry, static_cast<unsigned>(hr),
         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler output");
    return nullptr;
  }
  return code;
}

}  // namespace

void Renderer::SrvAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
  UINT index = kSrvHeapSize - 1;
  if (!free.empty()) {
    index = free.back();
    free.pop_back();
  } else if (!exhaustedLogged) {
    // The backend has no failure path. Reusing a slot only garbles a texture; ImGui uses one
    // or two of the 63 slots, so this is not expected to happen.
    logf("overlay: SRV descriptor heap exhausted; reusing the last slot");
    exhaustedLogged = true;
  }
  cpu->ptr = cpuStart.ptr + static_cast<SIZE_T>(index) * increment;
  gpu->ptr = gpuStart.ptr + static_cast<UINT64>(index) * increment;
}

void Renderer::SrvAllocator::release(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
  free.push_back(static_cast<UINT>((cpu.ptr - cpuStart.ptr) / increment));
}

bool Renderer::init(ID3D12Device* device) {
  if (!shutdown()) return false;
  device_ = device;
  if (!createDeviceObjects() || !createCompositeShaders() || !initImGuiBackend()) {
    shutdown();
    return false;
  }
  logf("overlay: renderer initialised on device {}", static_cast<const void*>(device));
  return true;
}

bool Renderer::shutdown() {
  if (!device_) return true;
  if (fence_ && fenceEvent_ && !waitIdle()) {  // the backend and canvas may still be in use
    abandon();
    return false;
  }
  if (imguiBackend_) ImGui_ImplDX12_Shutdown();
  imguiBackend_ = false;
  compositePso_.Reset();
  compositeFormat_ = DXGI_FORMAT_UNKNOWN;
  compositeVs_.Reset();
  compositePs_.Reset();
  compositeRootSignature_.Reset();
  canvas_.Reset();
  canvasWidth_ = canvasHeight_ = 0;
  srvHeap_.Reset();
  srvAlloc_ = {};
  rtvHeap_.Reset();
  for (Frame& f : frames_) f = {};
  frameCount_ = 0;
  if (fenceEvent_) CloseHandle(fenceEvent_);
  fenceEvent_ = nullptr;
  fence_.Reset();
  lastSignaled_ = 0;
  commandList_.Reset();
  uploadQueue_.Reset();
  device_.Reset();
  return true;
}

void Renderer::abandon() {
  logf("overlay: leaking the renderer's GPU objects (the GPU may still be using them)");
  // Detach without Release. imgui_impl_dx12 keeps its objects too: it is never called again.
  for (Frame& f : frames_) f.allocator.Detach();
  compositePso_.Detach();
  compositeVs_.Detach();
  compositePs_.Detach();
  compositeRootSignature_.Detach();
  canvas_.Detach();
  srvHeap_.Detach();
  rtvHeap_.Detach();
  fence_.Detach();
  fenceEvent_ = nullptr;
  commandList_.Detach();
  uploadQueue_.Detach();
  device_.Detach();
}

bool Renderer::createDeviceObjects() {
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&uploadQueue_)))) {
    logf("overlay: creating the upload queue failed");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = 2;
  if (FAILED(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)))) {
    logf("overlay: creating the RTV heap failed");
    return false;
  }
  // RTV descriptors are read when OMSetRenderTargets is recorded, so the back-buffer slot can
  // be rewritten every frame while earlier frames are still in flight.
  canvasRtv_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
  backBufferRtv_ = canvasRtv_;
  backBufferRtv_.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = kSrvHeapSize;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_)))) {
    logf("overlay: creating the SRV heap failed");
    return false;
  }
  srvAlloc_.cpuStart = srvHeap_->GetCPUDescriptorHandleForHeapStart();
  srvAlloc_.gpuStart = srvHeap_->GetGPUDescriptorHandleForHeapStart();
  srvAlloc_.increment = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (UINT i = kSrvHeapSize - 1; i >= 1; --i) srvAlloc_.free.push_back(i);  // slot 0 is the canvas

  for (Frame& f : frames_) {
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) {
      logf("overlay: creating a command allocator failed");
      return false;
    }
  }
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commandList_))) ||
      FAILED(commandList_->Close())) {
    logf("overlay: creating the command list failed");
    return false;
  }
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    logf("overlay: creating the fence failed");
    return false;
  }
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fenceEvent_) {
    logf("overlay: CreateEventW failed (error {})", GetLastError());
    return false;
  }
  return true;
}

bool Renderer::createCompositeShaders() {
  compositeVs_ = compileShader("vsMain", "vs_5_0");
  compositePs_ = compileShader("psMain", "ps_5_0");
  if (!compositeVs_ || !compositePs_) return false;

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 2;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = 2;
  desc.pParameters = params;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
  ComPtr<ID3DBlob> blob, errors;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&compositeRootSignature_)))) {
    logf("overlay: creating the composite root signature failed: {}",
         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    return false;
  }
  return true;
}

bool Renderer::initImGuiBackend() {
  ImGui_ImplDX12_InitInfo info;
  info.Device = device_.Get();
  info.CommandQueue = uploadQueue_.Get();
  info.NumFramesInFlight = static_cast<int>(kFramesInFlight);
  info.RTVFormat = kCanvasFormat;
  info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  info.UserData = &srvAlloc_;
  info.SrvDescriptorHeap = srvHeap_.Get();
  info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    static_cast<SrvAllocator*>(i->UserData)->alloc(cpu, gpu);
  };
  info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE) { static_cast<SrvAllocator*>(i->UserData)->release(cpu); };
  if (!ImGui_ImplDX12_Init(&info)) {
    logf("overlay: ImGui_ImplDX12_Init failed");
    return false;
  }
  imguiBackend_ = true;
  // Init cannot fail; the root signature, shaders and PSO are otherwise created lazily in
  // the first NewFrame behind an assert. Create them now so a failure is reported here.
  if (!ImGui_ImplDX12_CreateDeviceObjects()) {
    logf("overlay: ImGui_ImplDX12_CreateDeviceObjects failed");
    return false;
  }
  return true;
}

bool Renderer::ensureCanvas(UINT width, UINT height) {
  if (canvas_ && canvasWidth_ == width && canvasHeight_ == height) return true;
  if (canvas_ && !waitIdle()) return false;  // earlier frames may still sample it
  canvas_.Reset();

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = kCanvasFormat;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = kCanvasFormat;
  const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                      IID_PPV_ARGS(&canvas_));
  if (FAILED(hr)) {
    logf("overlay: creating the {}x{} canvas failed (0x{:08X})", width, height, static_cast<unsigned>(hr));
    return false;
  }
  device_->CreateRenderTargetView(canvas_.Get(), nullptr, canvasRtv_);
  device_->CreateShaderResourceView(canvas_.Get(), nullptr, srvAlloc_.cpuStart);  // slot 0
  canvasWidth_ = width;
  canvasHeight_ = height;
  logf("overlay: canvas {}x{}", width, height);
  return true;
}

bool Renderer::ensureCompositePso(DXGI_FORMAT format) {
  if (compositePso_ && compositeFormat_ == format) return true;
  if (compositePso_ && !waitIdle()) return false;
  compositePso_.Reset();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = compositeRootSignature_.Get();
  pso.VS = {compositeVs_->GetBufferPointer(), compositeVs_->GetBufferSize()};
  pso.PS = {compositePs_->GetBufferPointer(), compositePs_->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_ONE;  // premultiplied
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
  blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  // Leave the game's alpha channel alone (it may be meaningful to a later compositor).
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                D3D12_COLOR_WRITE_ENABLE_BLUE;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                     D3D12_COMPARISON_FUNC_ALWAYS};
  pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = format;
  pso.SampleDesc.Count = 1;
  const HRESULT hr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&compositePso_));
  if (FAILED(hr)) {
    logf("overlay: creating the composite PSO for {} failed (0x{:08X})", formatName(format),
         static_cast<unsigned>(hr));
    return false;
  }
  compositeFormat_ = format;
  logf("overlay: composite PSO for {}", formatName(format));
  return true;
}

bool Renderer::waitForFence(UINT64 value) {
  if (fence_->GetCompletedValue() >= value) return true;
  if (SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_)) &&
      WaitForSingleObject(fenceEvent_, kGpuTimeoutMs) == WAIT_OBJECT_0)
    return true;
  logf("overlay: the GPU did not reach overlay fence value {} within {} ms", value, kGpuTimeoutMs);
  return false;
}

void Renderer::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  commandList_->ResourceBarrier(1, &b);
}

bool Renderer::render(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue, ImDrawData* drawData, bool hdr10) {
  if (!device_ || !drawData || drawData->TotalVtxCount == 0) return true;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (FAILED(swapChain->GetDesc1(&desc)) || desc.Width == 0 || desc.Height == 0) return true;

  Encoding encoding = Encoding::Sdr;
  switch (desc.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: encoding = Encoding::Sdr; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: encoding = hdr10 ? Encoding::Hdr10 : Encoding::Sdr; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: encoding = Encoding::ScRgb; break;
    default:
      if (unsupportedFormatLogged_ != desc.Format) {
        logf("overlay: back-buffer format {} is not supported; not drawing", static_cast<int>(desc.Format));
        unsupportedFormatLogged_ = desc.Format;
      }
      return true;
  }
  if (static_cast<int>(encoding) != loggedEncoding_) {
    static constexpr const char* kNames[] = {"SDR", "scRGB", "HDR10"};
    logf("overlay: compositing onto {} as {}", formatName(desc.Format), kNames[static_cast<UINT>(encoding)]);
    loggedEncoding_ = static_cast<int>(encoding);
  }

  // The back buffer first: if it cannot be drawn into, nothing is recorded, so the ImGui
  // backend's per-frame buffer ring (advanced by RenderDrawData) and ours stay in step.
  // Released on every return; never kept across frames (the game's ResizeBuffers fails while
  // anyone references a back buffer).
  struct BackBufferScope {
    Renderer& r;
    ~BackBufferScope() { r.releaseAfterFault(); }
  } backBufferScope{*this};
  if (FAILED(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer_)))) {
    backBuffer_ = nullptr;
    return true;
  }
  if (!(backBuffer_->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
    if (!notRenderTargetLogged_) logf("overlay: the back buffer is not a render target; not drawing");
    notRenderTargetLogged_ = true;
    return true;
  }
  if (!ensureCanvas(desc.Width, desc.Height) || !ensureCompositePso(desc.Format)) return false;

  Frame& frame = frames_[frameCount_ % kFramesInFlight];
  if (!waitForFence(frame.fenceValue)) return false;
  // Texture uploads (font atlas growth) run on uploadQueue_ while earlier frames may still
  // read the textures on the game's queue: let those finish.
  if (hasPendingTextureWork(drawData) && !waitIdle()) return false;
  if (FAILED(frame.allocator->Reset()) || FAILED(commandList_->Reset(frame.allocator.Get(), nullptr))) {
    logf("overlay: resetting the command list failed");
    return false;
  }

  // ImGui -> canvas.
  ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
  commandList_->SetDescriptorHeaps(1, heaps);
  transition(canvas_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  commandList_->ClearRenderTargetView(canvasRtv_, transparent, 0, nullptr);
  commandList_->OMSetRenderTargets(1, &canvasRtv_, FALSE, nullptr);
  ImGui_ImplDX12_RenderDrawData(drawData, commandList_.Get());
  transition(canvas_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // Canvas -> back buffer.
  D3D12_RENDER_TARGET_VIEW_DESC rtv{};
  rtv.Format = desc.Format;
  rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device_->CreateRenderTargetView(backBuffer_, &rtv, backBufferRtv_);
  transition(backBuffer_, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const UINT constants[2] = {static_cast<UINT>(encoding), std::bit_cast<UINT>(kPaperWhiteNits)};
  commandList_->SetGraphicsRootSignature(compositeRootSignature_.Get());
  commandList_->SetGraphicsRootDescriptorTable(0, srvAlloc_.gpuStart);
  commandList_->SetGraphicsRoot32BitConstants(1, 2, constants, 0);
  commandList_->SetPipelineState(compositePso_.Get());
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height),
                                0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
  commandList_->RSSetViewports(1, &viewport);
  commandList_->RSSetScissorRects(1, &scissor);
  commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commandList_->OMSetRenderTargets(1, &backBufferRtv_, FALSE, nullptr);
  commandList_->DrawInstanced(3, 1, 0, 0);
  transition(backBuffer_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

  const HRESULT hr = commandList_->Close();
  if (SUCCEEDED(hr)) {
    ID3D12CommandList* lists[] = {commandList_.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++lastSignaled_);
    frame.fenceValue = lastSignaled_;
    ++frameCount_;
  }
  if (FAILED(hr)) {
    logf("overlay: closing the command list failed (0x{:08X})", static_cast<unsigned>(hr));
    return false;
  }
  return true;
}

void Renderer::releaseAfterFault() {
  if (backBuffer_) backBuffer_->Release();
  backBuffer_ = nullptr;
}

}  // namespace ft::mod::overlay
