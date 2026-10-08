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

RampResult flyRamp(const PipelineConfig& cfg, double fps) {
  TelemetryPipeline p(cfg);
  GProfile prof(rampNz);
  RampResult r;
  double lagSum = 0.0;
  int n = 0;
  for (int i = 0; i < static_cast<int>(3.0 * fps); ++i) {
    const double t = i / fps;
    p.push(sampleAt(prof.trajectory(), t));
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
  CHECK(snap.filter.accelWindowS >= 0.08);
  CHECK(snap.filter.accelWindowS <= PipelineConfig{}.adaptiveMaxWindowS);
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

TEST_CASE("adaptive: falls back to the fixed windows until the noise estimate is ready") {
  TelemetryPipeline p;
  for (int i = 0; i < 5; ++i) p.push(levelSample(i / 60.0));
  const FilterState f = p.snapshot().filter;
  CHECK_FALSE(f.adaptive);
  CHECK(f.accelWindowS == PipelineConfig{}.accelWindowS);
  CHECK(f.smoothingTauS == PipelineConfig{}.smoothingTauS);
}

TEST_CASE("adaptive: position-only input keeps the fixed position window") {
  TelemetryPipeline p;
  FlyOptions o;
  o.positionOnly = true;
  fly(p, levelTurn(60.0), 0.0, 3.0, o);
  const FilterState f = p.snapshot().filter;
  CHECK_FALSE(f.adaptive);
  CHECK(f.accelWindowS == PipelineConfig{}.accelWindowPositionOnlyS);
  CHECK(f.smoothingTauS == PipelineConfig{}.smoothingTauS);
  CHECK(p.snapshot().current.g == Approx(2.0).epsilon(0.01));
}

TEST_CASE("toggling adaptive filtering in setConfig does not reset the estimators") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  double t = fly(p, levelTurn(60.0), 0.0, 3.0);
  CHECK(p.snapshot().filter.adaptive);
  const DataQuality q = p.snapshot().quality;

  PipelineConfig cfg = p.config();
  for (bool on : {false, true, false}) {
    CAPTURE(on);
    cfg.adaptiveFiltering = on;
    p.setConfig(cfg);
    CHECK(p.snapshot().filter.adaptive == on);  // applies immediately
    t = fly(p, levelTurn(60.0), t, t + 0.5 / 60.0);  // one sample
    CHECK(c.last.valid);  // a reset would need several samples to become valid again
    CHECK(c.last.g == Approx(2.0).epsilon(0.01));
    // The noise statistics survive as well (adaptive windows are known at once).
    CHECK(p.snapshot().filter.sampleRateHz == Approx(60.0).epsilon(0.01));
  }
  CHECK(c.resets == 0);
  CHECK(p.snapshot().quality.accepted == q.accepted + 3);
  CHECK(p.snapshot().quality.resetsGap == 0);
}
