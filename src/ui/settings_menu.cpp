#include "ui/settings_menu.h"

#include <cfloat>

#include <imgui.h>

#include "ui/hotkey_input.h"
#include "ui/style.h"

namespace ft {
namespace {

constexpr ImVec4 kAccent = kHudAccent;
constexpr ImVec4 kWarn = kHudWarn;
constexpr ImVec4 kError = kHudError;

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

int missionsDeletedByNewLimit(int currentLimit, int newLimit, int savedMissions) {
  if (newLimit <= 0 || savedMissions <= newLimit) return 0;  // unlimited, or all fit
  // Missions the current limit already lets go are not news (a lower or equal current limit).
  const int alreadyDeleted = currentLimit > 0 && currentLimit < savedMissions ? savedMissions - currentLimit : 0;
  const int deleted = savedMissions - newLimit;
  return deleted > alreadyDeleted ? deleted : 0;
}

SettingsMenuResult SettingsMenu::draw(SettingsStore& store, bool& open, bool& editMode, bool hasLastReport,
                                      const MissionArchiveInfo& archive, const FilterState& filter, double now) {
  SettingsMenuResult r;
  if (!open) {
    capture_ = Capture::None;
    pendingKeep_ = -1;  // closing the menu cancels an unconfirmed "Keep missions" change
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
  metricsSection(s, filter, r);
  hotkeySection(s, r);

  missionSection(s, hasLastReport, archive, r);
  ImGui::PopItemWidth();

  ImGui::Spacing();
  ImGui::Separator();
  if (ImGui::Button("Restore defaults...")) ImGui::OpenPopup("Restore defaults?");
  ImGui::SameLine();
  if (ImGui::Button("Close")) open = false;

  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Reset every overlay, metric, hotkey, summary and history\nsetting to its default value?"
                           "\nSaved missions are kept.");
    const int defaultKeep = Settings{}.history.maxSavedMissions;
    const int deletes = archive.available && !archive.counting
                            ? missionsDeletedByNewLimit(s.history.maxSavedMissions, defaultKeep, archive.savedMissions)
                            : 0;
    if (deletes > 0)
      ImGui::TextColored(kWarn, "Except: \"Keep missions\" goes back to %d, which deletes\n%d older mission%s on the next save.",
                         defaultKeep, deletes, deletes == 1 ? "" : "s");
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
  ImGui::TextDisabled("%s", pathToUtf8(store.path()).c_str());
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

void SettingsMenu::metricsSection(Settings& s, const FilterState& filter, SettingsMenuResult& r) {
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
  pipe |= ImGui::Checkbox("Adaptive filtering", &m.adaptiveFiltering);
  helpMarker("Auto window from data rate & noise: sizes the G filter from the measured data "
             "rate and velocity noise. Clean or high-rate data gets a short window and much less "
             "display delay; noisy data gets the fixed filter, never more. Data without velocity "
             "always uses the fixed filter. Turn off to set the smoothing by hand.");
  pipe |= ImGui::SliderFloat("Smoothing", &m.smoothingTauS, 0.0f, 1.0f, "tau %.2f s", ImGuiSliderFlags_AlwaysClamp);
  helpMarker("Time constant of the G display filter. 0 = raw (after spike rejection). With "
             "adaptive filtering on, this is the upper limit: clean data gets less.");
  if (m.adaptiveFiltering) {
    switch (filter.input) {
      case FilterState::Input::None:
        ImGui::TextDisabled("Now: waiting for data");
        break;
      case FilterState::Input::PositionOnly:
        ImGui::TextDisabled("Now: position-only input: fixed windows");
        break;
      case FilterState::Input::Velocity:
        if (filter.adaptive) {
          ImGui::TextDisabled("Now: window %.0f ms, tau %.0f ms", filter.accelWindowS * 1000.0,
                              filter.smoothingTauS * 1000.0);
          ImGui::TextDisabled("     data %.0f Hz, noise %.3f m/s", filter.sampleRateHz, filter.velocityNoiseMS);
        } else {
          ImGui::TextDisabled("Now: measuring noise... (fixed windows meanwhile)");
        }
        break;
    }
  }
  pipe |= ImGui::SliderFloat("High-G enter", &m.highGEnter, 2.0f, 12.0f, "%.1f G", ImGuiSliderFlags_AlwaysClamp);
  pipe |= ImGui::SliderFloat("High-G exit", &m.highGExit, 1.0f, m.highGEnter - 0.1f, "%.1f G",
                             ImGuiSliderFlags_AlwaysClamp);
  helpMarker("A sustained high-G episode starts at the enter threshold and ends below the exit "
             "threshold (hysteresis avoids flicker around a single value).");
  pipe |= ImGui::SliderFloat("Sustained G: ignore bumps shorter than", &m.minExcursionS, 0.0f, 0.5f,
                             m.minExcursionS <= 0.0f ? "off" : "%.2f s", ImGuiSliderFlags_AlwaysClamp);
  helpMarker("The mission keeps two kinds of G values. Peak G (MAX / MIN G on the overlay) is "
             "every reading as the live G showed it, real high-G spikes included. Sustained G is "
             "the highest / lowest level held for at least about this long: bumps and dips shorter "
             "than it (collisions, physics jolts) are left out, so it can be lower than the peak "
             "even in a real manoeuvre (a pull that only touches 9 G reads less). High-G episodes "
             "use the sustained G. It is ready up to about twice this time after the live G. "
             "On noisy data it smooths more (adapting to the measured noise): then bumps shorter "
             "than about half this time are still removed, longer ones partly kept, and the "
             "sustained extremes can read about 0.1 G (up to ~0.35 G with very noisy data at "
             "60 fps) beyond the level really held. 0 = sustained equals peak.");
  if (pipe) {
    s.clamp();  // keeps exit < enter when enter is lowered
    r.changed = r.pipelineChanged = true;
  }
}

void SettingsMenu::missionSection(Settings& s, bool hasLastReport, const MissionArchiveInfo& archive,
                                  SettingsMenuResult& r) {
  sectionHeader("Mission summary & history");
  r.changed |= ImGui::Checkbox("Show summary when a mission ends", &s.summary.autoShow);
  ImGui::BeginDisabled(!hasLastReport);
  if (ImGui::Button("Show last mission")) r.showLastSummary = true;
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!archive.available);
  if (ImGui::Button("Mission history...")) r.showHistory = true;
  ImGui::EndDisabled();
  if (!hasLastReport) ImGui::TextDisabled("No mission finished yet.");
  if (!archive.available) {
    ImGui::TextDisabled("Mission history is not available in this host.");
    pendingKeep_ = -1;
    return;
  }
  r.changed |= ImGui::Checkbox("Save finished missions", &s.history.saveMissions);
  helpMarker("Every mission with G data is saved to the mission history folder (a small JSON "
             "summary plus a CSV of the G curve), so its summary and chart can be reopened later.");
  ImGui::BeginDisabled(!s.history.saveMissions);
  keepMissionsInput(s, archive, r);
  ImGui::EndDisabled();
}

void SettingsMenu::keepMissionsInput(Settings& s, const MissionArchiveInfo& archive, SettingsMenuResult& r) {
  int& limit = s.history.maxSavedMissions;
  int value = pendingKeep_ >= 0 ? pendingKeep_ : limit;
  // Typed digits must not count one by one ("30" over "50" would first ask to delete down to
  // 3). With ImGuiItemFlags_LiveEditOnInputScalar off (ImGui >= 1.92.9 default, pinned here)
  // InputInt writes the typed value and returns true only when the edit ends (Enter, Tab,
  // focus lost): the IsItemDeactivatedAfterEdit() moment. The -/+ step buttons write and
  // return true at once (Ctrl: steps of 10). Covered by a headless test in imgui_smoke_tests.
  ImGui::PushItemFlag(ImGuiItemFlags_LiveEditOnInputScalar, false);
  const bool edited = ImGui::InputInt("Keep missions", &value, 1, 10);
  ImGui::PopItemFlag();
  if (edited && pendingKeep_ < 0) {
    value = value < 0 ? 0 : (value > 1000 ? 1000 : value);
    const int deletes = archive.counting ? 0 : missionsDeletedByNewLimit(limit, value, archive.savedMissions);
    if (deletes > 0) {
      pendingKeep_ = value;
      pendingDeletes_ = deletes;
      ImGui::OpenPopup("Delete older missions?");
    } else if (value != limit) {
      limit = value;
      r.changed = true;
    }
  }
  helpMarker("How many saved missions to keep. When a mission is saved, the oldest ones beyond "
             "this number are deleted from disk. 0 = unlimited (keep every mission).");
  if (limit == 0) ImGui::TextDisabled("0 = unlimited: every saved mission is kept.");
  else ImGui::TextDisabled("Keeps the newest %d. 0 = unlimited.", limit);
  if (!archive.counting) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d saved now)", archive.savedMissions);
  }

  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Delete older missions?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Keep only the newest %d missions?", pendingKeep_);
    ImGui::TextColored(kWarn, "This will delete %d older mission%s on the next save.", pendingDeletes_,
                       pendingDeletes_ == 1 ? "" : "s");
    ImGui::TextDisabled("Deleted missions cannot be recovered.");
    ImGui::Spacing();
    if (ImGui::Button("Apply") && pendingKeep_ >= 0) {
      limit = pendingKeep_;
      r.changed = true;
      pendingKeep_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      pendingKeep_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else if (!ImGui::IsPopupOpen("Delete older missions?")) {
    pendingKeep_ = -1;  // the popup went away without an answer
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
