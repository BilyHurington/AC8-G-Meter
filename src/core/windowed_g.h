#pragma once
// Highest / lowest N-second average of the live G over a mission (results screen reference:
// "best 5 s average"), from a MissionTrace.
#include <array>
#include <span>
#include <vector>

#include "core/metrics.h"

namespace ft {

// The windows shown on the results screen and stored in MissionReport::windowed.
inline constexpr std::array<double, 5> kDefaultAverageWindowsS{1.0, 5.0, 10.0, 30.0, 60.0};

// For each window length: the extremes of the average of the trace's time-weighted live G
// (TracePoint::gMean, not the sustained line) over every window of round(windowS * hz)
// buckets, with the mission time the window starts at (consistent with trace.startS). O(n)
// per window (prefix sums).
//
// tracedEndS is the mission time the trace really ends at (MissionStats::durationS): the last
// bucket is usually partial (a 29.987 s mission fills 600 buckets at 20 Hz, the last one 74 %).
// A window is valid only when it fits in [trace.startS, tracedEndS]; the window that ends the
// trace ends exactly at tracedEndS, with the partial bucket weighted by its real share and the
// rest taken from the bucket before the window. tracedEndS outside (timeAt(n - 1), endS()]
// (NaN, 0: unknown) treats the last bucket as full.
std::vector<WindowedG> computeWindowedAverages(const MissionTrace& trace, double tracedEndS,
                                               std::span<const double> windowsS = kDefaultAverageWindowsS);

}  // namespace ft
