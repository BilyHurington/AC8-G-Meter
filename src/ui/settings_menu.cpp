#include "ui/settings_menu.h"

#include <cfloat>

#include <imgui.h>

#include "ui/hotkey_input.h"

namespace ft {
namespace {

constexpr ImVec4 kAccent{0.576f, 0.965f, 0.659f, 1.0f};  // #93F6A8, same as the HUD text
constexpr ImVec4 kWarn{1.0f, 0.62f, 0.35f, 1.0f};
constexpr ImVec4 kError{1.0f, 0.40f, 0.40f, 1.0f};

void sectionHeader(const char* title) {
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::SeparatorText(title);
  ImGui::PopStyleColor();
}

void helpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

}  // namespace

SettingsMenuResult SettingsMenu::draw(SettingsStore& store, bool& open, bool& editMode,
                                      bool hasLastReport, double now) {
  SettingsMenuResult r;
  if (!open) {
    capture_ = Capture::None;
    return r;
  }

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 24.0f, 0.0f),
                                      ImVec2(FLT_MAX, vp->WorkSize.y * 0.95f));
  if (!ImGui::Begin("Flight Telemetry Settings###ft_settings", &open,
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return r;
  }

  Settings& s = store.settings();
  ImGui::PushItemWidth(ImGui::GetFontSize() * 13.0f);
  overlaySection(s, editMode, r);
  metricsSection(s, r);
  hotkeySection(s, r);

  sectionHeader("Mission summary");
  r.changed |= ImGui::Checkbox("Show summary when a mission ends", &s.summary.autoShow);
  ImGui::BeginDisabled(!hasLastReport);
  if (ImGui::Button("Show last mission")) r.showLastSummary = true;
  ImGui::EndDisabled();
  if (!hasLastReport) {
    ImGui::SameLine();
    ImGui::TextDisabled("no mission finished yet");
  }
  ImGui::PopItemWidth();

  ImGui::Spacing();
  ImGui::Separator();
  if (ImGui::Button("Restore defaults...")) ImGui::OpenPopup("Restore defaults?");
  ImGui::SameLine();
  if (ImGui::Button("Close")) open = false;

  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Reset every overlay, metric, hotkey and summary setting\nto its default value?");
    ImGui::Spacing();
    if (ImGui::Button("Restore defaults")) {
      store.restoreDefaults(now);
      r.changed = r.pipelineChanged = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // Status line: where settings live and whether the last change reached the disk.
  ImGui::Spacing();
  if (!store.lastError().empty()) ImGui::TextColored(kError, "Save failed: %s", store.lastError().c_str());
  else if (store.dirty() || r.changed) ImGui::TextColored(kWarn, "Saving...");
  else ImGui::TextColored(kAccent, "All changes saved");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", store.path().string().c_str());
  ImGui::PopTextWrapPos();

  ImGui::End();
  if (!open) capture_ = Capture::None;
  return r;
}

void SettingsMenu::overlaySection(Settings& s, bool& editMode, SettingsMenuResult& r) {
  OverlaySettings& o = s.overlay;
  sectionHeader("Overlay");
  ImGui::Checkbox("Edit layout (drag the overlay)", &editMode);
  helpMarker("While editing, drag the telemetry block with the mouse. "
             "The position is stored relative to the screen size.");
  r.changed |= ImGui::Checkbox("Lock position", &o.locked);

  r.changed |= ImGui::SliderFloat("Font size", &o.fontSizeAt1080p, 8.0f, 64.0f, "%.0f px @1080p",
                                  ImGuiSliderFlags_AlwaysClamp);
  helpMarker("Size at 1920x1080. Other resolutions scale proportionally to screen height.");
  r.changed |= ImGui::SliderFloat("Line spacing", &o.lineSpacing, 1.0f, 2.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
  r.changed |= ImGui::SliderFloat("Text opacity", &o.opacity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
  r.changed |= ImGui::ColorEdit3("Text color", &o.textColor.r, ImGuiColorEditFlags_DisplayHex);

  r.changed |= ImGui::Checkbox("Drop shadow", &o.shadow);
  ImGui::BeginDisabled(!o.shadow);
  r.changed |= ImGui::SliderFloat("Shadow strength", &o.shadowAlpha, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
  ImGui::EndDisabled();

  r.changed |= ImGui::Checkbox("Background panel", &o.background);
  ImGui::BeginDisabled(!o.background);
  r.changed |= ImGui::SliderFloat("Background opacity", &o.backgroundAlpha, 0.0f, 1.0f, "%.2f",
                                  ImGuiSliderFlags_AlwaysClamp);
  ImGui::EndDisabled();
}

void SettingsMenu::metricsSection(Settings& s, SettingsMenuResult& r) {
  sectionHeader("Metrics");
  ImGui::TextDisabled("Shown on the overlay:");
  if (ImGui::BeginTable("##ft_metric_rows", 2)) {
    for (int i = 0; i < kMetricCount; ++i) {
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      r.changed |= ImGui::Checkbox(metricLabel(static_cast<MetricId>(i)),
                                   &s.overlay.visible[static_cast<std::size_t>(i)]);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  MetricsSettings& m = s.metrics;
  bool pipe = false;
  const char* modes[] = {"Nz (head-to-seat, signed)", "Total |n| (magnitude)"};
  int mode = m.gMode == GMode::Total ? 1 : 0;
  if (ImGui::Combo("G mode", &mode, modes, IM_ARRAYSIZE(modes))) {
    m.gMode = mode == 1 ? GMode::Total : GMode::Nz;
    pipe = true;
  }
  helpMarker("Nz is the load the pilot feels along the spine (negative when pushing). "
             "Total is the magnitude of the whole load-factor vector.");
  pipe |= ImGui::SliderFloat("Smoothing", &m.smoothingTauS, 0.0f, 1.0f, "tau %.2f s", ImGuiSliderFlags_AlwaysClamp);
  helpMarker("Time constant of the G display filter. 0 = raw (after spike rejection).");
  pipe |= ImGui::SliderFloat("High-G enter", &m.highGEnter, 2.0f, 12.0f, "%.1f G", ImGuiSliderFlags_AlwaysClamp);
  pipe |= ImGui::SliderFloat("High-G exit", &m.highGExit, 1.0f, m.highGEnter - 0.1f, "%.1f G",
                             ImGuiSliderFlags_AlwaysClamp);
  helpMarker("A sustained high-G episode starts at the enter threshold and ends below the exit "
             "threshold (hysteresis avoids flicker around a single value).");
  if (pipe) {
    s.clamp();  // keeps exit < enter when enter is lowered
    r.changed = r.pipelineChanged = true;
  }
}

void SettingsMenu::hotkeyRow(const char* label, Hotkey& hotkey, Capture which, SettingsMenuResult& r) {
  ImGui::PushID(label);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
  if (capture_ == which) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::Button("Press a key...  (Esc cancels)");
    ImGui::PopStyleColor();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      capture_ = Capture::None;
    } else if (auto captured = pollHotkeyCapture()) {
      hotkey = *captured;
      capture_ = Capture::None;
      r.changed = true;
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::Text("%-14s", hotkey.toString().c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Change")) capture_ = which;
  }
  ImGui::PopID();
}

void SettingsMenu::hotkeySection(Settings& s, SettingsMenuResult& r) {
  sectionHeader("Hotkeys");
  hotkeyRow("Settings menu", s.hotkeys.toggleMenu, Capture::Menu, r);
  hotkeyRow("Edit layout", s.hotkeys.toggleEditMode, Capture::EditMode, r);
  if (s.hotkeys.toggleMenu == s.hotkeys.toggleEditMode)
    ImGui::TextColored(kWarn, "Both actions use the same key: only the menu will toggle.");
}

}  // namespace ft
