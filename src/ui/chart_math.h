#pragma once
// Pure math behind GChart (no ImGui), unit-testable: visible time window, zoom / pan, axis
// ticks and per-pixel-column decimation of a MissionTrace.
#include <cstdint>
#include <vector>

#include "core/metrics.h"

namespace ft {

// Visible time window of the chart, in mission seconds.
struct TimeView {
  double t0 = 0.0, t1 = 1.0;
  double span() const { return t1 - t0; }
  bool operator==(const TimeView&) const = default;
};

// Zooms by `factor` (< 1 zooms in) keeping `anchorT` at the same screen position, then
// keeps the window inside [minT, maxT] with a span of at least minSpan.
TimeView zoomView(TimeView v, double anchorT, double factor, double minT, double maxT, double minSpan);
// Shifts the window by deltaT, stopping at the data limits (the span never changes).
TimeView panView(TimeView v, double deltaT, double minT, double maxT);
// A view that shows the span [s0, s1]: v itself when the span is already inside it; else v
// panned to centre the span when it fits (zoom kept); else the span plus 10 % either side.
// Always within [minT, maxT].
TimeView revealSpan(TimeView v, double s0, double s1, double minT, double maxT);

// Tick positions in [lo, hi] at a step from `steps` (ascending) that is at least
// minSpacing / unitsToPixels apart: the smallest step whose spacing on screen is readable.
// unitsToPixels = pixels per axis unit. Ticks are multiples of the step.
std::vector<double> axisTicks(double lo, double hi, double unitsToPixels, double minSpacingPx,
                              const std::vector<double>& steps);
// Steps for a mm:ss axis (1 s ... 1 h) and for G (whole G: 1, 2, 5, 10, ...).
const std::vector<double>& timeTickSteps();
const std::vector<double>& gTickSteps();
// 1-2-5 steps over many decades (secondary axis: onset G/s or AoA degrees).
const std::vector<double>& niceTickSteps();

enum class ChartSeries : std::uint8_t { None, Onset, Aoa };

// One drawable x position: a single bucket (zoomed in) or the aggregate of every bucket whose
// centre falls in one pixel column (zoomed out), so drawing is O(width) at any trace length.
struct ChartPoint {
  double t = 0.0;               // mission time (bucket centre, or column centre)
  float gLo = 0.0f, gHi = 0.0f; // min of gMin / max of gMax (live G): peaks always survive
  float gMean = 0.0f;           // mean of gMean (live G)
  float gSustained = 0.0f;      // mean of the finite gSustained, NaN when none
  float secLo = 0.0f, secHi = 0.0f;  // secondary series range (NaN when none / invalid)
  bool highG = false;           // some bucket was in a high-G episode
};

// Points covering [t0, t1] (plus one neighbour on each side, so lines run to the edges),
// at most ~`columns` of them.
void decimateTrace(const MissionTrace& trace, double t0, double t1, int columns, ChartSeries secondary,
                   std::vector<ChartPoint>& out);

// True when some bucket has a sustained G value (archives from before it have none).
bool traceHasSustained(const MissionTrace& trace);

// Value range of the points (G or secondary), NaN-safe; false when there is no finite value.
bool pointsRange(const std::vector<ChartPoint>& points, bool secondary, float& lo, float& hi);

// Index of the bucket at mission time t, clamped to the trace (trace must not be empty).
std::size_t bucketAt(const MissionTrace& trace, double t);

}  // namespace ft
