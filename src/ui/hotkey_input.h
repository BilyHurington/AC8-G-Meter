#pragma once
// Hotkey <-> ImGui key mapping (thin ImGui wrappers around config/hotkey_names.h).
#include <optional>
#include <string_view>

#include <imgui.h>

#include "config/settings.h"

namespace ft {

// Maps an ImGui key name ("F8", "A", "Insert") to the ImGuiKey; ImGuiKey_None if unknown.
ImGuiKey imguiKeyFromName(std::string_view name);

// True on the frame the hotkey's key goes down with exactly its modifiers held.
// "Ctrl" also accepts Cmd on macOS (ImGui swaps Ctrl/Super when ConfigMacOSXBehaviors is set,
// and physical Ctrl+F-keys are often taken by the OS there).
bool isHotkeyPressed(const Hotkey& hotkey);

// "Press a key" capture: returns the bindable key pressed this frame together with the held
// modifiers, or nullopt if none was pressed.
std::optional<Hotkey> pollHotkeyCapture();

}  // namespace ft
