#include "ui/hotkey_input.h"

#include <string>

#include "config/hotkey_names.h"

namespace ft {
namespace {

bool ctrlHeld(const ImGuiIO& io) { return io.KeyCtrl || (io.ConfigMacOSXBehaviors && io.KeySuper); }

}  // namespace

ImGuiKey imguiKeyFromName(std::string_view name) {
  if (name.empty()) return ImGuiKey_None;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    const auto key = static_cast<ImGuiKey>(k);
    if (name == ImGui::GetKeyName(key)) return key;
  }
  return ImGuiKey_None;
}

bool isHotkeyPressed(const Hotkey& hotkey) {
  const ImGuiKey key = imguiKeyFromName(hotkey.key);
  if (key == ImGuiKey_None || !ImGui::IsKeyPressed(key, /*repeat=*/false)) return false;
  const ImGuiIO& io = ImGui::GetIO();
  return ctrlHeld(io) == hotkey.ctrl && io.KeyShift == hotkey.shift && io.KeyAlt == hotkey.alt;
}

std::optional<Hotkey> pollHotkeyCapture() {
  const ImGuiIO& io = ImGui::GetIO();
  for (std::string_view name : knownHotkeyNames()) {
    const ImGuiKey key = imguiKeyFromName(name);
    if (key != ImGuiKey_None && ImGui::IsKeyPressed(key, false))
      return Hotkey{std::string(name), ctrlHeld(io), io.KeyShift, io.KeyAlt};
  }
  return std::nullopt;
}

}  // namespace ft
