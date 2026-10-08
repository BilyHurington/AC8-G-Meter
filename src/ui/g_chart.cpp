#include "ui/g_chart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "ui/format.h"
#include "ui/style.h"

namespace ft {
namespace {

constexpr double kMinSpanS = 2.0;      // deepest zoom
constexpr double kWheelZoom = 0.8;     // span factor per wheel notch

ImU32 col(ImVec4 c, float alpha) { return ImGui::GetColorU32(withAlpha(c, alpha)); }

// Plot rectangle <-> data mapping.
struct Mapping {
  ImVec2 min, max;  // plot area in screen space
  TimeView view;
  float lo = 0.0f, hi = 1.0f;  // G range
  float x(double t) const {
    return min.x + static_cast<float>((t - view.t0) / view.span()) * (max.x - min.x);
  }
  double t(float px) const { return view.t0 + static_cast<double>((px - min.x) / (max.x - min.x)) * view.span(); }
  float y(float v, float a, float b) const { return max.y - (v - a) / (b - a) * (max.y - min.y); }
  float y(float g) const { return y(g, lo, hi); }
};

void textAt(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text) { dl->AddText(pos, color, text); }

// Text on a dark plate, readable over the curve.
void labelBox(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text) {
  const ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(pos.x - 3.0f, pos.y), ImVec2(pos.x + ts.x + 3.0f, pos.y + ts.y), col(ImVec4(0, 0, 0, 1), 0.65f),
                    2.0f);
  dl->AddText(pos, color, text);
}

std::string gLabel(double g) { return formatNumber(g, 1, true) + " G"; }

// Value shown for the secondary series of a point: AoA is a maximum, onset keeps its sign.
float secondaryValue(const ChartPoint& p, ChartSeries s) {
  if (s == ChartSeries::Aoa) return p.secHi;
  return std::abs(p.secLo) > std::abs(p.secHi) ? p.secLo : p.secHi;
}

}  // namespace

void GChart::draw(const MissionReport& report, ImVec2 size) {
  const MissionTrace& trace = report.trace;
  const float fs = ImGui::GetFontSize();
  ImGui::PushID(this);

  // Header: overlay series choice and the interaction hint.
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Overlay:");
  int overlay = static_cast<int>(overlay_);
  ImGui::SameLine();
  ImGui::RadioButton("none", &overlay, static_cast<int>(ChartSeries::None));
  ImGui::SameLine();
  ImGui::RadioButton("onset", &overlay, static_cast<int>(ChartSeries::Onset));
  ImGui::SameLine();
  ImGui::RadioButton("AoA", &overlay, static_cast<int>(ChartSeries::Aoa));
  overlay_ = static_cast<ChartSeries>(overlay);
  const char* hint = "wheel: zoom   drag: pan   double-click: reset";
  const float hintW = ImGui::CalcTextSize(hint).x;
  ImGui::SameLine();
  const float hintX = ImGui::GetContentRegionAvail().x - hintW;
  if (hintX > fs) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + hintX);
    ImGui::TextDisabled("%s", hint);
  } else {
    ImGui::NewLine();
  }

  if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
  size.x = std::max(size.x, fs * 10.0f);
  size.y = std::max(size.y, fs * 6.0f);
  ImGui::InvisibleButton("##plot", size);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  const ImVec2 boxMin = ImGui::GetItemRectMin();
  const ImVec2 boxMax = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool showSecondary = overlay_ != ChartSeries::None;

  Mapping m;
  m.min = ImVec2(boxMin.x + fs * 3.2f, boxMin.y + fs * 0.5f);
  m.max = ImVec2(boxMax.x - (showSecondary ? fs * 3.2f : fs * 0.8f), boxMax.y - fs * 1.6f);
  dl->AddRectFilled(m.min, m.max, ImGui::GetColorU32(ImVec4(0.03f, 0.05f, 0.04f, 1.0f)), 3.0f);
  dl->AddRect(m.min, m.max, ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
  if (trace.empty() || m.max.x - m.min.x < 10.0f || m.max.y - m.min.y < 10.0f) {
    const char* msg = "No trace recorded.";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    textAt(dl, ImVec2((m.min.x + m.max.x - ts.x) * 0.5f, (m.min.y + m.max.y - ts.y) * 0.5f),
           ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
    ImGui::PopID();
    return;
  }

  // ---- view: reset on a new report, then wheel zoom / drag pan / double-click reset ------
  const double minT = trace.startS, maxT = std::max(trace.endS(), trace.startS + 1e-3);
  const ReportKey key{&report, report.id, trace.size(), trace.startS, report.stats.durationS};
  if (!(key == key_)) {
    key_ = key;
    viewValid_ = false;
    pointsColumns_ = -1;
  }
  if (!viewValid_) {
    view_ = {minT, maxT};
    viewValid_ = true;
  }
  m.view = view_;
  const ImGuiIO& io = ImGui::GetIO();
  const float plotW = m.max.x - m.min.x;
  if (hovered) {
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel zooms instead of scrolling the window
    if (io.MouseWheel != 0.0f) {
      const double anchor = m.t(std::clamp(io.MousePos.x, m.min.x, m.max.x));
      view_ = zoomView(view_, anchor, std::pow(kWheelZoom, static_cast<double>(io.MouseWheel)), minT, maxT, kMinSpanS);
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) view_ = {minT, maxT};
  }
  if (active && io.MouseDelta.x != 0.0f)
    view_ = panView(view_, -static_cast<double>(io.MouseDelta.x / plotW) * view_.span(), minT, maxT);
  m.view = view_;

  // ---- data (decimated per pixel column, cached) ----------------------------------------
  const int columns = std::max(1, static_cast<int>(plotW));
  if (!(pointsView_ == view_) || pointsColumns_ != columns || pointsOverlay_ != overlay_) {
    decimateTrace(trace, view_.t0, view_.t1, columns, overlay_, points_);
    pointsView_ = view_;
    pointsColumns_ = columns;
    pointsOverlay_ = overlay_;
  }
  float lo = 1.0f, hi = 1.0f;
  if (pointsRange(points_, false, lo, hi)) {
    lo = std::min(lo, 1.0f);
    hi = std::max(hi, 1.0f);
  }
  const float threshold = static_cast<float>(report.highGEnter);
  hi = std::max(hi, threshold + 0.5f);  // the threshold line is always in view
  const float pad = std::max(0.3f, 0.06f * (hi - lo));
  m.lo = lo - pad;
  m.hi = hi + pad;

  const ImU32 axisText = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  const ImU32 grid = col(ImVec4(0.30f, 0.45f, 0.35f, 1.0f), 0.28f);
  const float plotH = m.max.y - m.min.y;

  dl->PushClipRect(m.min, m.max, true);
  // High-G episodes: shaded columns (half a bucket, or half a pixel, either side).
  const float half = std::max(0.5f, 0.5f * plotW / static_cast<float>(view_.span() * trace.hz));
  for (std::size_t i = 0; i < points_.size();) {
    if (!points_[i].highG) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j + 1 < points_.size() && points_[j + 1].highG) ++j;
    dl->AddRectFilled(ImVec2(m.x(points_[i].t) - half, m.min.y), ImVec2(m.x(points_[j].t) + half, m.max.y),
                      col(kHudWarn, 0.13f));
    i = j + 1;
  }
  dl->PopClipRect();

  // Grid and G axis (whole G).
  for (double g : axisTicks(m.lo, m.hi, plotH / (m.hi - m.lo), fs * 1.8f, gTickSteps())) {
    const float y = m.y(static_cast<float>(g));
    dl->AddLine(ImVec2(m.min.x, y), ImVec2(m.max.x, y), g == 0.0 ? col(ImVec4(0.5f, 0.6f, 0.55f, 1.0f), 0.45f) : grid);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%+.0f", g);
    if (g == 0.0) std::snprintf(buf, sizeof buf, "0");
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    textAt(dl, ImVec2(m.min.x - ts.x - fs * 0.4f, y - ts.y * 0.5f), axisText, buf);
  }
  textAt(dl, ImVec2(boxMin.x, m.min.y - fs * 0.45f), axisText, "G");
  // Time axis (mm:ss).
  for (double t : axisTicks(view_.t0, view_.t1, plotW / view_.span(), fs * 5.0f, timeTickSteps())) {
    const float x = m.x(t);
    dl->AddLine(ImVec2(x, m.min.y), ImVec2(x, m.max.y), grid);
    const std::string label = formatClock(t);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const float lx = std::clamp(x - ts.x * 0.5f, boxMin.x, boxMax.x - ts.x);
    textAt(dl, ImVec2(lx, m.max.y + fs * 0.25f), axisText, label.c_str());
  }

  dl->PushClipRect(m.min, m.max, true);
  // Reference lines: 1 G and the high-G threshold.
  const float y1 = m.y(1.0f);
  dl->AddLine(ImVec2(m.min.x, y1), ImVec2(m.max.x, y1), col(ImVec4(0.75f, 0.8f, 0.78f, 1.0f), 0.35f));
  const float yThr = m.y(threshold);
  dl->AddLine(ImVec2(m.min.x, yThr), ImVec2(m.max.x, yThr), col(kHudWarn, 0.75f));

  // Secondary series (onset or AoA) on its own right-hand axis, under the G curve; gaps where
  // it is invalid.
  float sLo = 0.0f, sHi = 1.0f;
  const bool haveSecondary = showSecondary && pointsRange(points_, true, sLo, sHi);
  if (haveSecondary) {
    if (overlay_ == ChartSeries::Onset) {
      const float a = std::max({std::abs(sLo), std::abs(sHi), 1.0f});  // symmetric around 0
      sLo = -a * 1.1f;
      sHi = a * 1.1f;
    } else {
      const float spanPad = std::max(1.0f, 0.08f * (sHi - sLo));
      sLo -= spanPad;
      sHi += spanPad;
    }
    std::vector<ImVec2> seg;
    auto flush = [&] {
      if (seg.size() >= 2)
        dl->AddPolyline(seg.data(), static_cast<int>(seg.size()), col(kHudSecondary, 0.6f), 1.2f);
      seg.clear();
    };
    for (const ChartPoint& p : points_) {
      const float v = secondaryValue(p, overlay_);
      if (std::isnan(v)) {
        flush();
        continue;
      }
      seg.emplace_back(m.x(p.t), m.y(v, sLo, sHi));
    }
    flush();
  }

  // gMin..gMax band, then the mean line on top.
  const ImU32 band = col(kHudAccent, 0.22f);
  for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
    const ChartPoint& a = points_[i];
    const ChartPoint& b = points_[i + 1];
    const float xa = m.x(a.t), xb = m.x(b.t);
    dl->AddQuadFilled(ImVec2(xa, m.y(a.gHi)), ImVec2(xb, m.y(b.gHi)), ImVec2(xb, m.y(b.gLo)), ImVec2(xa, m.y(a.gLo)),
                      band);
  }
  std::vector<ImVec2> line;
  line.reserve(points_.size());
  for (const ChartPoint& p : points_) line.emplace_back(m.x(p.t), m.y(p.gMean));
  if (line.size() == 1) line.emplace_back(line[0].x + 1.0f, line[0].y);
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), col(kHudAccent, 1.0f), 1.5f);

  // Labels last, on plates over the curve: threshold, then max / min G markers (from the
  // statistics, which see every sample).
  char thrLabel[32];
  std::snprintf(thrLabel, sizeof thrLabel, "HIGH-G %.1f", static_cast<double>(threshold));
  const ImVec2 thrSize = ImGui::CalcTextSize(thrLabel);
  labelBox(dl, ImVec2(m.max.x - thrSize.x - fs * 0.4f, yThr - thrSize.y - 2.0f), col(kHudWarn, 0.9f), thrLabel);

  const MissionStats& st = report.stats;
  if (st.hasGData) {
    auto marker = [&](double t, double g, bool above) {
      if (t < view_.t0 || t > view_.t1) return;
      const ImVec2 p(m.x(t), m.y(static_cast<float>(g)));
      dl->AddCircleFilled(p, 3.5f, col(kHudWarn, 1.0f));
      dl->AddCircle(p, 5.5f, col(kHudWarn, 0.6f));
      const std::string label = (above ? "MAX " : "MIN ") + gLabel(g);
      const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
      const float lx = std::clamp(p.x - ts.x * 0.5f, m.min.x + 2.0f, m.max.x - ts.x - 2.0f);
      float ly = above ? p.y - ts.y - 6.0f : p.y + 6.0f;
      ly = std::clamp(ly, m.min.y + 1.0f, m.max.y - ts.y - 1.0f);
      labelBox(dl, ImVec2(lx, ly), col(kHudWarn, 1.0f), label.c_str());
    };
    marker(st.maxGAtS, st.maxG, true);
    marker(st.minGAtS, st.minG, false);
  }
  dl->PopClipRect();

  if (haveSecondary) {
    const ImU32 sText = col(kHudSecondary, 0.85f);
    for (double v : axisTicks(sLo, sHi, plotH / (sHi - sLo), fs * 1.8f, niceTickSteps())) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "%g", v);
      textAt(dl, ImVec2(m.max.x + fs * 0.4f, m.y(static_cast<float>(v), sLo, sHi) - fs * 0.5f), sText, buf);
    }
    const char* unit = overlay_ == ChartSeries::Onset ? "G/s" : "AoA";
    textAt(dl, ImVec2(boxMax.x - ImGui::CalcTextSize(unit).x, m.min.y - fs * 0.45f), sText, unit);
  }

  // Hover: crosshair and the values of the bucket under the cursor.
  if (hovered && !active && io.MousePos.x >= m.min.x && io.MousePos.x <= m.max.x && io.MousePos.y >= m.min.y &&
      io.MousePos.y <= m.max.y) {
    const std::size_t i = bucketAt(trace, m.t(io.MousePos.x));
    const TracePoint p = trace.at(i);
    const double t = trace.timeAt(i) + 0.5 / trace.hz;
    const float x = m.x(t);
    dl->AddLine(ImVec2(x, m.min.y), ImVec2(x, m.max.y), col(ImVec4(1, 1, 1, 1), 0.35f));
    dl->AddCircleFilled(ImVec2(x, m.y(p.gMean)), 3.0f, col(kHudAccent, 1.0f));
    ImGui::BeginTooltip();
    ImGui::TextColored(kHudAccent, "%s", formatClock(t).c_str());
    ImGui::Text("G     %s  (min %s, max %s)", formatNumber(p.gMean, 2, true).c_str(),
                formatNumber(p.gMin, 2, true).c_str(), formatNumber(p.gMax, 2, true).c_str());
    ImGui::Text("Onset %s G/s", formatNumber(p.onset, 1, true).c_str());
    if (std::isnan(p.aoaDeg)) ImGui::Text("AoA   --");
    else ImGui::Text("AoA   %s%s", formatNumber(p.aoaDeg, 1, false).c_str(), kDegreeSign);
    ImGui::Text("Speed %s", formatSpeedKmh(p.speed).c_str());
    ImGui::Text("Alt   %s m", formatNumber(p.altitude, 0, false).c_str());
    if (p.highG) ImGui::TextColored(kHudWarn, "high-G episode");
    ImGui::EndTooltip();
  }
  ImGui::PopID();
}

}  // namespace ft
