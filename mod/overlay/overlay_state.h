#pragma once
// State shared by the present hook (overlay.cpp) and the window procedure (input.cpp).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace ft::mod {
class OverlayClient;
}

namespace ft::mod::overlay {

struct SharedState {
  // Serialises everything that touches the ImGui context or calls the client: the present
  // thread (NewFrame .. Render) and the window thread (ImGui_ImplWin32_WndProcHandler,
  // wantsInput). Recursive because SetCapture/ReleaseCapture re-enter the window procedure.
  // Timed so that neither thread can ever block the game for long, even if a structured
  // exception left it locked (/EHsc does not unwind on SEH).
  std::recursive_timed_mutex imguiMutex;
  std::atomic<bool> disabled{false};
  OverlayClient* client = nullptr;  // set once by startOverlay() before any hook can run
  // Window whose messages feed ImGui (the Win32 backend's window); null while none.
  std::atomic<HWND> inputWindow{nullptr};
};

SharedState& shared();

// Longest a window message or a frame waits for the other thread to leave ImGui.
inline constexpr std::chrono::milliseconds kImGuiLockTimeout{50};

}  // namespace ft::mod::overlay
