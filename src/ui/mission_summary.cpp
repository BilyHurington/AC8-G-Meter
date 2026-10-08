#include "ui/mission_summary.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "ui/format.h"

namespace ft {
namespace {

constexpr ImVec4 kAccent{0.576f, 0.965f, 0.659f, 1.0f};
constexpr ImVec4 kWarn{1.0f, 0.62f, 0.35f, 1.0f};

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
  row("Max G", formatNumber(st.maxG, 1, true) + " G", at(st.maxGAtS));
  row("Min G", formatNumber(st.minG, 1, true) + " G", at(st.minGAtS));
  row("Max onset", formatNumber(st.maxOnset, 1, true) + " G/s");
  row("Max unload", formatNumber(st.minOnset, 1, true) + " G/s");
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
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, i >= 5 ? kWarn : kAccent);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), overlay);
    ImGui::PopStyleColor();
  }
}

void timeline(const MissionReport& report, float highGEnter) {
  const std::vector<float>& g = report.gTimeline;
  if (g.size() < 2) {
    ImGui::TextDisabled("No timeline recorded.");
    return;
  }
  const auto [lo, hi] = std::minmax_element(g.begin(), g.end());
  const float scaleMin = std::min(*lo, 0.0f) - 0.5f;
  const float scaleMax = std::max(*hi, highGEnter) + 0.5f;
  const float height = ImGui::GetFontSize() * 7.0f;
  ImGui::PushStyleColor(ImGuiCol_PlotLines, kAccent);
  ImGui::PlotLines("##ft_g_timeline", g.data(), static_cast<int>(g.size()), 0, nullptr, scaleMin,
                   scaleMax, ImVec2(-FLT_MIN, height));
  ImGui::PopStyleColor();

  // Reference lines drawn over the plot's inner area (frame rect minus frame padding).
  const ImVec2 pad = ImGui::GetStyle().FramePadding;
  const ImVec2 mn(ImGui::GetItemRectMin().x + pad.x, ImGui::GetItemRectMin().y + pad.y);
  const ImVec2 mx(ImGui::GetItemRectMax().x - pad.x, ImGui::GetItemRectMax().y - pad.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto yFor = [&](float v) { return mx.y - (v - scaleMin) / (scaleMax - scaleMin) * (mx.y - mn.y); };
  const float yThr = yFor(highGEnter);
  dl->AddLine(ImVec2(mn.x, yThr), ImVec2(mx.x, yThr), ImGui::GetColorU32(ImVec4(kWarn.x, kWarn.y, kWarn.z, 0.7f)));
  char label[32];
  std::snprintf(label, sizeof label, "high-G %.1f", static_cast<double>(highGEnter));
  dl->AddText(ImVec2(mn.x + 4.0f, yThr - ImGui::GetFontSize() - 1.0f), ImGui::GetColorU32(kWarn), label);
  const float y1 = yFor(1.0f);
  dl->AddLine(ImVec2(mn.x, y1), ImVec2(mx.x, y1), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f));

  // The timeline starts at timelineStartS (non-zero after a mid-mission G mode change).
  const double startS = report.timelineStartS;
  const double endS = startS + static_cast<double>(g.size() - 1) / report.timelineHz;
  ImGui::TextDisabled("%s", formatClock(startS).c_str());
  const std::string end = formatClock(endS);
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(end.c_str()).x);
  ImGui::TextDisabled("%s", end.c_str());
}

}  // namespace

void drawMissionSummary(const MissionReport& report, const Settings& settings, bool& open,
                        ImFont* titleFont) {
  if (!open) return;
  const float fs = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(fs * 34.0f, 0.0f), ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(1.0f);   // readable over a busy results screen
  char title[64];
  std::snprintf(title, sizeof title, "Mission %u summary###ft_summary", report.id);
  // NoFocusOnAppearing: popping up at mission end must not take keyboard focus from the game.
  if (!ImGui::Begin(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }
  // AlwaysAutoResize: keep the width stable by reserving it explicitly.
  ImGui::Dummy(ImVec2(fs * 32.0f, 0.0f));

  const MissionStats& st = report.stats;
  ImGui::PushFont(titleFont, ImGui::GetStyle().FontSizeBase * 1.6f);  // size before style scaling
  ImGui::TextColored(kAccent, "MISSION %u DEBRIEF", report.id);
  ImGui::PopFont();
  if (report.aborted) ImGui::TextColored(kWarn, "Mission aborted (left before the results screen)");
  if (!st.hasData) {
    ImGui::TextDisabled("No flight data was recorded during this mission.");
  } else {
    statsTable(st);
    ImGui::SeparatorText("Time at G");
    histogram(st);
    ImGui::SeparatorText("G over time");
    timeline(report, settings.metrics.highGEnter);
  }

  ImGui::SeparatorText("Data quality");
  ImGui::Text("%llu samples accepted, %llu rejected",
              static_cast<unsigned long long>(st.acceptedSamples),
              static_cast<unsigned long long>(st.rejectedSamples));

  ImGui::Spacing();
  if (ImGui::Button("Close", ImVec2(fs * 6.0f, 0.0f))) open = false;
  ImGui::End();
}

}  // namespace ft
