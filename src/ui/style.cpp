#include "ui/style.h"

#include <imgui.h>

namespace ft {

void applyTelemetryStyle(ImGuiStyle& style) {
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.PopupRounding = 4.0f;
  style.WindowPadding = ImVec2(12.0f, 10.0f);
  style.ItemSpacing = ImVec2(8.0f, 6.0f);
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

  // Neutral dark panels with the HUD green (#93F6A8) as the accent colour.
  const ImVec4 accent(0.576f, 0.965f, 0.659f, 1.0f);
  auto withAlpha = [](ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); };
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.07f, 0.94f);
  c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.09f, 0.08f, 0.97f);
  c[ImGuiCol_Border] = ImVec4(0.30f, 0.45f, 0.35f, 0.45f);
  c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.12f, 0.10f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.20f, 0.15f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.18f, 0.15f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.26f, 0.21f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.32f, 0.26f, 1.0f);
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_SliderGrab] = withAlpha(accent, 0.75f);
  c[ImGuiCol_SliderGrabActive] = accent;
  c[ImGuiCol_Button] = ImVec4(0.16f, 0.27f, 0.20f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.38f, 0.28f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.48f, 0.35f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.16f, 0.27f, 0.20f, 1.0f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.38f, 0.28f, 1.0f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.48f, 0.35f, 1.0f);
  c[ImGuiCol_Separator] = ImVec4(0.30f, 0.45f, 0.35f, 0.50f);
  c[ImGuiCol_PlotLines] = accent;
  c[ImGuiCol_PlotHistogram] = withAlpha(accent, 0.85f);
  c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.30f);
  c[ImGuiCol_NavCursor] = accent;
  c[ImGuiCol_Tab] = ImVec4(0.12f, 0.20f, 0.15f, 1.0f);
  c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.38f, 0.28f, 1.0f);
  c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.32f, 0.24f, 1.0f);
}

}  // namespace ft
