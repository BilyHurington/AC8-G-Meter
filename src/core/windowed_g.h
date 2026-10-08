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
// (TracePoint::gMean, not the sustained line) over every run of round(windowS * hz) whole
// buckets, with the mission time the run starts at (consistent with trace.startS). O(n) per
// window (prefix sums). A window longer than the trace is not valid.
std::vector<WindowedG> computeWindowedAverages(const MissionTrace& trace,
                                               std::span<const double> windowsS = kDefaultAverageWindowsS);

}  // namespace ft
