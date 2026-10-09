#pragma once
// Game window input: a window-procedure subclass that feeds ImGui and hides input from the
// game while the client wants it.
//
// While the client does not want input, ImGui gets keyboard and focus messages only (enough
// for its hotkeys); the mouse position is polled by the present thread instead, so ImGui's
// Win32 backend never takes or releases the game's mouse capture. While the client wants
// input, every message goes to ImGui and mouse / key-down input is hidden from the game.
// Key-ups and mouse button-ups always reach the game (no stuck keys); gamepads (XInput, HID
// raw input) are never affected.
#include <windows.h>

namespace ft::mod::overlay {

// Subclasses `hwnd` once (call from any thread of this process). Messages reach ImGui only
// while shared().inputWindow == hwnd. Never un-subclassed: another hook may have chained on
// top of ours.
bool subclassWindow(HWND hwnd);

// Present thread, every frame: if someone restored the window procedure we replaced (taking
// ours out of the chain), subclass again. Logs once per window what it finds.
void checkSubclass(HWND hwnd);

// Back-buffer pixels per client-area pixel (ImGui works in back-buffer pixels).
void setMouseScale(float x, float y);

// True (once) if the window thread dropped a key-up, button-up or focus loss because ImGui
// was busy: the present thread then clears ImGui's key and mouse state (no stuck keys).
bool takeDroppedRelease();

}  // namespace ft::mod::overlay
