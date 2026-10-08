#pragma once
// Optional ImGui theme for the telemetry menus (dark, HUD-green accents). Hosts call it once
// after creating the context; the overlay text itself does not depend on the style.
#include <imgui.h>

namespace ft {

// Palette shared by the menus, the mission summary and the G chart.
inline constexpr ImVec4 kHudAccent{0.576f, 0.965f, 0.659f, 1.0f};  // #93F6A8, same as the HUD text
inline constexpr ImVec4 kHudWarn{1.0f, 0.62f, 0.35f, 1.0f};        // high G, warnings
inline constexpr ImVec4 kHudError{1.0f, 0.40f, 0.40f, 1.0f};
inline constexpr ImVec4 kHudSecondary{0.45f, 0.78f, 1.0f, 1.0f};   // secondary chart series

inline ImVec4 withAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

void applyTelemetryStyle(ImGuiStyle& style);

}  // namespace ft
