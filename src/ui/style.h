#pragma once
// Optional ImGui theme for the telemetry menus (dark, HUD-green accents). Hosts call it once
// after creating the context; the overlay text itself does not depend on the style.
struct ImGuiStyle;

namespace ft {

void applyTelemetryStyle(ImGuiStyle& style);

}  // namespace ft
