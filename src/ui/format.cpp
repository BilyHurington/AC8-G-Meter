#include "ui/format.h"

#include <cmath>
#include <cstdio>

namespace ft {
namespace {

std::string padLeft(std::string s, int width) {
  if (static_cast<int>(s.size()) < width) s.insert(0, static_cast<std::size_t>(width) - s.size(), ' ');
  return s;
}

std::string padRight(std::string s, int width) {
  if (static_cast<int>(s.size()) < width) s.append(static_cast<std::size_t>(width) - s.size(), ' ');
  return s;
}

std::string line(const char* label, const std::string& value) {
  return padRight(label, kHudLabelWidth) + value;
}

}  // namespace

std::string formatNumber(double v, int decimals, bool showSign, int width) {
  if (!std::isfinite(v)) return padLeft("--", width);
  const double scale = std::pow(10.0, decimals);
  v = std::round(v * scale) / scale;
  if (v == 0.0) v = 0.0;  // collapses -0.0 to +0.0
  char buf[64];
  if (showSign) std::snprintf(buf, sizeof buf, "%+*.*f", width, decimals, v);
  else std::snprintf(buf, sizeof buf, "%*.*f", width, decimals, v);
  return buf;
}

std::string formatMetricLine(MetricId id, const MetricsSnapshot& s) {
  const FlightMetrics& m = s.current;
  const MissionStats& ms = s.mission;
  const std::string na = "--";
  switch (id) {
    case MetricId::CurrentG:
      return line("G", m.valid ? formatNumber(m.g, 1, true, 5) : padLeft(na, 5));
    case MetricId::MaxG:
      return line("MAX G", ms.hasData ? formatNumber(ms.maxG, 1, true, 5) : padLeft(na, 5));
    case MetricId::MinG:
      return line("MIN G", ms.hasData ? formatNumber(ms.minG, 1, true, 5) : padLeft(na, 5));
    case MetricId::OnsetRate:
      return line("ONSET", m.valid ? formatNumber(m.onsetRate, 1, true, 5) + " G/s" : padLeft(na, 5));
    case MetricId::Aoa:
      return line("AOA", m.valid && m.aoaValid ? formatNumber(m.aoaDeg, 1, false, 5) + kDegreeSign
                                               : padLeft(na, 5));
    case MetricId::SustainedHighG:
      return line("HI-G", m.valid ? formatNumber(m.highGDurationS, 1, false, 4) + " s" : padLeft(na, 4));
    case MetricId::Sideslip:
      return line("BETA", m.valid && m.aoaValid
                              ? formatNumber(m.sideslipDeg, 1, true, 5) + kDegreeSign
                              : padLeft(na, 5));
    case MetricId::Speed:
      return line("SPD", m.valid ? formatNumber(m.speed * kMsToKmh, 0, false, 4) + " km/h"
                                 : padLeft(na, 4));
    case MetricId::Count: break;
  }
  return {};
}

std::vector<std::string> formatHudLines(const OverlaySettings& overlay,
                                        const MetricsSnapshot& snapshot) {
  std::vector<std::string> lines;
  for (int i = 0; i < kMetricCount; ++i) {
    const auto id = static_cast<MetricId>(i);
    if (overlay.isVisible(id)) lines.push_back(formatMetricLine(id, snapshot));
  }
  return lines;
}

std::string formatClock(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) return "--:--";
  const long long total = static_cast<long long>(std::floor(seconds));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02lld:%02lld", total / 60, total % 60);
  return buf;
}

std::string formatSpeedKmh(double metresPerSecond) {
  return formatNumber(metresPerSecond * kMsToKmh, 0, false) + " km/h";
}

std::string gBinLabel(int bin) {
  constexpr int last = static_cast<int>(kGBinEdges.size());
  auto edge = [](int i) { return formatNumber(kGBinEdges[static_cast<std::size_t>(i)], 0, false); };
  if (bin <= 0) return "< " + edge(0);
  if (bin >= last) return ">= " + edge(last - 1);
  return edge(bin - 1) + ".." + edge(bin);
}

}  // namespace ft
