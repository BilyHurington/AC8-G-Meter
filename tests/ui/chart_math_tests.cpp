// GChart's pure helpers: ticks, zoom / pan, per-column decimation.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "ui/chart_math.h"

using namespace ft;
using doctest::Approx;

namespace {

MissionTrace flatTrace(std::size_t n, double hz = 20.0) {
  MissionTrace t;
  t.hz = hz;
  for (std::size_t i = 0; i < n; ++i) t.push({1.0f, 0.9f, 1.1f, 1.0f, 0.0f, 5.0f, 200.0f, 1000.0f, false});
  return t;
}

}  // namespace

TEST_CASE("axis ticks: readable steps, multiples of the step, inside the range") {
  // 300 s over 300 px with labels >= 60 px apart: one tick per minute.
  CHECK(axisTicks(0.0, 300.0, 1.0, 60.0, timeTickSteps()) == std::vector<double>{0, 60, 120, 180, 240, 300});
  // 10 s over 800 px: every second.
  CHECK(axisTicks(12.5, 22.5, 80.0, 60.0, timeTickSteps()).front() == 13.0);
  CHECK(axisTicks(12.5, 22.5, 80.0, 60.0, timeTickSteps()).size() == 10);
  // G: -2.3 .. 9.1 at 20 px per G with labels >= 25 px apart -> every 2 G.
  CHECK(axisTicks(-2.3, 9.1, 20.0, 25.0, gTickSteps()) == std::vector<double>{-2, 0, 2, 4, 6, 8});
  // Plenty of room: whole G, never fractions.
  CHECK(axisTicks(0.5, 3.5, 200.0, 25.0, gTickSteps()) == std::vector<double>{1, 2, 3});
  // Secondary axis (1-2-5 steps) and degenerate input.
  const std::vector<double> fine = axisTicks(-0.25, 0.3, 500.0, 30.0, niceTickSteps());  // 0.1 = 50 px
  REQUIRE(fine.size() == 6);
  CHECK(fine.front() == Approx(-0.2));
  CHECK(fine.back() == Approx(0.3));
  CHECK(axisTicks(1.0, 1.0, 10.0, 10.0, gTickSteps()).empty());
  CHECK(axisTicks(0.0, 1e9, 1e-9, 10.0, gTickSteps()).size() <= 1000);
}

TEST_CASE("zoom keeps the anchor in place and the view inside the data") {
  const TimeView full{0.0, 100.0};
  const TimeView z = zoomView(full, 25.0, 0.5, 0.0, 100.0, 2.0);
  CHECK(z.span() == Approx(50.0));
  CHECK((25.0 - z.t0) / z.span() == Approx(0.25));  // same relative position as before
  // Zooming in again near the right edge stays inside [0, 100].
  const TimeView edge = zoomView({60.0, 100.0}, 99.0, 0.5, 0.0, 100.0, 2.0);
  CHECK(edge.t1 <= 100.0);
  CHECK(edge.span() == Approx(20.0));
  // Never narrower than the minimum span, never wider than the data.
  CHECK(zoomView(full, 50.0, 1e-6, 0.0, 100.0, 2.0).span() == Approx(2.0));
  CHECK(zoomView(z, 10.0, 100.0, 0.0, 100.0, 2.0) == full);
  // Pan stops at the ends without changing the span.
  CHECK(panView({10.0, 30.0}, -50.0, 0.0, 100.0) == TimeView{0.0, 20.0});
  CHECK(panView({10.0, 30.0}, 500.0, 0.0, 100.0) == TimeView{80.0, 100.0});
  CHECK(panView({10.0, 30.0}, 5.0, 0.0, 100.0) == TimeView{15.0, 35.0});
}

TEST_CASE("decimation keeps one-bucket extremes and is bounded by the width") {
  MissionTrace t = flatTrace(100'000);  // 83 min at 20 Hz
  t.gMax[54'321] = 15.0f;
  t.gMin[77'777] = -4.0f;
  t.highG[54'321] = 1;
  t.onset[12'345] = -30.0f;
  std::vector<ChartPoint> pts;
  decimateTrace(t, t.startS, t.endS(), 800, ChartSeries::Onset, pts);
  CHECK(pts.size() <= 802);
  CHECK(pts.size() >= 790);
  float lo = 0, hi = 0;
  REQUIRE(pointsRange(pts, false, lo, hi));
  CHECK(hi == 15.0f);
  CHECK(lo == -4.0f);
  REQUIRE(pointsRange(pts, true, lo, hi));
  CHECK(lo == -30.0f);
  int highG = 0;
  for (const ChartPoint& p : pts) {
    highG += p.highG ? 1 : 0;
    CHECK(p.gMean == Approx(1.0f));  // the mean is not dragged by the extremes
  }
  CHECK(highG == 1);
  for (std::size_t i = 1; i < pts.size(); ++i) CHECK(pts[i].t > pts[i - 1].t);

  // Zoomed in (fewer buckets than pixels): one point per bucket, at the bucket centre, plus
  // one neighbour on each side.
  decimateTrace(t, 100.0, 102.5, 800, ChartSeries::None, pts);
  REQUIRE(pts.size() == 52);
  CHECK(pts[1].t == Approx(100.025));
  CHECK(pts.front().t < 100.0);
  CHECK(pts.back().t > 102.5);
  CHECK(std::isnan(pts[0].secLo));

  // AoA gaps (NaN) are kept as NaN, not as zero.
  MissionTrace a = flatTrace(10);
  a.aoaDeg[3] = std::nanf("");
  decimateTrace(a, 0.0, 0.5, 100, ChartSeries::Aoa, pts);
  CHECK(std::isnan(pts[3].secHi));
  CHECK(pts[4].secHi == 5.0f);

  decimateTrace(MissionTrace{}, 0.0, 1.0, 100, ChartSeries::None, pts);
  CHECK(pts.empty());
}

TEST_CASE("decimation: the sustained column is a NaN-aware mean") {
  MissionTrace t = flatTrace(2000);
  for (std::size_t i = 0; i < t.size(); ++i) t.gSustained[i] = i < 1000 ? 2.0f : std::nanf("");
  t.gSustained[500] = 6.0f;
  CHECK(traceHasSustained(t));
  std::vector<ChartPoint> pts;
  decimateTrace(t, t.startS, t.endS(), 100, ChartSeries::None, pts);  // 20 buckets per column
  REQUIRE(pts.size() >= 99);
  int finite = 0;
  for (const ChartPoint& p : pts) {
    if (p.t < 49.0) CHECK(std::isfinite(p.gSustained));
    if (p.t > 51.0) CHECK(std::isnan(p.gSustained));  // no value: a gap in the line, not 0
    if (std::isfinite(p.gSustained)) ++finite;
  }
  CHECK(finite == Approx(50).epsilon(0.05));
  float hi = 0.0f;
  for (const ChartPoint& p : pts) if (std::isfinite(p.gSustained)) hi = std::max(hi, p.gSustained);
  CHECK(hi == Approx(2.2f));  // (19 * 2 + 6) / 20

  for (float& v : t.gSustained) v = std::nanf("");  // an archive from before the column
  CHECK_FALSE(traceHasSustained(t));
}

TEST_CASE("bucketAt clamps to the trace") {
  MissionTrace t = flatTrace(40);
  t.startS = 10.0;
  CHECK(bucketAt(t, 10.0) == 0);
  CHECK(bucketAt(t, 10.07) == 1);
  CHECK(bucketAt(t, -5.0) == 0);
  CHECK(bucketAt(t, 1e9) == 39);
}
