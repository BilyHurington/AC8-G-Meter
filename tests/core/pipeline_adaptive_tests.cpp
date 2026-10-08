// Adaptive filtering: the acceleration window, EMA tau and onset window follow the
// measured sample rate and velocity noise (PipelineConfig::adaptiveFiltering).
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

// 1 G, then a 10 G/s ramp to 8 G starting at t = 2 s, then 8 G held.
double rampNz(double t) { return std::clamp(1.0 + 10.0 * (t - 2.0), 1.0, 8.0); }

struct RampResult {
  double lagS = 0.0;  // mean delay of g behind the truth on the steady part of the ramp
  FilterState filter;
  std::vector<double> g;
};

// velocityNoise: gaussian, per axis (m/s).
RampResult flyRamp(const PipelineConfig& cfg, double fps, double velocityNoise = 0.0) {
  TelemetryPipeline p(cfg);
  GProfile prof(rampNz);
  Gaussian noise(17);
  RampResult r;
  double lagSum = 0.0;
  int n = 0;
  for (int i = 0; i < static_cast<int>(3.0 * fps); ++i) {
    const double t = i / fps;
    RawFlightSample s = sampleAt(prof.trajectory(), t);
    *s.velocity += noise.vec(velocityNoise);
    p.push(s);
    const FlightMetrics& m = p.snapshot().current;
    r.g.push_back(m.g);
    // Past the filters' start-up transient, before the top of the ramp (t = 2.7 s).
    if (t >= 2.4 && t <= 2.6) {
      lagSum += (rampNz(t) - m.g) / 10.0;
      ++n;
    }
  }
  r.lagS = lagSum / n;
  r.filter = p.snapshot().filter;
  return r;
}

}  // namespace

TEST_CASE("adaptive: clean 240 Hz data gets a short window and tracks a 10 G/s ramp closely") {
  const RampResult r = flyRamp(PipelineConfig{}, 240.0);
  CHECK(r.filter.adaptive);
  CHECK(r.filter.sampleRateHz == Approx(240.0).epsilon(0.01));
  CHECK(r.filter.velocityNoiseMS < 1e-3);
  CHECK(r.filter.accelWindowS == Approx(2.5 / 240.0).epsilon(0.01));  // floor: 2.5 samples
  CHECK(r.filter.smoothingTauS == Approx(0.4 * r.filter.accelWindowS));
  CHECK(r.filter.onsetWindowS == Approx(3.0 * r.filter.accelWindowS));
  CHECK(r.lagS > 0.0);
  CHECK(r.lagS < 0.02);
}

TEST_CASE("adaptive: noisy 60 Hz data gets a long window") {
  TelemetryPipeline p;
  Gaussian noise(42);
  const Trajectory turn = levelTurn(75.0);
  for (int i = 0; i < 6 * 60; ++i) {
    RawFlightSample s = sampleAt(turn, i / 60.0);
    *s.velocity += noise.vec(0.2);
    p.push(s);
  }
  const MetricsSnapshot snap = p.snapshot();
  CHECK(snap.filter.adaptive);
  CHECK(snap.filter.sampleRateHz == Approx(60.0).epsilon(0.01));
  CHECK(snap.filter.velocityNoiseMS == Approx(0.2).epsilon(0.2));
  // Converges to the fixed configuration (never longer: see the next test).
  CHECK(snap.filter.accelWindowS == Approx(PipelineConfig{}.accelWindowS));
  CHECK(snap.filter.smoothingTauS == Approx(PipelineConfig{}.smoothingTauS));
  CHECK(snap.quality.rejectedSpike == 0);
  CHECK(snap.current.g == Approx(1.0 / std::cos(75.0 * kDegToRad)).epsilon(0.05));
}

TEST_CASE("adaptive off: the fixed config windows are used as-is") {
  PipelineConfig cfg;
  cfg.adaptiveFiltering = false;
  const RampResult r = flyRamp(cfg, 240.0);
  CHECK_FALSE(r.filter.adaptive);
  CHECK(r.filter.sampleRateHz == Approx(240.0).epsilon(0.01));  // still measured
  CHECK(r.filter.accelWindowS == cfg.accelWindowS);
  CHECK(r.filter.smoothingTauS == cfg.smoothingTauS);
  CHECK(r.filter.onsetWindowS == cfg.onsetWindowS);
  // Delay of the fixed filters on a ramp: LS window centre (40 ms) + EMA tau (50 ms).
  CHECK(r.lagS == Approx(0.09).epsilon(0.1));

  // The adaptive tuning has no effect while it is off: identical output.
  PipelineConfig other = cfg;
  other.adaptiveTargetG = 0.5;
  other.adaptiveTauRatio = 2.0;
  other.adaptiveMaxWindowS = 0.3;
  CHECK(flyRamp(other, 240.0).g == r.g);
}

TEST_CASE("adaptive filtering is never slower than the fixed filter") {
  // The window is capped at accelWindowS (unless 2.5 samples are longer: below 31 Hz, where
  // both keep the same 3 samples) and tau at smoothingTauS, whatever the rate and noise.
  const PipelineConfig cfg;
  for (double fps : {30.0, 60.0, 240.0, 1000.0}) {
    for (double sigma : {0.0, 0.05, 0.2, 0.5}) {
      CAPTURE(fps);
      CAPTURE(sigma);
      TelemetryPipeline p;
      Gaussian noise(3);
      const Trajectory turn = levelTurn(60.0);
      for (int i = 0; i < static_cast<int>(3.0 * fps); ++i) {
        RawFlightSample s = sampleAt(turn, i / fps);
        *s.velocity += noise.vec(sigma);
        p.push(s);
      }
      const FilterState f = p.snapshot().filter;
      CHECK(f.adaptive);
      CHECK(f.accelWindowS <= std::max(cfg.accelWindowS, 2.5 / fps) + 1e-12);
      CHECK(f.smoothingTauS <= cfg.smoothingTauS + 1e-12);
      CHECK(f.onsetWindowS <= cfg.onsetWindowS + 1e-12);
    }
  }

  // Lag on a 10 G/s ramp at 240 Hz. Moderate noise: a shorter window and tau than the fixed
  // filter (measured 75 vs 88 ms). Heavy noise: the same windows as the fixed filter, so the
  // very same output (it was ~10 ms slower before tau was capped).
  PipelineConfig fixed;
  fixed.adaptiveFiltering = false;
  const RampResult moderate = flyRamp(cfg, 240.0, 0.05);
  CHECK(moderate.filter.adaptive);
  CHECK(moderate.lagS < flyRamp(fixed, 240.0, 0.05).lagS - 0.01);
  const RampResult heavy = flyRamp(cfg, 240.0, 0.3);
  CHECK(heavy.filter.accelWindowS == Approx(fixed.accelWindowS));
  CHECK(heavy.filter.smoothingTauS == Approx(fixed.smoothingTauS));
  CHECK(heavy.lagS <= flyRamp(fixed, 240.0, 0.3).lagS + 0.001);
}

TEST_CASE("adaptive: falls back to the fixed windows until the noise estimate is ready") {
  TelemetryPipeline p;
  CHECK(p.snapshot().filter.input == FilterState::Input::None);
  for (int i = 0; i < 5; ++i) p.push(levelSample(i / 60.0));
  FilterState f = p.snapshot().filter;
  CHECK(f.input == FilterState::Input::Velocity);
  CHECK_FALSE(f.noiseReady);
  CHECK_FALSE(f.adaptive);
  CHECK(f.accelWindowS == PipelineConfig{}.accelWindowS);
  CHECK(f.smoothingTauS == PipelineConfig{}.smoothingTauS);
  for (int i = 5; i < 30; ++i) p.push(levelSample(i / 60.0));
  f = p.snapshot().filter;
  CHECK(f.noiseReady);
  CHECK(f.adaptive);
}

TEST_CASE("frame-time jitter does not make the output flicker invalid") {
  // Frame time uniform in [1/144, 1/30] s. The window keeps one sample older than its length
  // while the rest span too little, so it never drops below what the estimate needs (before:
  // ~5 % of the frames invalid, and the high-G time 3.3 % short of the fixed filter's).
  const double nz = 8.0;
  const Trajectory turn = levelTurn(std::acos(1.0 / nz) * kRadToDeg);
  double highGS[2] = {};
  for (int adaptive = 0; adaptive < 2; ++adaptive) {
    CAPTURE(adaptive);
    PipelineConfig cfg;
    cfg.adaptiveFiltering = adaptive == 1;
    TelemetryPipeline p(cfg);
    RecordingConsumer c;
    p.addConsumer(&c);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> frame(1.0 / 144.0, 1.0 / 30.0);
    int invalid = 0;
    for (double t = 0.0; t < 20.0; t += frame(rng)) {
      p.push(sampleAt(turn, t));
      if (t > 0.5 && !c.last.valid) ++invalid;
    }
    CHECK(invalid <= 2);  // measured 0
    CHECK(p.snapshot().mission.highGEpisodes == 1);
    highGS[adaptive] = p.snapshot().mission.totalHighGS;
  }
  CHECK(highGS[1] == Approx(highGS[0]).epsilon(0.01));
  // Sustained statistics lag by up to W / 8 + 2 W (0.425 s), and the first 1.125 W of a
  // stream (here the mission's start) cannot start an episode.
  CHECK(highGS[1] > 19.1);
}

TEST_CASE("adaptive: position-only input keeps the fixed position window") {
  TelemetryPipeline p;
  FlyOptions o;
  o.positionOnly = true;
  fly(p, levelTurn(60.0), 0.0, 3.0, o);
  const FilterState f = p.snapshot().filter;
  CHECK_FALSE(f.adaptive);
  CHECK(f.input == FilterState::Input::PositionOnly);
  CHECK(f.accelWindowS == PipelineConfig{}.accelWindowPositionOnlyS);
  CHECK(f.smoothingTauS == PipelineConfig{}.smoothingTauS);
  CHECK(p.snapshot().current.g == Approx(2.0).epsilon(0.01));
}

TEST_CASE("toggling adaptive filtering in setConfig does not reset the estimators") {
  // At 240 Hz switching off grows the window from ~10 to 80 ms; it keeps reporting while it
  // fills (the span requirement is a warm-up rule only).
  for (double fps : {60.0, 240.0}) {
    CAPTURE(fps);
    TelemetryPipeline p;
    RecordingConsumer c;
    p.addConsumer(&c);
    FlyOptions o;
    o.fps = fps;
    double t = fly(p, levelTurn(60.0), 0.0, 3.0, o);
    CHECK(p.snapshot().filter.adaptive);
    const DataQuality q = p.snapshot().quality;

    PipelineConfig cfg = p.config();
    int invalid = 0;
    for (bool on : {false, true, false}) {
      CAPTURE(on);
      cfg.adaptiveFiltering = on;
      p.setConfig(cfg);
      CHECK(p.snapshot().filter.adaptive == on);  // applies immediately
      for (int i = 0; i < 20; ++i) {
        t = fly(p, levelTurn(60.0), t, t + 0.5 / fps, o);  // one sample
        invalid += c.last.valid ? 0 : 1;
        CHECK(c.last.g == Approx(2.0).epsilon(0.01));
      }
      // The noise statistics survive as well (adaptive windows are known at once).
      CHECK(p.snapshot().filter.sampleRateHz == Approx(fps).epsilon(0.01));
    }
    CHECK(invalid == 0);  // a reset would need several samples to become valid again
    CHECK(c.resets == 0);
    CHECK(p.snapshot().quality.accepted == q.accepted + 60);
    CHECK(p.snapshot().quality.resetsGap == 0);
  }
}
