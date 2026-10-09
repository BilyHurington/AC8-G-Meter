#pragma once
// In-game overlay: draws Dear ImGui into the game's DX12 swap chain from a hook on
// IDXGISwapChain::Present / Present1 and forwards window input to ImGui.
//
// Threads:
//   - startOverlay() may be called from any thread except inside Present (installation runs
//     on a worker thread it starts);
//   - OverlayClient::onImGuiInit / onFrame run on the game's present (render) thread, with the
//     overlay's own ImGui context current;
//   - OverlayClient::wantsInput runs on the game window's thread (WndProc) and the present thread.
// The overlay owns the ImGui context and the Win32 + DX12 backends; the client only issues
// ImGui calls (ft::TelemetryUi).

namespace ft::mod {

class OverlayClient {
 public:
  virtual ~OverlayClient() = default;
  // Once, after the ImGui context and backends exist, before the first frame: fonts, style.
  virtual void onImGuiInit() = 0;
  // Every presented frame, between ImGui::NewFrame() and ImGui::Render(). Size in pixels of
  // the swap chain's back buffer; `now` in seconds of a monotonic clock.
  virtual void onFrame(float width, float height, double now) = 0;
  // True while the overlay needs mouse / keyboard (menus, edit mode): input is then swallowed
  // instead of reaching the game. Must be cheap and thread-safe (TelemetryUi::wantsInput()).
  virtual bool wantsInput() const = 0;
};

// Installs the hooks. `client` must stay valid for the rest of the process (the mod DLL is
// pinned; hooks are never removed). Returns false if installation could not start; the
// outcome of the asynchronous installation is logged.
bool startOverlay(OverlayClient& client);

// Stops drawing (e.g. on an unrecoverable fault reported elsewhere). The hooks stay in place
// and pass through to the game. Once it returns, the client is no longer called (it waits up
// to 1 s for a frame or input message that is using the client; safe to call from the client).
void disableOverlay();

// True once the Present hooks are in place (installation finished successfully).
bool overlayInstalled();

}  // namespace ft::mod
