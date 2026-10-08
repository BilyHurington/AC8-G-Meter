#include "ui/mission_summary.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "ui/format.h"
#include "ui/g_chart.h"
#include "ui/style.h"

namespace ft {
namespace {

constexpr ImVec4 kAccent = kHudAccent;
constexpr ImVec4 kWarn = kHudWarn;

void row(const char* label, const std::string& value, const std::string& detail = {}) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", label);
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(value.c_str());
  if (!detail.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", detail.c_str());
  }
}

std::string at(double s) { return "at " + formatClock(s); }

void statsTable(const MissionStats& st) {
  if (!ImGui::BeginTable("##ft_summary_stats", 2, ImGuiTableFlags_SizingFixedFit)) return;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
  row("Flight time", formatClock(st.durationS));
  if (st.hasGData) {
    row("Max G", formatNumber(st.maxG, 1, true) + " G", at(st.maxGAtS));
    row("Min G", formatNumber(st.minG, 1, true) + " G", at(st.minGAtS));
    row("Max onset", formatNumber(st.maxOnset, 1, true) + " G/s");
    row("Max unload", formatNumber(st.minOnset, 1, true) + " G/s");
  } else {
    row("Max / min G", "--");
  }
  row("Max AoA", formatNumber(st.maxAoaDeg, 1, false) + kDegreeSign);
  row("Max speed", formatSpeedKmh(st.maxSpeed));
  row("Longest high-G", formatNumber(st.longestHighGS, 1, false) + " s");
  char buf[64];
  std::snprintf(buf, sizeof buf, "%d episode%s", st.highGEpisodes, st.highGEpisodes == 1 ? "" : "s");
  row("Total high-G", formatNumber(st.totalHighGS, 1, false) + " s", buf);
  ImGui::EndTable();
}

void histogram(const MissionStats& st) {
  double total = 0.0;
  for (double v : st.timeInGBinS) total += v;
  const float labelW = ImGui::GetFontSize() * 5.0f;
  for (int i = 0; i < kGBinCount; ++i) {
    const double secs = st.timeInGBinS[static_cast<std::size_t>(i)];
    const float frac = total > 0.0 ? static_cast<float>(secs / total) : 0.0f;
    ImGui::TextUnformatted(gBinLabel(i).c_str());
    ImGui::SameLine(labelW);
    char overlay[48];
    std::snprintf(overlay, sizeof overlay, "%.1f s  (%.0f%%)", secs, 100.0 * static_cast<double>(frac));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, i >= 5 ? kWarn : kAccent);  // bins from 5 G up
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), overlay);  // compact rows
    ImGui::PopStyleColor();
  }
}

}  // namespace

SummaryResult drawMissionSummary(const MissionReport& report, const SummaryContext& context, GChart& chart,
                                 bool& open, ImFont* titleFont) {
  SummaryResult result;
  if (!open) return result;
  const float fs = ImGui::GetFontSize();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(fs * 62.0f, vp->WorkSize.x * 0.92f), std::min(fs * 60.0f, vp->WorkSize.y * 0.92f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 34.0f, fs * 24.0f), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::SetNextWindowBgAlpha(1.0f);   // readable over a busy results screen
  char title[64];
  std::snprintf(title, sizeof title, "Mission %u summary###ft_summary", report.id);
  // NoFocusOnAppearing: popping up at mission end must not take keyboard focus from the game.
  if (!ImGui::Begin(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return result;
  }

  const MissionStats& st = report.stats;
  ImGui::PushFont(titleFont, ImGui::GetStyle().FontSizeBase * 1.6f);  // size before style scaling
  ImGui::TextColored(kAccent, "MISSION %u DEBRIEF", report.id);
  ImGui::PopFont();
  if (!context.subtitle.empty()) ImGui::TextDisabled("%s", context.subtitle.c_str());
  if (report.aborted) ImGui::TextColored(kWarn, "Mission aborted (left before the results screen)");
  if (!st.hasData) {
    ImGui::TextDisabled("No flight data was recorded during this mission.");
  } else {
    // The chart takes ~40% of the window; statistics and histogram share the row below.
    // (38%: at the default size everything down to the buttons fits without scrolling.)
    ImGui::SeparatorText(report.gMode == GMode::Total ? "G over time (total |n|)" : "G over time (Nz)");
    chart.draw(report, ImVec2(0.0f, std::max(fs * 11.0f, ImGui::GetWindowHeight() * 0.38f)));
    if (ImGui::BeginTable("##ft_summary_cols", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("stats", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("bins", ImGuiTableColumnFlags_WidthStretch, 1.1f);
      ImGui::TableNextColumn();
      ImGui::SeparatorText("Statistics");
      statsTable(st);
      ImGui::TableNextColumn();
      ImGui::SeparatorText("Time at G");
      histogram(st);
      ImGui::EndTable();
    }
  }

  ImGui::SeparatorText("Data quality");
  ImGui::Text("%llu samples accepted, %llu rejected",
              static_cast<unsigned long long>(st.acceptedSamples),
              static_cast<unsigned long long>(st.rejectedSamples));
  if (st.extremeGSamples > 0)
    ImGui::Text("%llu extreme-G samples (> %s G)", static_cast<unsigned long long>(st.extremeGSamples),
                formatNumber(report.extremeGThreshold, 0, false).c_str());

  if (!context.status.empty()) {
    const ImVec4 tone = context.statusTone == SummaryContext::Tone::Error  ? kHudError
                        : context.statusTone == SummaryContext::Tone::Busy ? kWarn
                                                                           : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, tone);
    ImGui::TextWrapped("%s", context.status.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Spacing();
  if (ImGui::Button("Close", ImVec2(fs * 6.0f, 0.0f))) open = false;
  if (context.canOpenHistory) {
    ImGui::SameLine();
    if (ImGui::Button("Mission history...")) result.openHistory = true;
  }
  ImGui::End();
  return result;
}

}  // namespace ft
