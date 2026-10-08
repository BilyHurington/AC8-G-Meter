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
std::string gValue(double g) { return formatNumber(g, 1, true) + " G"; }

// "Sustained G (>= 0.20 s)": W of the report (0: the filter was off, sustained = peak).
std::string sustainedLabel(const MissionReport& report, const char* what) {
  char buf[64];
  if (report.legacyFilteredG)  // schema-1 archive: its (only) extremes were the filtered G
    std::snprintf(buf, sizeof buf, "Filtered %s (v1 file)", what);
  else if (report.minExcursionS > 0.0)
    std::snprintf(buf, sizeof buf, "Sustained %s (>=%.1f s)", what, report.minExcursionS);  // fonts lack U+2265
  else
    std::snprintf(buf, sizeof buf, "Sustained %s", what);
  return buf;
}

void statsTable(const MissionReport& report) {
  const MissionStats& st = report.stats;
  if (!ImGui::BeginTable("##ft_summary_stats", 2, ImGuiTableFlags_SizingFixedFit)) return;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);  // fits the longest label
  row("Flight time", formatClock(st.durationS));
  // Peak: every reading as the live G showed it. Sustained: excursions shorter than W removed.
  // (Schema-1 archives recorded no peak: "--".)
  const bool peak = st.hasGData && !report.legacyFilteredG;
  if (peak) {
    row("Peak G", gValue(st.maxG), at(st.maxGAtS));
  } else {
    row("Peak G", "--");
  }
  if (st.hasSustainedData)
    row(sustainedLabel(report, "G").c_str(), gValue(st.sustainedMaxG), at(st.sustainedMaxGAtS));
  else
    row(sustainedLabel(report, "G").c_str(), "--");
  if (peak) {
    row("Peak min G", gValue(st.minG), at(st.minGAtS));
  } else {
    row("Peak min G", "--");
  }
  if (st.hasSustainedData)
    row(sustainedLabel(report, "min G").c_str(), gValue(st.sustainedMinG), at(st.sustainedMinGAtS));
  else
    row(sustainedLabel(report, "min G").c_str(), "--");
  if (st.hasGData) {
    row("Max onset", formatNumber(st.maxOnset, 1, true) + " G/s");
    row("Max unload", formatNumber(st.minOnset, 1, true) + " G/s");
  }
  row("Max AoA", formatNumber(st.maxAoaDeg, 1, false) + kDegreeSign);
  row("Max speed", formatSpeedKmh(st.maxSpeed));
  // Episodes are counted on the sustained G, so a short bump never makes one.
  row("Longest sustained high-G", formatNumber(st.longestHighGS, 1, false) + " s");
  char buf[64];
  std::snprintf(buf, sizeof buf, "%d episode%s", st.highGEpisodes, st.highGEpisodes == 1 ? "" : "s");
  row("Total sustained high-G", formatNumber(st.totalHighGS, 1, false) + " s", buf);
  ImGui::EndTable();
}

// Highest / lowest N-second average of the live G. Hovering a row (or one of its cells)
// highlights that window on the chart: the max one in orange, the min one in blue.
void averagesTable(const MissionReport& report, GChart& chart) {
  const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
  if (!ImGui::BeginTable("##ft_summary_avg", 3, flags)) return;
  const float fs = ImGui::GetFontSize();
  ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthFixed, fs * 4.5f);
  ImGui::TableSetupColumn("Max avg", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Min avg", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();
  for (std::size_t i = 0; i < report.windowed.size(); ++i) {
    const WindowedG& w = report.windowed[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    char label[32];
    std::snprintf(label, sizeof label, "%g s", w.windowS);
    // A selectable spanning the row makes the whole row hoverable.
    ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
    const bool rowHovered = ImGui::IsItemHovered();
    bool maxHovered = false, minHovered = false;
    auto cell = [&](double g, double startS, bool& hovered) {
      ImGui::TableNextColumn();
      if (!w.valid) {
        ImGui::TextDisabled("--");
        return;
      }
      ImGui::TextUnformatted(gValue(g).c_str());
      hovered = ImGui::IsItemHovered();
      ImGui::SameLine();
      ImGui::TextDisabled("%s", at(startS).c_str());
      hovered = hovered || ImGui::IsItemHovered();
    };
    cell(w.maxAvgG, w.maxAvgStartS, maxHovered);
    cell(w.minAvgG, w.minAvgStartS, minHovered);
    if (w.valid && (rowHovered || maxHovered || minHovered)) {
      // The row: the max window (the one the user most likely looks for); a cell: its own.
      if (minHovered)
        chart.setHighlight(w.minAvgStartS, w.minAvgStartS + w.windowS, false);
      else
        chart.setHighlight(w.maxAvgStartS, w.maxAvgStartS + w.windowS, true);
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Average of the live G over the best / worst window of each length. Hover a row to see it "
                     "on the chart.");
  ImGui::PopStyleColor();
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
    // The chart's space is reserved first and the chart drawn after the tables, so the
    // average row hovered in this frame is highlighted in this frame (not one frame late).
    const float plotH = std::max(fs * 11.0f, ImGui::GetWindowHeight() * 0.38f);
    const ImVec2 chartPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, GChart::height(plotH)));
    if (ImGui::BeginTable("##ft_summary_cols", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("stats", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("averages", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextColumn();
      ImGui::SeparatorText("Statistics");
      statsTable(report);
      ImGui::TableNextColumn();
      ImGui::SeparatorText("Average G over window");
      averagesTable(report, chart);
      ImGui::EndTable();
    }
    const ImVec2 below = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(chartPos);
    chart.draw(report, ImVec2(0.0f, plotH));
    ImGui::SetCursorScreenPos(below);
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
