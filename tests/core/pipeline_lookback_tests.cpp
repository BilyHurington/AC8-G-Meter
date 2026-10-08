// Spike gate look-back and noise detection: one-frame G impulses are real data and must be
// shown, glitches of up to SpikeGate::kMaxLookBack samples must not; noise the estimate has not
// caught up with yet raises the window at once; and the sample counters stay consistent when
// the gate revises a verdict.
#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <random>

#include "core/spike_gate.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

struct Flight {
  MetricsSnapshot snap;
  double maxShownG = -1e9;   // largest displayed G from fromS on
  double maxErr = 0.0, sumSq = 0.0;
  int n = 0, invalid = 0;
  double crossT = -1.0;      // first time the displayed G reached `crossAt`
  double rms() const { return n > 0 ? std::sqrt(sumSq / n) : 0.0; }
};

struct FlightOptions {
  double fps = 60.0;
  double durationS = 3.0;
  double fromS = 0.5;           // errors and maxima from here on
  double jitter = 0.0;          // frame time uniform in (1 +- jitter) / fps
  double crossAt = 1e9;
  std::function<void(int, RawFlightSample&)> mutate;
  PipelineConfig cfg;
};

// Flies the G profile nz(t) with velocity input; displayed G is compared with the truth.
Flight flyG(const std::function<double(double)>& nz, const FlightOptions& o) {
  GProfile prof(nz);
  TelemetryPipeline p(o.cfg);
  RecordingConsumer c;
  p.addConsumer(&c);
  std::mt19937 rng(4);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  Flight f;
  double t = 0.0;
  for (int i = 0; t < o.durationS; ++i) {
    RawFlightSample s = sampleAt(prof.trajectory(), t);
    if (o.mutate) o.mutate(i, s);
    p.push(s);
    t = o.jitter > 0.0 ? t + (1.0 + o.jitter * u(rng)) / o.fps : (i + 1) / o.fps;
    if (s.t < o.fromS) continue;
    if (!c.last.valid) {
      ++f.invalid;
      continue;
    }
    f.maxShownG = std::max(f.maxShownG, c.last.g);
    if (f.crossT < 0.0 && c.last.g >= o.crossAt) f.crossT = s.t;
    const double err = c.last.g - nz(c.last.t);
    f.maxErr = std::max(f.maxErr, std::abs(err));
    f.sumSq += err * err;
    ++f.n;
  }
  f.snap = p.snapshot();
  return f;
}

}  // namespace

TEST_CASE("a one-frame G impulse is shown; a glitch of the same size is not") {
  // A one-frame 16 G / 25 G pulse is a lasting velocity step of 2.5 - 8 m/s, below the gate
  // threshold: real data. The least-squares window (2.5 samples) shows it as 1 + (amp - 1) / 2
  // for the frame(s) it spans, smoothed by an EMA with tau = one frame: a displayed peak of
  // 1 + 0.316 (amp - 1) = 5.74 / 8.59 G. (A 3-sample median showed 1.00 G: the impulse was
  // erased.) The same velocity offset reverting after one frame is a glitch and never shows.
  for (double fps : {30.0, 60.0, 240.0}) {
    for (double amp : {16.0, 25.0}) {
      CAPTURE(fps);
      CAPTURE(amp);
      const int at = static_cast<int>(1.5 * fps);
      const double t0 = at / fps;  // a sample time: the pulse lasts exactly one interval
      FlightOptions o;
      o.fps = fps;
      const Flight impulse = flyG([&](double t) { return t >= t0 && t < t0 + 1.0 / fps ? amp : 1.0; }, o);
      const double expectedPeak = 1.0 + (1.0 - std::exp(-1.0)) * 0.5 * (amp - 1.0);
      CHECK(impulse.maxShownG == Approx(expectedPeak).epsilon(0.1));  // measured 5.74 / 8.59
      CHECK(impulse.maxShownG < 1.0 + 0.5 * (amp - 1.0));               // never more than the window shows
      CHECK(impulse.snap.quality.rejectedSpike == 0);
      CHECK(impulse.snap.quality.resetsJump == 0);
      CHECK(impulse.invalid == 0);
      CHECK(impulse.snap.current.g == Approx(1.0).epsilon(0.01));

      const double stepMS = (amp - 1.0) * kG0 / fps;  // the impulse's velocity step
      o.mutate = [&](int i, RawFlightSample& s) {
        if (i == at + 1) s.velocity->z += stepMS;
      };
      const Flight glitch = flyG([](double) { return 1.0; }, o);
      CHECK(glitch.maxErr < 0.05);  // measured 0.00
      CHECK(glitch.snap.quality.rejectedSpike == 1);
    }
  }
}

TEST_CASE("real G steps with frame-time jitter are never dropped and cost at most one frame") {
  // 2 -> 12 / 17 / 22 / 28 G in one frame, frame time +-50 %: the first sample after the step
  // may be held (it might be a glitch); the next one moves further from the old trend and
  // confirms it. Nothing is dropped or rejected.
  for (double fps : {30.0, 60.0}) {
    for (double high : {12.0, 17.0, 22.0, 28.0}) {
      CAPTURE(fps);
      CAPTURE(high);
      FlightOptions o;
      o.fps = fps;
      o.jitter = 0.5;
      o.fromS = 1.8;
      const Flight f = flyG([&](double t) { return t < 1.5 ? 2.0 : high; }, o);
      CHECK(f.snap.quality.rejectedSpike == 0);
      CHECK(f.snap.quality.resetsJump == 0);
      CHECK(f.invalid == 0);
      CHECK(f.maxErr < 0.1);  // settled 0.3 s after the step (measured <= 0.06)
    }
    // Delay: a step large enough to be held reaches its midpoint at most one frame after a
    // step too small to be suspect (the filter is linear otherwise). Measured: the same frame.
    double crossT[2] = {};
    for (int k = 0; k < 2; ++k) {
      const double high = k == 0 ? 4.0 : 24.0;
      FlightOptions o;
      o.fps = fps;
      o.crossAt = 0.5 * (2.0 + high);
      crossT[k] = flyG([&](double t) { return t < 1.5 ? 2.0 : high; }, o).crossT;
    }
    CHECK(crossT[0] > 1.5);
    CHECK(crossT[1] <= crossT[0] + 1.0 / fps + 1e-9);
  }
}

TEST_CASE("glitch bursts up to kMaxLookBack samples are removed, longer ones read as real") {
  // A velocity offset that lasts kMaxLookBack + 1 samples and then reverts cannot be told from
  // a real two-way velocity step, so it is shown (never discard a real reading).
  for (int length = 1; length <= SpikeGate::kMaxLookBack + 1; ++length) {
    CAPTURE(length);
    FlightOptions o;
    o.mutate = [&](int i, RawFlightSample& s) {
      if (i >= 120 && i < 120 + length) s.velocity->z += 3.0;
    };
    const Flight f = flyG([](double) { return 3.0; }, o);
    if (length <= SpikeGate::kMaxLookBack) {
      CHECK(f.maxErr < 0.05);  // measured 0.00
      CHECK(f.snap.quality.rejectedSpike == static_cast<std::uint64_t>(length));
    } else {
      CHECK(f.maxErr > 3.0);
      CHECK(f.snap.quality.rejectedSpike == 0);
    }
    CHECK(f.invalid == 0);
  }
}

TEST_CASE("a noise step shows no transient: the gate raises the window before the estimate") {
  // Velocity noise 0.005 -> 0.5 m/s per axis at t = 3 s. The noise estimate needs ~0.1 s to
  // follow; meanwhile the window sized for the clean data (10 ms at 1000 Hz) read up to 8.5 G
  // of error. Scattered rejections and a busy gate now take the longest window at once.
  for (double fps : {240.0, 1000.0}) {
    CAPTURE(fps);
    Gaussian noise(8);
    FlightOptions o;
    o.fps = fps;
    o.durationS = 4.0;
    o.fromS = 2.9;
    o.mutate = [&](int, RawFlightSample& s) { *s.velocity += noise.vec(s.t < 3.0 ? 0.005 : 0.5); };
    const Flight f = flyG([](double t) { return 3.0 + 2.0 * std::sin(t); }, o);
    CHECK(f.maxErr < 1.0);  // measured 0.55 / 0.37
    CHECK(f.invalid == 0);
    CHECK(f.snap.quality.resetsJump == 0);
  }
}

TEST_CASE("a real step of more than 30 G stays valid at 30 - 240 Hz") {
  // Rejected as inconsistent, then recognised as a jump from the agreeing rejected samples.
  // Neither the scattered-noise check nor the busy gate may treat it as noise (that would
  // lengthen the window and its warm-up).
  for (double fps : {30.0, 60.0, 240.0}) {
    CAPTURE(fps);
    FlightOptions o;
    o.fps = fps;
    const Flight f = flyG([](double t) { return t < 1.5 ? 1.0 : 40.0; }, o);
    CHECK(f.invalid == 0);
    CHECK(f.snap.quality.resetsJump == 1);
    CHECK(f.snap.current.g == Approx(40.0).epsilon(0.01));
    if (fps > 31.0) CHECK(f.snap.filter.accelWindowS < PipelineConfig{}.adaptiveMaxWindowS);  // not raised
  }
}

TEST_CASE("noise on one axis only is measured as such and passes the gate") {
  // 0.5 m/s on the vertical axis only. A quantile of the 3-D residual norm read 0.14 and
  // rejected 9.5 % of the samples at 240 Hz.
  for (double fps : {240.0, 1000.0}) {
    CAPTURE(fps);
    std::uint64_t rejected[2] = {};
    for (int oneAxis = 0; oneAxis < 2; ++oneAxis) {
      CAPTURE(oneAxis);
      Gaussian noise(9);
      FlightOptions o;
      o.fps = fps;
      o.durationS = 6.0;
      o.mutate = [&](int, RawFlightSample& s) {
        if (oneAxis)
          s.velocity->z += noise(0.5);
        else
          *s.velocity += noise.vec(0.5);
      };
      const Flight f = flyG([](double t) { return 4.0 + 3.0 * std::sin(2.0 * t); }, o);
      CHECK(f.snap.filter.velocityNoiseMS == Approx(0.5).epsilon(0.2));
      CHECK(f.rms() < 0.5);
      rejected[oneAxis] = f.snap.quality.rejectedSpike;  // while the estimate starts up
    }
    CHECK(rejected[1] <= rejected[0] + 3);  // measured 0 / 4 vs 0 / 7
  }
}

TEST_CASE("velocity quantised coarser than its noise keeps a 10 ms window") {
  // 0.2 m/s steps plus 0.02 m/s noise at 1000 Hz: most residuals are exactly 0, so the noise
  // reads 0, and the 2.5-sample window (2.5 ms) read 2.96 G RMS. The adaptiveMinWindowS floor
  // holds it at 10 ms (and the busy gate raises it while the steps look like glitches).
  Gaussian noise(3);
  FlightOptions o;
  o.fps = 1000.0;
  o.durationS = 4.0;
  o.mutate = [&](int, RawFlightSample& s) {
    *s.velocity += noise.vec(0.02);
    auto q = [](double v) { return std::round(v / 0.2) * 0.2; };
    s.velocity = Vec3{q(s.velocity->x), q(s.velocity->y), q(s.velocity->z)};
  };
  const Flight f = flyG([](double t) { return 4.0 + 3.0 * std::sin(2.0 * t); }, o);
  CHECK(f.rms() < 0.4);  // measured 0.29
  CHECK(f.snap.filter.accelWindowS >= PipelineConfig{}.adaptiveMinWindowS);
  CHECK(f.invalid == 0);

  // Clean 1000 Hz data: the floor, not 2.5 samples.
  FlightOptions clean;
  clean.fps = 1000.0;
  const Flight c = flyG([](double t) { return 4.0 + 3.0 * std::sin(2.0 * t); }, clean);
  CHECK(c.snap.filter.accelWindowS == Approx(PipelineConfig{}.adaptiveMinWindowS));
  CHECK(c.maxErr < 0.1);
}

TEST_CASE("sample counts: accepted + rejected = pushed through look-back corrections and jumps") {
  // A dirty stream: single glitches and bursts (removed after the fact: accepted, then
  // rejected), jumps (rejected, then used: rejected, then accepted), non-finite values,
  // duplicate timestamps, a noisy stretch. The mission starts after 1 s.
  GProfile prof([](double t) { return 3.0 + 2.0 * std::sin(t); });
  TelemetryPipeline p;
  std::mt19937 rng(12);
  Gaussian noise(5);
  std::uint64_t pushed = 0, pushedActive = 0;
  double lastT = 0.0;
  for (int i = 0; i < 1200; ++i) {
    const double t = i / 60.0;
    RawFlightSample s = sampleAt(prof.trajectory(), t);
    s.phase = t < 1.0 ? MissionPhase::None : MissionPhase::Active;
    const bool nearJump = (i >= 395 && i < 410) || (i >= 595 && i < 610);
    if (rng() % 8 == 0 && !nearJump) s.velocity->z += 3.0;        // sub-gate glitches, some in bursts
    if (i % 97 == 0) s.velocity->x += 400.0;                      // gross glitch
    if (i >= 400 && i < 600) s.velocity->x += 30.0;               // velocity jumps
    if (i >= 700 && i < 760) *s.velocity += noise.vec(1.0);       // noise the estimate lags
    if (i % 151 == 0) s.velocity->y = std::nan("");               // non-finite
    if (i % 173 == 0 && i > 0) s.t = lastT;                       // duplicate timestamp
    p.push(s);
    lastT = s.t;
    ++pushed;
    if (s.phase == MissionPhase::Active) ++pushedActive;
  }
  const MetricsSnapshot snap = p.snapshot();
  const DataQuality& q = snap.quality;
  CHECK(q.rejectedSpike > 0);
  CHECK(q.resetsJump > 0);
  CHECK(q.rejectedNonFinite > 0);
  CHECK(q.rejectedTime > 0);
  CHECK(q.accepted + q.rejectedNonFinite + q.rejectedTime + q.rejectedSpike == pushed);
  CHECK(snap.mission.acceptedSamples + snap.mission.rejectedSamples == pushedActive);
}
