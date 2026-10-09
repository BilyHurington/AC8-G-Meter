#include "input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <utility>

#include <imgui.h>

#include "overlay.h"
#include "overlay_state.h"
#include "support/log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ft::mod::overlay {

namespace {

enum class Route {
  Game,         // the game's window procedure
  Swallow,      // nobody else
  DefaultProc,  // DefWindowProcW only (WM_INPUT must still be cleaned up)
};

// A window we subclassed and its previous procedure. `original` is stored before `hwnd`, so a
// reader that finds the window also finds its procedure. Slots are never reused.
struct Subclass {
  std::atomic<HWND> hwnd{nullptr};
  std::atomic<WNDPROC> original{nullptr};
  bool restoredLogged = false;  // under g_subclassMutex
  bool foreignLogged = false;
};

constexpr std::size_t kMaxWindows = 8;
std::array<Subclass, kMaxWindows> g_windows;
std::atomic<std::size_t> g_windowCount{0};
std::mutex g_subclassMutex;  // serialises subclassing
bool g_tooManyLogged = false;
HWND g_failedLogged = nullptr;

std::atomic<float> g_mouseScaleX{1.0f}, g_mouseScaleY{1.0f};
std::atomic<bool> g_droppedRelease{false};

// Window-thread state: mouse buttons whose down message reached ImGui (so their up must too,
// whatever the capture state), and who held the mouse capture when the first one went down.
unsigned g_buttonsForwarded = 0;
HWND g_captureBeforeButtons = nullptr;

// Whether an exception came out of the game's window procedure (possibly re-entered from inside
// our code, e.g. by ImGui's SetCapture/ReleaseCapture): our guards must let it through.
thread_local bool t_gameException = false;

Subclass* findSubclass(HWND hwnd) {
  const std::size_t n = std::min(g_windowCount.load(std::memory_order_acquire), kMaxWindows);
  for (std::size_t i = 0; i < n; ++i)
    if (g_windows[i].hwnd.load(std::memory_order_acquire) == hwnd) return &g_windows[i];
  return nullptr;
}

bool isMouseMessage(UINT msg) {
  return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE ||
         msg == WM_NCMOUSELEAVE || msg == WM_SETCURSOR;
}

// ImGui's button index (as imgui_impl_win32 numbers them), or -1; `down` for down/double-click.
int mouseButton(UINT msg, WPARAM wParam, bool* down) {
  *down = true;
  switch (msg) {
    case WM_LBUTTONUP: *down = false; [[fallthrough]];
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONUP: *down = false; [[fallthrough]];
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONUP: *down = false; [[fallthrough]];
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONUP: *down = false; [[fallthrough]];
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK: return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
    default: return -1;
  }
}

bool isRelease(UINT msg) {
  bool down = false;
  return msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_KILLFOCUS ||
         (mouseButton(msg, 0, &down) >= 0 && !down);
}

// Client-area mouse coordinates scaled to back-buffer pixels.
LPARAM scaleMousePosition(UINT msg, LPARAM lParam) {
  bool down = false;
  if (msg != WM_MOUSEMOVE && mouseButton(msg, 0, &down) < 0) return lParam;  // wheel etc.: screen coordinates
  const float sx = g_mouseScaleX.load(std::memory_order_relaxed);
  const float sy = g_mouseScaleY.load(std::memory_order_relaxed);
  if (sx == 1.0f && sy == 1.0f) return lParam;
  const auto x = static_cast<short>(std::lround(static_cast<short>(LOWORD(lParam)) * sx));
  const auto y = static_cast<short>(std::lround(static_cast<short>(HIWORD(lParam)) * sy));
  return MAKELPARAM(x, y);
}

// Raw mouse input, and raw key presses, belong to the overlay while it captures input; raw
// key releases (no stuck keys) and other HID devices (gamepads) stay with the game.
bool rawInputIsCaptured(LPARAM lParam) {
  const auto handle = reinterpret_cast<HRAWINPUT>(lParam);
  RAWINPUTHEADER header{};
  UINT size = sizeof header;
  if (GetRawInputData(handle, RID_HEADER, &header, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    return false;
  if (header.dwType == RIM_TYPEMOUSE) return true;
  if (header.dwType != RIM_TYPEKEYBOARD) return false;
  RAWINPUT input{};
  size = sizeof input;
  if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    return false;
  return (input.data.keyboard.Flags & RI_KEY_BREAK) == 0;
}

// Where a message goes while the client wants input.
Route capturedRoute(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_CHAR:
      return Route::Swallow;
    case WM_SYSKEYDOWN:
      return wParam == VK_F4 ? Route::Game : Route::Swallow;  // Alt+F4 still closes the game
    case WM_INPUT:
      return rawInputIsCaptured(lParam) ? Route::DefaultProc : Route::Game;
    default:  // button-ups, key-ups, focus, sizing, ...
      return Route::Game;
  }
}

// Passes one message to ImGui (if it should see it) and decides where it goes next.
// Called with imguiMutex held. Sets *restoreCapture if ImGui released a capture the game held.
void feedImGui(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool captured, bool* restoreCapture) {
  bool down = false;
  const int button = mouseButton(msg, wParam, &down);
  if (button >= 0) {
    const unsigned bit = 1u << button;
    if (down) {
      if (!captured) return;
      if (g_buttonsForwarded == 0) g_captureBeforeButtons = GetCapture();
      g_buttonsForwarded |= bit;
    } else {
      if (!(g_buttonsForwarded & bit)) return;  // ImGui never saw it go down
      g_buttonsForwarded &= ~bit;
    }
  } else if (isMouseMessage(msg) && !captured) {
    return;  // the position is polled by the present thread
  }
  ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, scaleMousePosition(msg, lParam));
  // Returned normally: a game exception raised in a nested call (SetCapture -> game WndProc)
  // was handled in between, so a later fault of ours is ours again.
  t_gameException = false;
  // imgui_impl_win32 releases the capture when its last button goes up, even if the game held
  // it before (it only takes it when nobody does): give it back.
  if (button >= 0 && !down && g_buttonsForwarded == 0 && g_captureBeforeButtons == hwnd && GetCapture() != hwnd)
    *restoreCapture = true;
}

Route routeMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  SharedState& s = shared();
  const auto active = [&] {
    return !s.disabled.load(std::memory_order_acquire) && s.inputWindow.load(std::memory_order_acquire) == hwnd;
  };
  if (!active()) return Route::Game;
  std::unique_lock lock(s.imguiMutex, std::defer_lock);
  if (!lock.try_lock_for(kImGuiLockTimeout)) {
    if (isRelease(msg)) g_droppedRelease.store(true, std::memory_order_release);
    return Route::Game;
  }
  if (!active()) return Route::Game;
  const bool captured = s.client->wantsInput();
  bool restoreCapture = false;
  feedImGui(hwnd, msg, wParam, lParam, captured, &restoreCapture);
  lock.unlock();
  if (restoreCapture) SetCapture(hwnd);
  return captured ? capturedRoute(msg, wParam, lParam) : Route::Game;
}

void disableAfterFault(const char* what) noexcept {
  shared().disabled.store(true, std::memory_order_release);
  try {
    logf("overlay: {} in the window procedure; overlay disabled", what);
  } catch (...) {
  }
}

// An exception escaping a window procedure ends the game: catch ours. One that came out of
// the game's own procedure (re-entered from ImGui's SetCapture/ReleaseCapture) is rethrown.
// Not noexcept: a structured exception must be able to pass through to guardedRoute.
Route routeMessageNoThrow(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  try {
    return routeMessage(hwnd, msg, wParam, lParam);
  } catch (const std::exception& e) {
    if (t_gameException) throw;
    disableAfterFault(e.what());
  } catch (...) {
    if (t_gameException) throw;
    disableAfterFault("unknown C++ exception");
  }
  return Route::Game;
}

int routeFilter() { return t_gameException ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER; }

// Structured exceptions too, but only those raised by our code. No C++ objects here: __try
// forbids unwinding.
bool guardedRoute(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, Route* route) {
  __try {
    *route = routeMessageNoThrow(hwnd, msg, wParam, lParam);
    return true;
  } __except (routeFilter()) {
    return false;
  }
}

int markGameException() {
  t_gameException = true;
  return EXCEPTION_CONTINUE_SEARCH;
}

// Calls the game's procedure; an exception from it is only marked, never handled.
LRESULT callGame(WNDPROC original, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  t_gameException = false;
  __try {
    return original ? CallWindowProcW(original, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
  } __except (markGameException()) {
    return 0;  // not reached: the filter never handles
  }
}

void forgetWindow(HWND hwnd) {
  if (Subclass* entry = findSubclass(hwnd)) entry->hwnd.store(nullptr, std::memory_order_release);
  HWND expected = hwnd;
  shared().inputWindow.compare_exchange_strong(expected, nullptr);
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  Route route = Route::Game;
  t_gameException = false;
  if (!guardedRoute(hwnd, msg, wParam, lParam, &route)) {
    disableAfterFault("structured exception");
    route = Route::Game;
  }
  switch (route) {
    case Route::Swallow:
      return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK ? TRUE : 0;  // "processed"
    case Route::DefaultProc:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    case Route::Game:
      break;
  }
  const Subclass* entry = findSubclass(hwnd);
  const LRESULT result =
      callGame(entry ? entry->original.load(std::memory_order_acquire) : nullptr, hwnd, msg, wParam, lParam);
  if (msg == WM_NCDESTROY) forgetWindow(hwnd);  // the handle value may be reused
  return result;
}

}  // namespace

bool subclassWindow(HWND hwnd) {
  std::lock_guard lock(g_subclassMutex);
  const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
  if (findSubclass(hwnd) || current == &subclassProc) return true;
  const std::size_t n = g_windowCount.load(std::memory_order_relaxed);
  if (n >= kMaxWindows) {
    if (!g_tooManyLogged) logf("overlay: too many game windows; not subclassing {}", static_cast<const void*>(hwnd));
    g_tooManyLogged = true;
    return false;
  }
  Subclass& entry = g_windows[n];
  entry.original.store(current, std::memory_order_release);
  entry.hwnd.store(hwnd, std::memory_order_release);
  g_windowCount.store(n + 1, std::memory_order_release);
  SetLastError(0);
  const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclassProc));
  if (previous == 0 && GetLastError() != 0) {
    if (g_failedLogged != hwnd)
      logf("overlay: subclassing window {} failed (error {})", static_cast<const void*>(hwnd), GetLastError());
    g_failedLogged = hwnd;
    entry.hwnd.store(nullptr, std::memory_order_release);
    return false;
  }
  // Someone may have subclassed the window between the read and the write.
  entry.original.store(reinterpret_cast<WNDPROC>(previous), std::memory_order_release);
  logf("overlay: subclassed window {}", static_cast<const void*>(hwnd));
  return true;
}

void checkSubclass(HWND hwnd) {
  const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
  if (current == &subclassProc) return;
  std::lock_guard lock(g_subclassMutex);
  Subclass* entry = findSubclass(hwnd);
  if (!entry) return;
  if (current == entry->original.load(std::memory_order_acquire)) {
    // Someone restored the procedure we replaced: we are out of the chain.
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclassProc));
    if (!entry->restoredLogged)
      logf("overlay: window {} lost our subclass (procedure restored); subclassed again",
           static_cast<const void*>(hwnd));
    entry->restoredLogged = true;
  } else if (!entry->foreignLogged) {
    // Normally another hook chained on top of ours; nothing to do, but worth knowing.
    logf("overlay: window {} procedure is now {} (assumed to chain to ours)", static_cast<const void*>(hwnd),
         reinterpret_cast<const void*>(current));
    entry->foreignLogged = true;
  }
}

void setMouseScale(float x, float y) {
  g_mouseScaleX.store(x, std::memory_order_relaxed);
  g_mouseScaleY.store(y, std::memory_order_relaxed);
}

bool takeDroppedRelease() { return g_droppedRelease.exchange(false, std::memory_order_acq_rel); }

}  // namespace ft::mod::overlay
