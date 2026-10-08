// computeWindowedAverages: best / worst N-second average of the live G from a MissionTrace.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "core/windowed_g.h"
#include "flight_helpers.h"

using namespace ft;
using doctest::Approx;

namespace {

MissionTrace traceOf(double seconds, double hz, double startS, double (*g)(double)) {
  MissionTrace t;
  t.hz = hz;
  t.startS = startS;
  const auto n = static_cast<std::size_t>(seconds * hz + 0.5);
  for (std::size_t i = 0; i < n; ++i) {
    TracePoint p;
    p.gMean = static_cast<float>(g(t.timeAt(i)));
    p.gMin = p.gMax = p.gMean;
    p.gSustained = 99.0f;  // must not be used
    t.push(p);
  }
  return t;
}

}  // namespace

TEST_CASE("windowed averages: constant G gives the same value for every valid window") {
  const MissionTrace t = traceOf(90.0, 20.0, 0.0, [](double) { return 3.0; });
  const auto w = computeWindowedAverages(t, t.endS());
  REQUIRE(w.size() == kDefaultAverageWindowsS.size());
  for (const WindowedG& x : w) {
    CAPTURE(x.windowS);
    CHECK(x.valid);
    CHECK(x.maxAvgG == Approx(3.0));
    CHECK(x.minAvgG == Approx(3.0));
    CHECK(x.maxAvgStartS == 0.0);  // ties: the earliest window
  }
}

TEST_CASE("windowed averages: a 5 s block of 7 G in 1 G flight, uneven start time") {
  // Trace from 2.5 s to 32.5 s; 7 G over [12.5, 17.5).
  const MissionTrace t = traceOf(30.0, 20.0, 2.5, [](double s) { return s >= 12.5 - 1e-9 && s < 17.5 - 1e-9 ? 7.0 : 1.0; });
  const double windows[] = {1.0, 5.0, 10.0, 30.0, 31.0};
  const auto w = computeWindowedAverages(t, t.endS(), windows);
  REQUIRE(w.size() == 5);
  CHECK(w[0].maxAvgG == Approx(7.0));
  CHECK(w[0].maxAvgStartS == Approx(12.5));
  CHECK(w[1].maxAvgG == Approx(7.0));
  CHECK(w[1].maxAvgStartS == Approx(12.5));
  CHECK(w[2].maxAvgG == Approx(4.0));         // half the window at 7 G
  CHECK(w[2].maxAvgStartS == Approx(7.5));    // earliest window containing the block
  CHECK(w[2].minAvgG == Approx(1.0));
  CHECK(w[2].minAvgStartS == Approx(2.5));
  CHECK(w[3].valid);                          // exactly the trace
  CHECK(w[3].maxAvgG == Approx(2.0));
  CHECK_FALSE(w[4].valid);                    // longer than the mission
  for (const WindowedG& x : w) CHECK(x.windowS > 0.0);
}

TEST_CASE("windowed averages: empty trace and default windows") {
  const auto w = computeWindowedAverages(MissionTrace{}, 0.0);
  REQUIRE(w.size() == 5);
  for (const WindowedG& x : w) CHECK_FALSE(x.valid);
  CHECK(w[0].windowS == 1.0);
  CHECK(w[4].windowS == 60.0);
}

TEST_CASE("windowed averages: the partial last bucket counts by its real share") {
  // A 29.987 s mission at 20 Hz: 600 buckets, the last one only 74 % traced.
  const MissionTrace t = traceOf(30.0, 20.0, 0.0, [](double s) { return s >= 29.0 ? 9.0 : 1.0; });
  REQUIRE(t.size() == 600);
  const double windows[] = {1.0, 29.0, 29.987, 30.0};
  const auto w = computeWindowedAverages(t, 29.987, windows);
  REQUIRE(w.size() == 4);
  CHECK_FALSE(w[3].valid);  // 30 s does not fit in 29.987 s
  // The 29.987 s window rounds to 600 buckets (30 s): longer than the trace, not valid.
  CHECK_FALSE(w[2].valid);
  REQUIRE(w[1].valid);
  REQUIRE(w[0].valid);
  // The best 1 s ends at the traced end: 0.987 s of 9 G (the last bucket at 74 %) and
  // 0.013 s of 1 G before 29 s. (Whole buckets ending at 29.95 s would give 8.6 G.)
  CHECK(w[0].maxAvgG == Approx(0.013 * 1.0 + 0.987 * 9.0));
  CHECK(w[0].maxAvgStartS == Approx(28.987));
  CHECK(w[0].minAvgG == Approx(1.0));
  // 29 s windows: the one ending at 29.987 s holds 0.987 s of 9 G and 28.013 s of 1 G.
  CHECK(w[1].maxAvgG == Approx((0.987 * 9.0 + 28.013 * 1.0) / 29.0));
  CHECK(w[1].maxAvgStartS == Approx(0.987));
  CHECK(w[1].minAvgG == Approx(1.0));
  CHECK(w[1].minAvgStartS == 0.0);

  // An unknown end (0, NaN) or one past the trace counts every bucket as full.
  for (double end : {0.0, std::nan(""), 31.0}) {
    const auto full = computeWindowedAverages(t, end, windows);
    CHECK(full[3].valid);
    CHECK(full[0].maxAvgStartS == Approx(29.0));
  }
}

TEST_CASE("windowed averages: a partial bucket that is a spike") {
  // 10.02 s at 20 Hz: 201 buckets, the last one 40 % traced at 20 G. A whole-bucket average
  // would weigh it fully.
  const MissionTrace t = traceOf(10.05, 20.0, 0.0, [](double s) { return s >= 10.0 - 1e-9 ? 20.0 : 1.0; });
  REQUIRE(t.size() == 201);
  const double windows[] = {1.0, 10.0};
  const auto w = computeWindowedAverages(t, 10.02, windows);
  REQUIRE(w[0].valid);
  CHECK(w[0].maxAvgG == Approx((0.02 * 20.0 + 0.98 * 1.0) / 1.0));
  CHECK(w[0].maxAvgStartS == Approx(9.02));
  REQUIRE(w[1].valid);
  CHECK(w[1].maxAvgG == Approx((0.02 * 20.0 + 9.98) / 10.0));
}

TEST_CASE("windowed averages: a 29.987 s mission has no 30 s average") {
  TelemetryPipeline p;
  const test::Trajectory level = test::levelFlight();
  test::fly(p, level, 0.0, 29.98);  // 60 Hz up to 29.9833 s
  RawFlightSample last = test::sampleAt(level, 29.987);
  p.push(last);
  last = test::sampleAt(level, 29.987 + 1.0 / 60.0);
  last.phase = MissionPhase::Ended;
  p.push(last);
  const auto report = p.snapshot().lastReport;
  REQUIRE(report);
  CHECK(report->stats.durationS == Approx(29.987));
  CHECK(report->trace.size() == 600);  // the last bucket is partial
  REQUIRE(report->windowed.size() == kDefaultAverageWindowsS.size());
  CHECK(report->windowed[2].valid);        // 10 s
  CHECK_FALSE(report->windowed[3].valid);  // 30 s: the mission is shorter
  CHECK_FALSE(report->windowed[4].valid);
}
