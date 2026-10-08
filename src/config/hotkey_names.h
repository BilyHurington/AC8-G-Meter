#pragma once
// Hotkey text representation, shared by the JSON settings and the UI.
// Key names match ImGui's ImGui::GetKeyName() spelling ("F8", "A", "Insert", "Keypad5"),
// but this header has no ImGui dependency so it stays usable from ft_config and tests.
#include <optional>
#include <span>
#include <string_view>

#include "config/settings.h"

namespace ft {

// Keys a hotkey may be bound to. Modifier keys and mouse buttons are deliberately excluded.
std::span<const std::string_view> knownHotkeyNames();
bool isKnownHotkeyName(std::string_view key);

// Parses "Ctrl+Shift+F8" (case-insensitive modifiers, '+' separated, key last).
// Returns nullopt for an empty string or an unknown key name.
std::optional<Hotkey> parseHotkey(std::string_view text);

}  // namespace ft
