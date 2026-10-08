// computeWindowedAverages: best / worst N-second average of the live G from a MissionTrace.
#include <doctest/doctest.h>

#include <vector>

#include "core/windowed_g.h"

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
  const auto w = computeWindowedAverages(t);
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
  const auto w = computeWindowedAverages(t, windows);
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
  const auto w = computeWindowedAverages(MissionTrace{});
  REQUIRE(w.size() == 5);
  for (const WindowedG& x : w) CHECK_FALSE(x.valid);
  CHECK(w[0].windowS == 1.0);
  CHECK(w[4].windowS == 60.0);
}
