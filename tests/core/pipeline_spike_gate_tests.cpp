// Spike gate details: glitches below the threshold (look-back), runs of rejections (real jump
// vs noise the gate underestimated), and gating right after a reset.
#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "core/spike_gate.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

struct Run {
  MetricsSnapshot snap;
  int resets = 0;  // onReset calls
  int invalid = 0;
  double maxErr = 0.0, sumSq = 0.0;
  int n = 0;
  double rms() const { return n > 0 ? std::sqrt(sumSq / n) : 0.0; }
};

// Flies the G profile nz(t) (velocity input) at a fixed rate; mutate(i, sample) injects the
// faults. Displayed G is compared with the truth on every sample from fromS on.
template <typename NzFn, typename Mutate>
Run flyProfile(NzFn nz, double fps, double durationS, Mutate mutate, double fromS = 0.5,
               const PipelineConfig& cfg = {}) {
  GProfile prof(nz);
  TelemetryPipeline p(cfg);
  RecordingConsumer c;
  p.addConsumer(&c);
  Run r;
  for (int i = 0; i < static_cast<int>(durationS * fps); ++i) {
    RawFlightSample s = sampleAt(prof.trajectory(), i / fps);
    mutate(i, s);
    p.push(s);
    if (s.t < fromS) continue;
    if (!c.last.valid) {
      ++r.invalid;
      continue;
    }
    const double err = c.last.g - nz(c.last.t);
    r.maxErr = std::max(r.maxErr, std::abs(err));
    r.sumSq += err * err;
    ++r.n;
  }
  r.snap = p.snapshot();
  r.resets = c.resets;
  return r;
}

const auto constant3G = [](double) { return 3.0; };

}  // namespace

TEST_CASE("velocity glitches below the gate threshold never reach the display") {
  // At 30 and 60 Hz each passes the gate (below spikeDeviationG * g0 * dt: 4.9 m/s at 60 Hz,
  // 9.8 m/s at 30 Hz) but would read as 3 - 13 G in a 3-sample least-squares window; the
  // look-back holds it (it misses the trend by more than 5 G in one step) and removes it when
  // the next sample returns to the trend. Without the look-back they showed as +2.9 G (3 m/s,
  // 30 Hz), +5.8 G (3 m/s, 60 Hz), +8.7 G (4.5 m/s, 60 Hz), +7.7 G (8 m/s, 30 Hz). At 240 Hz
  // the gate itself rejects them.
  struct Case {
    double fps, glitchMS;
  };
  for (const Case k : {Case{30.0, 3.0}, Case{30.0, 4.5}, Case{30.0, 8.0}, Case{60.0, 3.0}, Case{60.0, 4.5},
                       Case{60.0, 8.0}, Case{240.0, 3.0}, Case{240.0, 4.5}, Case{240.0, 8.0}}) {
    CAPTURE(k.fps);
    CAPTURE(k.glitchMS);
    const int at = static_cast<int>(2.0 * k.fps);
    const Run r = flyProfile(constant3G, k.fps, 4.0, [&](int i, RawFlightSample& s) {
      if (i == at) s.velocity->z += k.glitchMS;
    });
    CHECK(r.maxErr < 0.1);  // measured 0.00
    CHECK(r.invalid == 0);
    CHECK(r.snap.quality.rejectedSpike == 1);  // rejected, or removed from the window after the fact
    CHECK(r.snap.quality.resetsJump == 0);
    CHECK(r.snap.quality.resetsTeleport == 0);
    CHECK(r.snap.mission.maxG == Approx(3.0).epsilon(0.01));
  }
}

TEST_CASE("frequent sub-threshold glitches: 10 % of samples +3 m/s at 60 Hz") {
  // Bursts of up to SpikeGate::kMaxLookBack glitches are removed; a longer one reads like a
  // real two-way velocity step and is shown (pipeline_lookback_tests), so the random bursts
  // are capped at that length here (a longer one occurs about once in 10^4 samples). Before
  // the look-back this read 2.57 G RMS; the fixed filter with a 5-sample median reads 0.002 G.
  PipelineConfig fixed;
  fixed.adaptiveFiltering = false;
  fixed.medianWindow = 5;
  double sumAdaptive = 0.0, sumFixed = 0.0, worst = 0.0;
  constexpr int kSeeds = 10;
  for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
    CAPTURE(seed);
    double rms[2] = {};
    for (int k = 0; k < 2; ++k) {
      std::mt19937 rng(seed);
      int burst = 0;
      const Run r = flyProfile(constant3G, 60.0, 8.0, [&](int, RawFlightSample& s) {
        const bool glitch = rng() % 10 == 0 && burst < SpikeGate::kMaxLookBack;
        burst = glitch ? burst + 1 : 0;
        if (glitch) s.velocity->z += 3.0;
      }, 0.5, k == 0 ? PipelineConfig{} : fixed);
      CHECK(r.invalid == 0);
      CHECK(r.snap.quality.resetsJump == 0);
      CHECK(r.snap.quality.resetsTeleport == 0);
      rms[k] = r.rms();
    }
    sumAdaptive += rms[0];
    sumFixed += rms[1];
    worst = std::max(worst, rms[0]);
  }
  // Measured: 0.000 G on every seed adaptive, 0.002 G mean fixed.
  CHECK(sumAdaptive / kSeeds < 0.01);
  CHECK(sumAdaptive <= sumFixed);
  CHECK(worst < 0.05);
}

TEST_CASE("a real step of 15 - 30 G in one frame is accepted with one frame of delay") {
  // 3 -> 25 G at 60 Hz: the first post-step sample is suspect (it may be a glitch), the
  // second confirms it by moving further away from the old trend.
  auto nz = [](double t) { return t < 2.0 ? 3.0 : 25.0; };
  const Run r = flyProfile(nz, 60.0, 3.0, [](int, RawFlightSample&) {}, 2.2);
  CHECK(r.snap.quality.rejectedSpike == 0);
  CHECK(r.snap.quality.resetsJump == 0);
  CHECK(r.maxErr < 0.01);  // settled 0.2 s after the step
  CHECK(r.snap.mission.maxG == Approx(25.0).epsilon(0.01));
}

TEST_CASE("a step of more than 30 G in one frame is a velocity jump, not a discontinuity") {
  // 10 G -> 42 G -> 10 G: each step is rejected twice, then accepted as a jump. Only the
  // acceleration window restarts (from the agreeing rejected samples, which then count as
  // accepted): no onReset, valid throughout, and the high-G episode is not split (it was 3
  // episodes before).
  auto nz = [](double t) { return t < 2.0 ? 10.0 : (t < 2.5 ? 42.0 : 10.0); };
  const Run r = flyProfile(nz, 60.0, 5.0, [](int, RawFlightSample&) {});
  CHECK(r.snap.quality.rejectedSpike == 0);
  CHECK(r.snap.quality.accepted == 300);
  CHECK(r.snap.quality.resetsJump == 2);
  CHECK(r.snap.quality.resetsTeleport == 0);
  CHECK(r.resets == 0);
  CHECK(r.invalid == 0);
  CHECK(r.snap.mission.highGEpisodes == 1);
  // Live mission stats lag the G by 2 x minExcursionS (0.4 s) until the mission ends.
  CHECK(r.snap.mission.longestHighGS == Approx(4.5).epsilon(0.02));
  CHECK(r.snap.mission.maxG == Approx(42.0).epsilon(0.01));
  CHECK(r.snap.current.g == Approx(10.0).epsilon(0.01));
}

TEST_CASE("a noise step at 240 and 1000 Hz causes no reset storm") {
  // Velocity noise 0.005 -> 0.5 m/s per axis at t = 3 s. Before: at 1000 Hz 961 spike
  // rejections, 467 resets and 2000 invalid samples, because the noise estimate only saw
  // gate-accepted samples (and restarted on every reset).
  for (double fps : {240.0, 1000.0}) {
    CAPTURE(fps);
    Gaussian noise(8);
    auto nz = [](double t) { return 3.0 + 2.0 * std::sin(t); };
    const Run settled = flyProfile(nz, fps, 6.0, [&](int, RawFlightSample& s) {
      *s.velocity += noise.vec(s.t < 3.0 ? 0.005 : 0.5);
    }, 3.3);
    const DataQuality& q = settled.snap.quality;
    CHECK(q.resetsJump + q.resetsTeleport <= 1);  // measured 0
    CHECK(q.rejectedSpike < 150);                   // measured 8 / 41
    CHECK(settled.invalid == 0);
    // Fast attack: the larger of a 2 s median and a short-window quartile (measured 0.64 / 0.50).
    CHECK(settled.snap.filter.velocityNoiseMS > 0.4);
    CHECK(settled.snap.filter.velocityNoiseMS < 0.8);
    CHECK(settled.snap.filter.accelWindowS == Approx(PipelineConfig{}.adaptiveMaxWindowS));
    // Once the estimate has caught up, the error is what this noise implies for the fixed
    // 80 ms window (measured max 0.59 / 0.37 G, rms 0.22 / 0.15 G: as with adaptive off).
    CHECK(settled.maxErr < 1.0);
    CHECK(settled.rms() < 0.35);
  }
}

TEST_CASE("glitches right after a reset are gated") {
  // The first samples after a reset have no trend yet: the second one is gated against the
  // first at constant velocity. A glitch as the very first sample is outvoted by the samples
  // that follow (a run that agrees: restart from them, so they count as accepted).
  for (int k : {0, 1, 2, 3}) {
    CAPTURE(k);
    GProfile prof([](double t) { return 3.0 + 2.0 * std::sin(t); });
    TelemetryPipeline p;
    RecordingConsumer c;
    p.addConsumer(&c);
    double maxErr = 0.0;
    for (int i = 0; i < 240; ++i) {
      if (i == 120) p.reset();
      RawFlightSample s = sampleAt(prof.trajectory(), i / 60.0);
      if (i == 120 + k) s.velocity->z += 300.0;
      p.push(s);
      if (i > 120 && c.last.valid) maxErr = std::max(maxErr, std::abs(c.last.g - (3.0 + 2.0 * std::sin(s.t))));
    }
    CHECK(maxErr < 0.3);  // measured 0.11
    if (k == 0)
      CHECK(p.snapshot().quality.resetsJump == 1);
    else
      CHECK(p.snapshot().quality.rejectedSpike >= 1);
    CHECK(p.snapshot().quality.resetsTeleport == 0);
    CHECK(c.resets == 1);  // reset() itself only
    CHECK(c.last.valid);
  }
}
