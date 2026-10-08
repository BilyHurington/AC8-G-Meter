#pragma once
// Text formatting for the overlay and summary (pure, no ImGui), unit-testable.
#include <cstdint>
#include <string>
#include <vector>

#include "config/settings.h"
#include "core/metrics.h"

namespace ft {

inline constexpr const char* kDegreeSign = "\xC2\xB0";  // UTF-8 '°' (in B612 Mono and ImGui's default fonts)
inline constexpr double kMsToKmh = 3.6;
inline constexpr int kHudLabelWidth = 6;  // label column, values start at column 6

// printf-style fixed-point number, right-aligned to `width` characters. Rounds first so
// tiny negatives never print as "-0.0". `showSign` forces a leading '+' for positives.
std::string formatNumber(double v, int decimals, bool showSign, int width = 0);

// One overlay row: "G      +6.3", "MAX G  +7.9", "AOA    12.4°", "SPD    850 km/h".
// The label column is fixed and numbers are right-aligned so digits do not shift in a
// monospace font as values change. Unavailable data shows "--".
std::string formatMetricLine(MetricId id, const MetricsSnapshot& snapshot);
// All rows enabled in `overlay`, in MetricId order.
std::vector<std::string> formatHudLines(const OverlaySettings& overlay,
                                        const MetricsSnapshot& snapshot);

std::string formatClock(double seconds);   // "mm:ss" (minutes may exceed 99), "--:--" if invalid
std::string formatSpeedKmh(double metresPerSecond);  // "850 km/h"
std::string formatLocalDateTime(std::int64_t unixS);  // "2026-10-08 14:03" in the local time zone
std::string gBinLabel(int bin);            // "< -1", "-1..0", ..., ">= 9" (matches kGBinEdges)

}  // namespace ft
