// Extreme G and high sample rates: there is no G cap, so anything the data consistently
// shows is reported as measured, while glitches are still caught by the spike gate.
#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

// Deterministic gaussian noise (Box-Muller on mt19937, whose output is fully specified;
// std::normal_distribution's is implementation-defined).
struct Gaussian {
  std::mt19937 rng;
  explicit Gaussian(std::uint32_t seed) : rng(seed) {}
  double operator()(double sigma) {
    const double u1 = (static_cast<double>(rng()) + 1.0) / 4294967297.0;
    const double u2 = static_cast<double>(rng()) / 4294967296.0;
    return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
  }
  Vec3 vec(double sigma) { return {(*this)(sigma), (*this)(sigma), (*this)(sigma)}; }
};

// Flies `traj` against the truth `nz(t)` at a fixed rate and records the worst error after
// `settleS` (and before `untilS`, if given).
struct Run {
  MetricsSnapshot snap;
  double maxErr = 0.0, sumSq = 0.0;
  int n = 0;
  bool validThroughout = true;
  double rms() const { return n > 0 ? std::sqrt(sumSq / n) : 0.0; }
};

template <typename NzFn, typename Mutate>
Run flyAgainstTruth(const Trajectory& traj, NzFn nz, double durationS, double fps,
                    bool positionOnly, double settleS, Mutate mutate) {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  FlyOptions o;
  o.positionOnly = positionOnly;
  Run r;
  const int count = static_cast<int>(durationS * fps);
  for (int i = 0; i < count; ++i) {
    RawFlightSample s = sampleAt(traj, i / fps, o);
    mutate(i, s);
    p.push(s);
    if (s.t < 0.2) continue;
    r.validThroughout = r.validThroughout && c.last.valid;
    if (s.t < settleS) continue;
    const double err = c.last.g - nz(s.t);
    r.maxErr = std::max(r.maxErr, std::abs(err));
    r.sumSq += err * err;
    ++r.n;
  }
  r.snap = p.snapshot();
  return r;
}

void checkClean(const Run& r, double truthMaxG, double durationS) {
  const MetricsSnapshot& s = r.snap;
  CHECK(r.validThroughout);
  CHECK(s.quality.rejectedSpike == 0);
  CHECK(s.quality.resetsTeleport == 0);
  CHECK(s.quality.resetsGap == 0);
  CHECK(s.mission.rejectedSamples == 0);
  CHECK(s.quality.extremeG > 0);
  CHECK(s.mission.extremeGSamples > 0);
  CHECK(s.mission.maxG == Approx(truthMaxG).epsilon(0.02));
  CHECK(s.mission.durationS == Approx(durationS).epsilon(0.005));
}

const auto noMutation = [](int, RawFlightSample&) {};

}  // namespace

TEST_CASE("sustained 16 G and 20 G turns are reported as flown") {
  for (double nz : {16.0, 20.0}) {
    for (bool positionOnly : {false, true}) {
      CAPTURE(nz);
      CAPTURE(positionOnly);
      const Trajectory turn = levelTurn(std::acos(1.0 / nz) * kRadToDeg);
      const Run r = flyAgainstTruth(turn, [nz](double) { return nz; }, 5.0, 60.0, positionOnly, 0.5,
                                    noMutation);
      CHECK(r.maxErr < 0.01 * nz);
      checkClean(r, nz, 5.0);
      CHECK(r.snap.current.g == Approx(nz).epsilon(0.01));
      CHECK(r.snap.mission.highGEpisodes == 1);
      CHECK(r.snap.mission.timeInGBinS[kGBinCount - 1] > 4.0);  // the [15, inf) bin
    }
  }
}

TEST_CASE("gradual onset to 20 G and back is tracked without rejections") {
  // 1 -> 20 G at 10 G/s, 2 s at 20 G, back to 1 G at 10 G/s.
  auto nz = [](double t) {
    if (t < 1.0) return 1.0;
    if (t < 2.9) return 1.0 + 10.0 * (t - 1.0);
    if (t < 4.9) return 20.0;
    if (t < 6.8) return 20.0 - 10.0 * (t - 4.9);
    return 1.0;
  };
  for (bool positionOnly : {false, true}) {
    CAPTURE(positionOnly);
    GProfile prof(nz);
    const Run r = flyAgainstTruth(prof.trajectory(), nz, 8.0, 60.0, positionOnly, 0.5, noMutation);
    // Zero-lag error on the 10 G/s ramps is the filter delay (~0.1 - 0.15 s => 1 - 1.5 G).
    CHECK(r.maxErr < 1.6);
    checkClean(r, 20.0, 8.0);
  }
}

TEST_CASE("an instantaneous step from 2 G to 18 G is accepted, not treated as a glitch") {
  auto nz = [](double t) { return t < 1.5 ? 2.0 : 18.0; };
  for (bool positionOnly : {false, true}) {
    CAPTURE(positionOnly);
    GProfile prof(nz);
    TelemetryPipeline p;
    RecordingConsumer c;
    p.addConsumer(&c);
    FlyOptions o;
    o.positionOnly = positionOnly;
    const Trajectory traj = prof.trajectory();
    double maxErrSettled = 0.0;
    bool validThroughout = true;
    for (int i = 0; i < 4 * 60; ++i) {
      const double t = i / 60.0;
      p.push(sampleAt(traj, t, o));
      if (t > 0.2) validThroughout = validThroughout && c.last.valid;
      if (t > 1.8) maxErrSettled = std::max(maxErrSettled, std::abs(c.last.g - 18.0));
    }
    const MetricsSnapshot s = p.snapshot();
    CHECK(validThroughout);
    CHECK(maxErrSettled < 0.36);  // 2 % once the filters have caught up
    CHECK(s.quality.rejectedSpike == 0);
    CHECK(s.quality.resetsTeleport == 0);
    CHECK(s.mission.rejectedSamples == 0);
    CHECK(s.quality.extremeG > 0);
    CHECK(s.mission.maxG == Approx(18.0).epsilon(0.02));
    CHECK(s.mission.durationS == Approx(4.0).epsilon(0.005));
  }
}

TEST_CASE("a +300 m/s velocity glitch in a 16 G turn is still rejected") {
  const double nz = 16.0;
  const Trajectory turn = levelTurn(std::acos(1.0 / nz) * kRadToDeg);
  const Run r = flyAgainstTruth(turn, [nz](double) { return nz; }, 4.0, 60.0, false, 0.5,
                                [](int i, RawFlightSample& s) {
                                  if (i == 120) s.velocity->z += 300.0;
                                });
  CHECK(r.snap.quality.rejectedSpike == 1);
  CHECK(r.snap.mission.rejectedSamples == 1);
  CHECK(r.snap.quality.resetsTeleport == 0);
  CHECK(r.maxErr < 0.01 * nz);
  CHECK(r.snap.mission.maxG == Approx(nz).epsilon(0.01));
}

TEST_CASE("0.2 m/s velocity noise at 144 - 1000 Hz: no false spikes, accurate G") {
  // Per-step velocity noise implies large accelerations at high rates (sigma * sqrt(2) * fps
  // / g0 per axis: ~4 G at 144 Hz, ~29 G at 1000 Hz). The gate must stay above it.
  const double nz = 9.0, sigma = 0.2;
  const Trajectory turn = levelTurn(std::acos(1.0 / nz) * kRadToDeg);
  for (double fps : {144.0, 240.0, 480.0, 1000.0}) {
    CAPTURE(fps);
    Gaussian noise(static_cast<std::uint32_t>(fps));
    const Run r = flyAgainstTruth(turn, [nz](double) { return nz; }, 6.0, fps, false, 0.5,
                                  [&](int, RawFlightSample& s) { *s.velocity += noise.vec(sigma); });
    CHECK(r.validThroughout);
    CHECK(r.snap.quality.rejectedSpike == 0);
    CHECK(r.snap.quality.resetsTeleport == 0);
    CHECK(r.rms() < 0.15);
    CHECK(r.maxErr < 0.6);
    CHECK(r.snap.mission.maxG == Approx(nz).epsilon(0.06));
  }
}

TEST_CASE("a +300 m/s velocity glitch is rejected at 1000 Hz with noisy input") {
  const double nz = 9.0, fps = 1000.0;
  const Trajectory turn = levelTurn(std::acos(1.0 / nz) * kRadToDeg);
  Gaussian noise(99);
  const Run r = flyAgainstTruth(turn, [nz](double) { return nz; }, 4.0, fps, false, 0.5,
                                [&](int i, RawFlightSample& s) {
                                  *s.velocity += noise.vec(0.2);
                                  if (i == 2000) s.velocity->x += 300.0;
                                });
  CHECK(r.snap.quality.rejectedSpike == 1);
  CHECK(r.snap.quality.resetsTeleport == 0);
  CHECK(r.maxErr < 0.6);
  CHECK(r.snap.mission.maxG < nz + 0.6);
}
