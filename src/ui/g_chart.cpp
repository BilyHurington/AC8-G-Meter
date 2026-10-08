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

constexpr ImVec4 kSustainedMark{1.0f, 1.0f, 1.0f, 1.0f};  // sustained max / min markers

// Label plates already placed this frame; a new one moves away from the curve (up for a
// maximum, down for a minimum, the other way at the plot edge) until it overlaps none.
struct LabelPlacer {
  std::vector<ImVec4> boxes;  // x0, y0, x1, y1
  float top = 0.0f, bottom = 0.0f;

  ImVec2 place(float x, float y, ImVec2 size, bool up) {
    const float step = size.y + 2.0f;
    for (int attempt = 0; attempt < 8; ++attempt) {
      const float yy = std::clamp(y, top + 1.0f, bottom - size.y - 1.0f);
      bool overlaps = false;
      for (const ImVec4& b : boxes)
        if (x - 3.0f < b.z && x + size.x + 3.0f > b.x && yy < b.w && yy + size.y > b.y) overlaps = true;
      if (!overlaps) {
        boxes.emplace_back(x - 3.0f, yy, x + size.x + 3.0f, yy + size.y);
        return ImVec2(x, yy);
      }
      // At the edge, reverse direction.
      if ((up && yy <= top + 1.0f) || (!up && yy >= bottom - size.y - 1.0f)) up = !up;
      y = yy + (up ? -step : step);
    }
    boxes.emplace_back(x - 3.0f, y, x + size.x + 3.0f, y + size.y);
    return ImVec2(x, y);
  }
};

// Value shown for the secondary series of a point: AoA is a maximum, onset keeps its sign.
float secondaryValue(const ChartPoint& p, ChartSeries s) {
  if (s == ChartSeries::Aoa) return p.secHi;
  return std::abs(p.secLo) > std::abs(p.secHi) ? p.secLo : p.secHi;
}

}  // namespace

float GChart::height(float plotHeight) {
  // The header line (frame height), item spacing, then the plot (see draw()).
  return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + std::max(plotHeight, ImGui::GetFontSize() * 6.0f);
}

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
  const Highlight highlight = highlight_;
  highlight_.set = false;  // one frame only
  // A span that was not shown on the previous draw (the hover just started or moved on).
  const bool newHighlight =
      highlight.set && !(shown_.set && shown_.t0 == highlight.t0 && shown_.t1 == highlight.t1);
  shown_ = highlight;
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
    hasSustained_ = traceHasSustained(trace);
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
  // A newly highlighted span outside the zoomed view: bring it into view, once (the user can
  // pan away again while it stays highlighted), never while the user drags.
  if (newHighlight && !active) view_ = revealSpan(view_, highlight.t0, highlight.t1, minT, maxT);
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
  // Highlighted span (results table hover), with its edges marked.
  if (highlight.set && highlight.t1 > highlight.t0) {
    const ImVec4 c = highlight.warm ? kHudWarn : kHudSecondary;
    const float x0 = m.x(highlight.t0), x1 = std::max(m.x(highlight.t1), x0 + 1.0f);
    dl->AddRectFilled(ImVec2(x0, m.min.y), ImVec2(x1, m.max.y), col(c, 0.22f));
    dl->AddLine(ImVec2(x0, m.min.y), ImVec2(x0, m.max.y), col(c, 0.9f), 1.5f);
    dl->AddLine(ImVec2(x1, m.min.y), ImVec2(x1, m.max.y), col(c, 0.9f), 1.5f);
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

  // Live gMin..gMax band, then the sustained line on top (the live mean when the trace has no
  // sustained values), with gaps where it has none.
  const ImU32 band = col(kHudAccent, 0.2f);
  for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
    const ChartPoint& a = points_[i];
    const ChartPoint& b = points_[i + 1];
    const float xa = m.x(a.t), xb = m.x(b.t);
    dl->AddQuadFilled(ImVec2(xa, m.y(a.gHi)), ImVec2(xb, m.y(b.gHi)), ImVec2(xb, m.y(b.gLo)), ImVec2(xa, m.y(a.gLo)),
                      band);
  }
  {
    std::vector<ImVec2> seg;
    auto flush = [&] {
      if (seg.size() == 1) seg.emplace_back(seg[0].x + 1.0f, seg[0].y);
      if (seg.size() >= 2) dl->AddPolyline(seg.data(), static_cast<int>(seg.size()), col(kHudAccent, 1.0f), 1.6f);
      seg.clear();
    };
    for (const ChartPoint& p : points_) {
      const float v = hasSustained_ ? p.gSustained : p.gMean;
      if (!std::isfinite(v)) {
        flush();
        continue;
      }
      seg.emplace_back(m.x(p.t), m.y(v));
    }
    flush();
  }

  // Labels last, on plates over the curve: threshold, then the markers (from the statistics,
  // which see every sample): peak max / min (orange dot, on the band) and sustained max / min
  // (white diamond, on the line).
  LabelPlacer labels;
  labels.top = m.min.y;
  labels.bottom = m.max.y;

  const MissionStats& st = report.stats;
  const bool peak = st.hasGData && !report.legacyFilteredG;  // schema-1 archives have none
  // Marker positions are obstacles for every label, so no label hides a marker.
  auto inView = [&](double t) { return t >= view_.t0 && t <= view_.t1; };
  auto reserveMarker = [&](double t, double g) {
    if (!inView(t)) return;
    const ImVec2 p(m.x(t), m.y(static_cast<float>(g)));
    labels.boxes.emplace_back(p.x - 6.0f, p.y - 6.0f, p.x + 6.0f, p.y + 6.0f);
  };
  if (peak) {
    reserveMarker(st.maxGAtS, st.maxG);
    reserveMarker(st.minGAtS, st.minG);
  }
  if (st.hasSustainedData) {
    reserveMarker(st.sustainedMaxGAtS, st.sustainedMaxG);
    reserveMarker(st.sustainedMinGAtS, st.sustainedMinG);
  }
  char thrLabel[32];
  std::snprintf(thrLabel, sizeof thrLabel, "HIGH-G %.1f", static_cast<double>(threshold));
  const ImVec2 thrSize = ImGui::CalcTextSize(thrLabel);
  const ImVec2 thrPos = labels.place(m.max.x - thrSize.x - fs * 0.4f, yThr - thrSize.y - 2.0f, thrSize, true);
  labelBox(dl, thrPos, col(kHudWarn, 0.9f), thrLabel);
  // Marker shapes: a white diamond on the line (sustained), an orange dot on the band (peak).
  auto shape = [&](double t, double g, bool sustained) {
    if (!inView(t)) return;
    const ImVec2 p(m.x(t), m.y(static_cast<float>(g)));
    if (sustained) {
      const float r = 4.5f;
      dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y),
                        col(ImVec4(0, 0, 0, 1), 0.8f));
      dl->AddQuad(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y),
                  col(kSustainedMark, 1.0f), 1.5f);
    } else {
      dl->AddCircleFilled(p, 3.5f, col(kHudWarn, 1.0f));
      dl->AddCircle(p, 5.5f, col(kHudWarn, 0.6f));
    }
  };
  // Label of a marker: peak labels centred over / under their marker, sustained ones beside
  // it. When the placer has to move a label away from its marker (another label is there), a
  // thin leader line joins them.
  auto label = [&](double t, double g, bool above, bool sustained, const std::string& text) {
    if (!inView(t)) return;
    const ImVec2 p(m.x(t), m.y(static_cast<float>(g)));
    const ImVec4 c = sustained ? kSustainedMark : kHudWarn;
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float lx = sustained ? std::clamp(p.x + 8.0f, m.min.x + 2.0f, m.max.x - ts.x - 2.0f)
                               : std::clamp(p.x - ts.x * 0.5f, m.min.x + 2.0f, m.max.x - ts.x - 2.0f);
    const float ly = sustained ? p.y - ts.y * 0.5f : (above ? p.y - ts.y - 6.0f : p.y + 6.0f);
    const ImVec2 at = labels.place(lx, ly, ts, above);
    if (std::abs(at.y - ly) > 1.0f) {
      // From the marker's edge to the nearest point of the plate.
      const float ex = std::clamp(p.x, at.x - 3.0f, at.x + ts.x + 3.0f);
      const float ey = at.y + ts.y < p.y ? at.y + ts.y : (at.y > p.y ? at.y : p.y);
      const float dx = ex - p.x, dy = ey - p.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 7.0f) dl->AddLine(ImVec2(p.x + dx / len * 6.0f, p.y + dy / len * 6.0f), ImVec2(ex, ey), col(c, 0.7f), 1.0f);
    }
    labelBox(dl, at, col(c, 1.0f), text.c_str());
  };
  // Peak and sustained extreme at (almost) the same value and screen position: one label
  // ("MAX +8.2 G (sus +8.2)"), instead of two plates fighting for the same place.
  auto merged = [&](double peakT, double peakG, double susT, double susG) {
    return peak && st.hasSustainedData && inView(peakT) && inView(susT) && std::abs(peakG - susG) <= 0.05 &&
           std::abs(m.x(peakT) - m.x(susT)) <= 2.0f * fs;
  };
  const bool mergedMax = merged(st.maxGAtS, st.maxG, st.sustainedMaxGAtS, st.sustainedMaxG);
  const bool mergedMin = merged(st.minGAtS, st.minG, st.sustainedMinGAtS, st.sustainedMinG);
  // Sustained first: when both coincide the peak dot stays visible inside the diamond.
  if (st.hasSustainedData) {
    shape(st.sustainedMaxGAtS, st.sustainedMaxG, true);
    shape(st.sustainedMinGAtS, st.sustainedMinG, true);
  }
  if (peak) {
    shape(st.maxGAtS, st.maxG, false);
    shape(st.minGAtS, st.minG, false);
  }
  if (st.hasSustainedData) {
    if (!mergedMax) label(st.sustainedMaxGAtS, st.sustainedMaxG, true, true, "SUS MAX " + gLabel(st.sustainedMaxG));
    if (!mergedMin) label(st.sustainedMinGAtS, st.sustainedMinG, false, true, "SUS MIN " + gLabel(st.sustainedMinG));
  }
  if (peak) {
    label(st.maxGAtS, st.maxG, true, false,
          "MAX " + gLabel(st.maxG) + (mergedMax ? " (sus " + formatNumber(st.sustainedMaxG, 1, true) + ")" : ""));
    label(st.minGAtS, st.minG, false, false,
          "MIN " + gLabel(st.minG) + (mergedMin ? " (sus " + formatNumber(st.sustainedMinG, 1, true) + ")" : ""));
  }

  // Legend, top left.
  {
    // Schema-1 archives: band and line are that version's filtered G.
    const char* bandText = report.legacyFilteredG ? "filtered min-max (v1 file)" : "live min-max";
    const char* lineText = report.legacyFilteredG ? "filtered mean" : hasSustained_ ? "sustained" : "live mean";
    const float h = ImGui::GetTextLineHeight();
    const float w = ImGui::CalcTextSize(bandText).x + ImGui::CalcTextSize(lineText).x + fs * 4.6f;
    ImVec2 o(m.min.x + fs * 0.4f, m.min.y + fs * 0.3f);
    dl->AddRectFilled(ImVec2(o.x - 3.0f, o.y), ImVec2(o.x + w, o.y + h), col(ImVec4(0, 0, 0, 1), 0.65f), 2.0f);
    dl->AddRectFilled(ImVec2(o.x, o.y + h * 0.25f), ImVec2(o.x + fs, o.y + h * 0.75f), col(kHudAccent, 0.35f));
    o.x += fs * 1.3f;
    textAt(dl, o, axisText, bandText);
    o.x += ImGui::CalcTextSize(bandText).x + fs * 0.8f;
    dl->AddLine(ImVec2(o.x, o.y + h * 0.5f), ImVec2(o.x + fs, o.y + h * 0.5f), col(kHudAccent, 1.0f), 1.6f);
    o.x += fs * 1.3f;
    textAt(dl, o, axisText, lineText);
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
    const float lineG = hasSustained_ && std::isfinite(p.gSustained) ? p.gSustained : p.gMean;
    dl->AddCircleFilled(ImVec2(x, m.y(lineG)), 3.0f, col(kHudAccent, 1.0f));
    ImGui::BeginTooltip();
    ImGui::TextColored(kHudAccent, "%s", formatClock(t).c_str());
    ImGui::Text("G     %s  (min %s, max %s)", formatNumber(p.gMean, 2, true).c_str(),
                formatNumber(p.gMin, 2, true).c_str(), formatNumber(p.gMax, 2, true).c_str());
    if (hasSustained_) ImGui::Text("Sust. %s", formatNumber(p.gSustained, 2, true).c_str());
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
