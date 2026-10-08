// MissionTracker's fixed-rate MissionTrace: bucketing, time weighting, extremes, bounds.
#include <doctest/doctest.h>

#include <cmath>

#include "core/mission_tracker.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

FlightMetrics metrics(double g) {
  FlightMetrics m;
  m.valid = true;
  m.g = g;
  m.speed = 250.0;
  m.altitude = 1200.0;
  return m;
}

// One valid sample dtS after the previous one.
void step(MissionTracker& t, double dtS, const FlightMetrics& m) {
  t.addTime(dtS);
  t.add(m, dtS);
}

}  // namespace

TEST_CASE("trace buckets keep a one-frame peak that the mean smooths away") {
  MissionTracker t;
  t.start(20.0);
  for (int i = 1; i <= 120; ++i) step(t, 1.0 / 60.0, metrics(i == 61 ? 9.0 : 1.0));  // 2 s at 60 fps
  const MissionReport r = t.finish(false);
  REQUIRE(r.trace.size() == 40);
  std::size_t peak = 0;
  for (std::size_t i = 0; i < r.trace.size(); ++i)
    if (r.trace.gMax[i] > r.trace.gMax[peak]) peak = i;
  CHECK(r.trace.gMax[peak] == Approx(9.0f));
  CHECK(r.trace.gMin[peak] == Approx(1.0f));
  CHECK(r.trace.gMean[peak] < 4.0f);  // a third of the bucket at most
  CHECK(r.trace.gMean[peak] > 1.5f);
  CHECK(r.trace.timeAt(peak) == Approx(1.0).epsilon(0.03));  // the peak sample ends at 61/60 s
  for (std::size_t i = 0; i < r.trace.size(); ++i) {
    CHECK(r.trace.gMin[i] <= r.trace.gMean[i]);
    CHECK(r.trace.gMean[i] <= r.trace.gMax[i]);
  }
}

TEST_CASE("trace means are weighted by time with uneven frame times") {
  MissionTracker t;
  t.start(20.0);  // buckets [0, 0.05) [0.05, 0.10)
  step(t, 0.03, metrics(2.0));
  step(t, 0.04, metrics(6.0));  // 0.02 s in each bucket
  step(t, 0.03, metrics(3.0));
  const MissionReport r = t.finish(false);
  REQUIRE(r.trace.size() == 2);
  CHECK(r.trace.gMean[0] == Approx((0.03 * 2.0 + 0.02 * 6.0) / 0.05).epsilon(1e-4));
  CHECK(r.trace.gMean[1] == Approx((0.02 * 6.0 + 0.03 * 3.0) / 0.05).epsilon(1e-4));
  CHECK(r.trace.gMin[0] == 2.0f);
  CHECK(r.trace.gMax[0] == 6.0f);
  CHECK(r.trace.gMin[1] == 3.0f);
  CHECK(r.trace.gMax[1] == 6.0f);
  CHECK(r.trace.speed[1] == 250.0f);
  CHECK(r.trace.altitude[1] == 1200.0f);
}

TEST_CASE("trace columns: onset, AoA validity, high-G flag, warm-up fill") {
  MissionTracker t;
  t.start(10.0);  // 0.1 s buckets
  FlightMetrics a = metrics(5.0);
  a.onsetRate = 3.0;
  step(t, 0.05, a);
  FlightMetrics b = metrics(7.0);
  b.onsetRate = -4.0;  // larger magnitude wins, sign kept
  b.aoaValid = true;
  b.aoaDeg = 14.0;
  step(t, 0.05, b);
  // The high-G flag comes from the sustained path (episodes of the sustained G).
  SustainedSample sus;
  sus.g = 7.0;
  sus.atS = 0.1;
  sus.dtS = 0.05;
  sus.inHighG = true;
  sus.highGEntered = true;
  t.addSustained(sus);
  // Estimator warm-up after a reset: time passes without valid metrics, then one sample
  // stands for all of it.
  t.addTime(0.25);
  step(t, 0.05, metrics(2.0));
  const MissionReport r = t.finish(false);
  REQUIRE(r.trace.size() == 4);
  CHECK(r.trace.onset[0] == -4.0f);
  CHECK(r.trace.aoaDeg[0] == 14.0f);
  CHECK(r.trace.highG[0] == 1);
  for (std::size_t i = 1; i < 4; ++i) {
    CHECK(std::isnan(r.trace.aoaDeg[i]));
    CHECK(r.trace.highG[i] == 0);
    CHECK(r.trace.gMean[i] == Approx(2.0f));
  }
  CHECK(r.trace.endS() == Approx(r.stats.durationS));
}

TEST_CASE("trace restarts at the mission time of a G mode change") {
  MissionTracker t;
  t.start(20.0);
  for (int i = 0; i < 37; ++i) step(t, 1.0 / 30.0, metrics(-1.0));
  const double changeAt = t.stats().durationS;
  t.resetGStats();
  for (int i = 0; i < 30; ++i) step(t, 1.0 / 30.0, metrics(1.0));
  const MissionReport r = t.finish(false);
  CHECK(r.trace.startS == Approx(changeAt));
  CHECK(r.trace.endS() == Approx(r.stats.durationS).epsilon(1e-3));
  CHECK(r.trace.size() == 20);
  for (std::size_t i = 0; i < r.trace.size(); ++i) CHECK(r.trace.gMin[i] == 1.0f);

  // The next mission starts from scratch.
  t.start(20.0);
  step(t, 0.05, metrics(3.0));
  const MissionReport next = t.finish(false);
  CHECK(next.trace.startS == 0.0);
  CHECK(next.trace.size() == 1);
}

TEST_CASE("trace memory is bounded") {
  MissionTracker t;
  t.start(10000.0);
  step(t, 0.01, metrics(1.0));
  step(t, 250.0, metrics(2.0));  // would be 2.5 M buckets
  step(t, 0.01, metrics(3.0));
  const MissionReport r = t.finish(false);
  CHECK(r.trace.size() == MissionTracker::kMaxTracePoints);
  CHECK(r.trace.highG.size() == MissionTracker::kMaxTracePoints);
  CHECK(r.stats.maxG == 3.0);  // statistics continue past the cap
}

TEST_CASE("mission label and thresholds reach the report") {
  PipelineConfig cfg;
  cfg.highGEnter = 7.0;
  cfg.highGExit = 6.0;
  TelemetryPipeline p(cfg);
  p.setMissionLabel("Simulator: test");
  double t = fly(p, levelFlight(), 0.0, 1.0);
  FlyOptions ended;
  ended.phase = MissionPhase::Ended;
  fly(p, levelFlight(), t, 1.2, ended);
  REQUIRE(p.snapshot().lastReport);
  const MissionReport& r = *p.snapshot().lastReport;
  CHECK(r.label == "Simulator: test");
  CHECK(r.highGEnter == 7.0);
  CHECK(r.highGExit == 6.0);
  CHECK(r.trace.size() > 15);
}
