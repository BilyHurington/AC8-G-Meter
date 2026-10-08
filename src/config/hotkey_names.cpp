#include "config/hotkey_names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace ft {
namespace {

// Spelling must match ImGui::GetKeyName() (verified by the UI tests).
constexpr std::array<std::string_view, 82> kKeyNames{
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
    "LeftArrow", "RightArrow", "UpArrow", "DownArrow",
    "Space", "Tab", "Backspace", "Pause", "ScrollLock", "PrintScreen",
    "Keypad0", "Keypad1", "Keypad2", "Keypad3", "Keypad4",
    "Keypad5", "Keypad6", "Keypad7", "Keypad8", "Keypad9",
    "KeypadDecimal", "KeypadDivide", "KeypadMultiply", "KeypadSubtract", "KeypadAdd",
    "GraveAccent", "Minus", "Equal",
};

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// Canonical spelling for a case-insensitive key name, or empty if unknown.
std::string_view canonicalKey(std::string_view key) {
  for (std::string_view k : kKeyNames)
    if (iequals(k, key)) return k;
  return {};
}

}  // namespace

std::span<const std::string_view> knownHotkeyNames() { return kKeyNames; }

bool isKnownHotkeyName(std::string_view key) {
  return std::find(kKeyNames.begin(), kKeyNames.end(), key) != kKeyNames.end();
}

std::optional<Hotkey> parseHotkey(std::string_view text) {
  Hotkey hk;
  hk.ctrl = hk.shift = hk.alt = false;
  text = trim(text);
  while (!text.empty()) {
    const std::size_t plus = text.find('+');
    // A trailing "+" token would be the key itself; we do not support '+' as a key.
    const std::string_view token = trim(text.substr(0, plus));
    const bool last = plus == std::string_view::npos;
    if (last) {
      const std::string_view key = canonicalKey(token);
      if (key.empty()) return std::nullopt;
      hk.key = std::string(key);
      return hk;
    }
    if (iequals(token, "ctrl") || iequals(token, "control")) hk.ctrl = true;
    else if (iequals(token, "shift")) hk.shift = true;
    else if (iequals(token, "alt")) hk.alt = true;
    else return std::nullopt;
    text = text.substr(plus + 1);
  }
  return std::nullopt;
}

}  // namespace ft
