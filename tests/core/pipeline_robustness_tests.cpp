// Pipeline behaviour on bad input: glitches, time errors, gaps, teleports, pauses.
#include <doctest/doctest.h>

#include <limits>
#include <numeric>
#include <random>
#include <string>

#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

TEST_CASE("a single velocity spike is rejected before it can corrupt the window") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  double maxDev = 0.0;
  for (int i = 0; i < 120; ++i) {
    RawFlightSample s = levelSample(i / 60.0);
    if (i == 60) s.velocity->z += 60.0;  // ~370 G for one frame
    p.push(s);
    if (c.last.valid) maxDev = std::max(maxDev, std::abs(c.last.g - 1.0));
  }
  const MetricsSnapshot snap = p.snapshot();
  CHECK(snap.quality.rejectedSpike == 1);
  CHECK(snap.mission.rejectedSamples == 1);
  CHECK(snap.quality.resetsTeleport == 0);
  CHECK(maxDev < 1e-6);
  CHECK(snap.mission.maxG == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("a single position spike (position-only input) is rejected") {
  TelemetryPipeline p;
  for (int i = 0; i < 120; ++i) {
    RawFlightSample s = levelSample(i / 60.0);
    s.velocity.reset();
    if (i == 60) s.position.z += 5.0;  // small enough not to look like a teleport
    p.push(s);
  }
  const MetricsSnapshot snap = p.snapshot();
  CHECK(snap.quality.rejectedSpike == 1);
  CHECK(snap.quality.resetsTeleport == 0);
  CHECK(snap.mission.maxG == Approx(1.0).epsilon(1e-6));
  CHECK(snap.mission.minG == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("a persistent velocity jump is accepted as a discontinuity") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  for (int i = 0; i < 120; ++i) {
    RawFlightSample s = levelSample(i / 60.0);
    if (i >= 60) s.velocity->x += 100.0;  // e.g. respawn at a different speed
    p.push(s);
  }
  const MetricsSnapshot snap = p.snapshot();
  CHECK(snap.quality.rejectedSpike == 2);
  CHECK(snap.quality.resetsTeleport == 1);
  CHECK(c.resets == 1);
  CHECK(snap.current.valid);
  CHECK(snap.current.speed == Approx(kSpeed + 100.0));
  CHECK(snap.mission.maxG == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("non-finite values and degenerate attitudes are rejected; small drift is repaired") {
  TelemetryPipeline p;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  int t = 0;
  for (; t < 30; ++t) p.push(levelSample(t / 60.0));

  RawFlightSample bad = levelSample(t++ / 60.0);
  bad.position.x = nan;
  p.push(bad);
  bad = levelSample(t++ / 60.0);
  bad.velocity->y = std::numeric_limits<double>::infinity();
  p.push(bad);
  bad = levelSample(t++ / 60.0);
  bad.attitude.up = {0.0, 0.0, 0.0};
  p.push(bad);
  bad = levelSample(t++ / 60.0);
  bad.attitude.right = bad.attitude.forward;
  p.push(bad);
  CHECK(p.snapshot().quality.rejectedNonFinite == 4);

  RawFlightSample drift = levelSample(t++ / 60.0);
  drift.attitude.up = Vec3{0.01, 0.0, 1.002};  // slightly off: accepted after repair
  const auto acceptedBefore = p.snapshot().quality.accepted;
  p.push(drift);
  CHECK(p.snapshot().quality.accepted == acceptedBefore + 1);
  CHECK(p.snapshot().current.g == Approx(1.0).epsilon(1e-3));
}

TEST_CASE("duplicate or backwards timestamps are rejected") {
  TelemetryPipeline p;
  for (int i = 0; i < 30; ++i) p.push(levelSample(i / 60.0));
  p.push(levelSample(29 / 60.0));
  p.push(levelSample(25 / 60.0));
  const MetricsSnapshot s = p.snapshot();
  CHECK(s.quality.rejectedTime == 2);
  CHECK(s.quality.accepted == 30);
  CHECK(s.current.valid);
}

TEST_CASE("a large backwards time jump is a clock restart, not a stream of old samples") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  fly(p, levelFlight(), 100.0, 101.0);
  fly(p, levelFlight(), 0.0, 1.0);  // level reloaded, game clock back to 0
  const MetricsSnapshot s = p.snapshot();
  CHECK(s.quality.rejectedTime == 0);
  CHECK(s.quality.resetsGap == 1);
  CHECK(c.last.valid);
  CHECK(c.last.g == Approx(1.0).epsilon(1e-6));
  CHECK(s.mission.durationS == Approx(2.0).epsilon(0.05));
}

TEST_CASE("a long frame gap resets the estimators but keeps mission stats") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  fly(p, levelTurn(60.0), 0.0, 2.0);
  const MissionStats before = p.snapshot().mission;
  fly(p, levelTurn(60.0), 2.5, 2.51);  // one sample after a 0.5 s hitch
  CHECK(p.snapshot().quality.resetsGap == 1);
  CHECK_FALSE(c.last.valid);
  CHECK(c.resets == 0);  // a hitch is not a discontinuity
  CHECK(p.snapshot().mission.maxG == Approx(before.maxG));
  fly(p, levelTurn(60.0), 2.5 + 1.0 / 60.0, 3.0);
  CHECK(c.last.valid);
  CHECK(c.last.g == Approx(2.0).epsilon(1e-3));
  CHECK(p.snapshot().mission.durationS == Approx(before.durationS + 0.5).epsilon(0.05));
}

TEST_CASE("teleport detection resets and notifies consumers") {
  for (bool positionOnly : {false, true}) {
    CAPTURE(positionOnly);
    TelemetryPipeline p;
    RecordingConsumer c;
    p.addConsumer(&c);
    FlyOptions o;
    o.positionOnly = positionOnly;
    fly(p, levelFlight(), 0.0, 1.0, o);
    // Respawn 20 km away, same speed: the jump would be ~1e6 G if differentiated.
    auto moved = [](double t) {
      return State{{kSpeed * t + 20000.0, 5000.0, 3000.0}, {kSpeed, 0.0, 0.0}, Basis{}};
    };
    fly(p, moved, 1.0, 2.0, o);
    const MetricsSnapshot s = p.snapshot();
    CHECK(s.quality.resetsTeleport == 1);
    CHECK(c.resets == 1);
    CHECK(s.quality.rejectedSpike == 0);
    CHECK(s.mission.maxG == Approx(1.0).epsilon(1e-6));
    CHECK(s.current.valid);
  }
}

TEST_CASE("resuming from pause restarts the differentiators") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  fly(p, levelFlight(), 0.0, 1.0);
  const int metricsBefore = c.metrics;
  // Paused: game time frozen, samples ignored.
  for (int i = 0; i < 30; ++i) p.push(levelSample(1.0 - 1.0 / 60.0, MissionPhase::Paused));
  CHECK(c.metrics == metricsBefore);
  CHECK(std::string(toString(p.snapshot().phase)) == "paused");
  // On resume the aircraft is somewhere else at a different speed, yet dt is tiny.
  auto resumed = [](double t) {
    return State{{kSpeed * t + 30.0, 0.0, 3000.0}, {kSpeed + 40.0, 0.0, 0.0}, Basis{}};
  };
  fly(p, resumed, 1.0, 1.01);
  CHECK_FALSE(c.last.valid);
  fly(p, resumed, 1.0 + 1.0 / 60.0, 2.0);
  const MetricsSnapshot s = p.snapshot();
  CHECK(s.mission.maxG == Approx(1.0).epsilon(1e-6));
  CHECK(s.quality.rejectedSpike == 0);
  CHECK(s.quality.resetsGap == 0);
  CHECK(s.current.speed == Approx(kSpeed + 40.0));
}

TEST_CASE("position-only input with centimetre noise: no false spikes, accurate G") {
  // Deterministic noise without std distributions (their output is implementation-defined):
  // uniform in [-a, a] with a = sqrt(3) * sigma has standard deviation sigma.
  const double sigma = 0.01;
  std::mt19937 rng(2024);
  auto noise = [&] {
    return (static_cast<double>(rng()) / 4294967295.0 * 2.0 - 1.0) * std::sqrt(3.0) * sigma;
  };
  const double nz = 6.0;
  const Trajectory turn = levelTurn(std::acos(1.0 / nz) * kRadToDeg);
  FlyOptions o;
  o.positionOnly = true;
  for (double fps : {60.0, 144.0}) {
    CAPTURE(fps);
    TelemetryPipeline p;
    RecordingConsumer c;
    p.addConsumer(&c);
    double sumSq = 0.0, maxErr = 0.0;
    int n = 0;
    for (int i = 0; i < static_cast<int>(10.0 * fps); ++i) {
      RawFlightSample smp = sampleAt(turn, i / fps, o);
      smp.position += Vec3{noise(), noise(), noise()};
      p.push(smp);
      if (smp.t < 1.0) continue;
      const double err = c.last.g - nz;
      sumSq += err * err;
      maxErr = std::max(maxErr, std::abs(err));
      ++n;
    }
    const MetricsSnapshot s = p.snapshot();
    CHECK(s.quality.rejectedSpike == 0);
    CHECK(s.quality.resetsTeleport == 0);
    // Measured: rms 0.16 / 0.10 G, max 0.50 / 0.31 G at 60 / 144 fps (median filter off).
    CHECK(std::sqrt(sumSq / n) < 0.2);
    CHECK(maxErr < 0.7);
    CHECK(s.mission.maxG < nz + 0.7);
  }
}

TEST_CASE("position-only teleport is measured against the fit prediction") {
  // 60 m sideways: well within what the old speed-based check (1500 m/s * dt + 50 m)
  // accepted, but no aircraft moves 60 m off its path in one frame.
  TelemetryPipeline p;
  FlyOptions o;
  o.positionOnly = true;
  fly(p, levelFlight(), 0.0, 1.0, o);
  auto moved = [](double t) {
    return State{{kSpeed * t, 60.0, 3000.0}, {kSpeed, 0.0, 0.0}, Basis{}};
  };
  fly(p, moved, 1.0, 2.0, o);
  const MetricsSnapshot s = p.snapshot();
  CHECK(s.quality.resetsTeleport == 1);
  CHECK(s.quality.rejectedSpike == 0);
  CHECK(s.mission.maxG == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("a respawn into a 16 G turn resets but keeps mission time and the real G") {
  // Level turn, then the trajectory switches instantly to a ~16.4 G turn and back: each
  // switch is a discontinuity (position and velocity jump), but every second flown still
  // counts and the 16 G turn in between is reported as flown.
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  const double bank = 86.5, nz = 1.0 / std::cos(bank * kDegToRad);
  double t = fly(p, levelTurn(60.0), 0.0, 2.0);
  t = fly(p, levelTurn(bank), t, 4.0);
  t = fly(p, levelTurn(60.0), t, 6.0);
  const MetricsSnapshot snap = p.snapshot();
  const MissionStats& s = snap.mission;
  CHECK(snap.quality.resetsTeleport == 2);
  CHECK(c.resets == 2);
  CHECK(s.durationS == Approx(6.0).epsilon(0.005));
  CHECK(s.maxG == Approx(nz).epsilon(0.01));
  CHECK(snap.quality.extremeG > 0);
  CHECK(c.last.valid);
  CHECK(c.last.g == Approx(2.0).epsilon(1e-3));
}
