#include "ui/chart_math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ft {
namespace {

TimeView clampView(TimeView v, double minT, double maxT) {
  const double span = std::min(v.span(), maxT - minT);
  if (v.t0 < minT) v = {minT, minT + span};
  if (v.t1 > maxT) v = {maxT - span, maxT};
  return v;
}

float nanMin(float a, float b) { return std::isnan(a) ? b : (std::isnan(b) ? a : std::min(a, b)); }
float nanMax(float a, float b) { return std::isnan(a) ? b : (std::isnan(b) ? a : std::max(a, b)); }

}  // namespace

TimeView zoomView(TimeView v, double anchorT, double factor, double minT, double maxT, double minSpan) {
  const double full = maxT - minT;
  if (!(full > 0.0)) return {minT, maxT};
  const double span = std::clamp(v.span() * factor, std::min(minSpan, full), full);
  const double rel = v.span() > 0.0 ? std::clamp((anchorT - v.t0) / v.span(), 0.0, 1.0) : 0.5;
  const double t0 = anchorT - rel * span;
  return clampView({t0, t0 + span}, minT, maxT);
}

TimeView panView(TimeView v, double deltaT, double minT, double maxT) {
  return clampView({v.t0 + deltaT, v.t1 + deltaT}, minT, maxT);
}

std::vector<double> axisTicks(double lo, double hi, double unitsToPixels, double minSpacingPx,
                              const std::vector<double>& steps) {
  std::vector<double> ticks;
  if (!(hi > lo) || !(unitsToPixels > 0.0) || steps.empty()) return ticks;
  double step = steps.back();
  for (double s : steps) {
    if (s * unitsToPixels >= minSpacingPx) {
      step = s;
      break;
    }
  }
  // Integer multiples (no accumulated round-off), bounded in case of an absurd range.
  const double first = std::ceil(lo / step - 1e-9);
  for (int k = 0; k < 1000; ++k) {
    const double v = (first + k) * step;
    if (v > hi + step * 1e-9) break;
    ticks.push_back(v);
  }
  return ticks;
}

const std::vector<double>& timeTickSteps() {
  static const std::vector<double> steps{1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 18000, 36000};
  return steps;
}

const std::vector<double>& gTickSteps() {
  static const std::vector<double> steps{1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
  return steps;
}

const std::vector<double>& niceTickSteps() {
  static const std::vector<double> steps = [] {
    std::vector<double> s;
    for (double decade = 0.01; decade <= 1e4; decade *= 10.0)
      for (double m : {1.0, 2.0, 5.0}) s.push_back(m * decade);
    return s;
  }();
  return steps;
}

void decimateTrace(const MissionTrace& trace, double t0, double t1, int columns, ChartSeries secondary,
                   std::vector<ChartPoint>& out) {
  out.clear();
  const std::size_t n = trace.size();
  if (n == 0 || columns <= 0 || !(t1 > t0) || !(trace.hz > 0.0)) return;
  const double hz = trace.hz;
  auto centre = [&](std::size_t i) { return trace.startS + (static_cast<double>(i) + 0.5) / hz; };
  auto clampIndex = [&](double x) {
    return static_cast<std::size_t>(std::clamp(x, 0.0, static_cast<double>(n - 1)));
  };
  // The last bucket centred before t0 and the first centred after t1 are included.
  const std::size_t i0 = clampIndex(std::floor((t0 - trace.startS) * hz - 0.5));
  const std::size_t i1 = clampIndex(std::ceil((t1 - trace.startS) * hz - 0.5));
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  auto secondaryAt = [&](std::size_t i) {
    switch (secondary) {
      case ChartSeries::Onset: return trace.onset[i];
      case ChartSeries::Aoa: return trace.aoaDeg[i];
      case ChartSeries::None: break;
    }
    return kNaN;
  };
  auto pointAt = [&](std::size_t i, double t) {
    const float s = secondaryAt(i);
    return ChartPoint{t, trace.gMin[i], trace.gMax[i], trace.gMean[i], trace.gSustained[i], s, s, trace.highG[i] != 0};
  };

  const std::size_t count = i1 - i0 + 1;
  if (count <= static_cast<std::size_t>(columns)) {
    out.reserve(count);
    for (std::size_t i = i0; i <= i1; ++i) out.push_back(pointAt(i, centre(i)));
    return;
  }

  out.reserve(static_cast<std::size_t>(columns) + 2);
  const double columnS = (t1 - t0) / columns;
  long long column = std::numeric_limits<long long>::min();
  double meanSum = 0.0, susSum = 0.0;
  int meanCount = 0, susCount = 0;
  auto finish = [&] {
    if (meanCount > 0) out.back().gMean = static_cast<float>(meanSum / meanCount);
    if (susCount > 0) out.back().gSustained = static_cast<float>(susSum / susCount);
  };
  auto addSustained = [&](std::size_t i) {
    if (std::isfinite(trace.gSustained[i])) {
      susSum += trace.gSustained[i];
      ++susCount;
    }
  };
  for (std::size_t i = i0; i <= i1; ++i) {
    const auto c = static_cast<long long>(std::floor((centre(i) - t0) / columnS));
    if (c != column) {
      finish();
      column = c;
      out.push_back(pointAt(i, t0 + (static_cast<double>(c) + 0.5) * columnS));
      meanSum = trace.gMean[i];
      meanCount = 1;
      susSum = 0.0;
      susCount = 0;
      addSustained(i);
      continue;
    }
    ChartPoint& p = out.back();
    p.gLo = std::min(p.gLo, trace.gMin[i]);
    p.gHi = std::max(p.gHi, trace.gMax[i]);
    const float s = secondaryAt(i);
    p.secLo = nanMin(p.secLo, s);
    p.secHi = nanMax(p.secHi, s);
    p.highG = p.highG || trace.highG[i] != 0;
    meanSum += trace.gMean[i];
    ++meanCount;
    addSustained(i);
  }
  finish();
}

bool traceHasSustained(const MissionTrace& trace) {
  return std::any_of(trace.gSustained.begin(), trace.gSustained.end(), [](float v) { return std::isfinite(v); });
}

bool pointsRange(const std::vector<ChartPoint>& points, bool secondary, float& lo, float& hi) {
  lo = std::numeric_limits<float>::infinity();
  hi = -lo;
  for (const ChartPoint& p : points) {
    const float a = secondary ? p.secLo : p.gLo;
    const float b = secondary ? p.secHi : p.gHi;
    if (std::isfinite(a)) lo = std::min(lo, a);
    if (std::isfinite(b)) hi = std::max(hi, b);
  }
  return lo <= hi;
}

std::size_t bucketAt(const MissionTrace& trace, double t) {
  const double i = std::floor((t - trace.startS) * trace.hz);
  return static_cast<std::size_t>(std::clamp(i, 0.0, static_cast<double>(trace.size() - 1)));
}

}  // namespace ft
