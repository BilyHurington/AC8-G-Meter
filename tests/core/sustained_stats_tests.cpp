// Dual mission statistics: peak values from the live G, sustained values (and high-G
// episodes) from the SustainedFilter path; restarts (H3), width changes (L5), noise (H1).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <vector>

#include "core/sustained_filter.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

using Sample = ExcursionFilter::Sample;

template <typename F>
std::vector<Sample> run(F& f, const std::vector<Sample>& in) {
  std::vector<Sample> out;
  Sample s;
  for (const Sample& x : in) {
    f.push(x.t, x.v);
    while (f.pop(s)) out.push_back(s);
  }
  f.flush();
  while (f.pop(s)) out.push_back(s);
  return out;
}

std::vector<Sample> series(double fps, double seconds, const std::function<double(double)>& v) {
  std::vector<Sample> out;
  for (int i = 0; i / fps < seconds; ++i) out.push_back({i / fps, v(i / fps)});
  return out;
}

double maxOf(const std::vector<Sample>& v) {
  double m = -1e300;
  for (const Sample& s : v) m = std::max(m, s.v);
  return m;
}

struct Flight {
  MissionReport report;
  std::vector<MetricsSnapshot> snaps;  // per Active frame, when recordSnaps
  double liveMax = -1e9, liveMin = 1e9, liveMaxOnset = -1e9, liveMinOnset = 1e9;
  std::uint64_t pushedOutsidePause = 0;
};

struct FlightSpec {
  std::function<double(double)> nz;
  double seconds = 4.0;
  double fps = 60.0;
  double noiseSigma = 0.0;
  std::uint32_t seed = 7;
  std::function<MissionPhase(double)> phase;            // default: Active throughout
  std::function<void(double, TelemetryPipeline&)> hook;  // called before each push
  bool recordSnaps = false;
  PipelineConfig cfg;
};

Flight flyMission(const FlightSpec& spec) {
  TelemetryPipeline p(spec.cfg);
  RecordingConsumer c;
  p.addConsumer(&c);
  GProfile prof(spec.nz);
  Gaussian noise(spec.seed);
  Flight f;
  for (int i = 0;; ++i) {
    const double t = i / spec.fps;
    if (t >= spec.seconds) break;
    RawFlightSample s = sampleAt(prof.trajectory(), t);
    if (spec.noiseSigma > 0.0) *s.velocity += noise.vec(spec.noiseSigma);
    s.phase = spec.phase ? spec.phase(t) : MissionPhase::Active;
    if (spec.hook) spec.hook(t, p);
    p.push(s);
    if (s.phase != MissionPhase::Paused) ++f.pushedOutsidePause;
    if (s.phase == MissionPhase::Active && c.last.valid && c.metricsActive > 0) {
      f.liveMax = std::max(f.liveMax, c.last.g);
      f.liveMin = std::min(f.liveMin, c.last.g);
      f.liveMaxOnset = std::max(f.liveMaxOnset, c.last.onsetRate);
      f.liveMinOnset = std::min(f.liveMinOnset, c.last.onsetRate);
      if (spec.recordSnaps) f.snaps.push_back(p.snapshot());
    }
  }
  for (int i = 0; i < 12; ++i) {
    RawFlightSample s = sampleAt(prof.trajectory(), spec.seconds + i / spec.fps);
    s.phase = MissionPhase::Ended;
    p.push(s);
    ++f.pushedOutsidePause;
  }
  REQUIRE(p.snapshot().lastReport);
  f.report = *p.snapshot().lastReport;
  const DataQuality q = p.snapshot().quality;
  CHECK(q.accepted + q.rejectedNonFinite + q.rejectedTime + q.rejectedSpike == f.pushedOutsidePause);
  return f;
}

double binSum(const MissionStats& s) { return std::accumulate(s.timeInGBinS.begin(), s.timeInGBinS.end(), 0.0); }

// Mean of trace.gSustained over the buckets in [t0, t1).
double sustainedMean(const MissionTrace& tr, double t0, double t1) {
  double sum = 0.0;
  int n = 0;
  for (std::size_t i = 0; i < tr.size(); ++i) {
    const double t = tr.timeAt(i);
    if (t >= t0 && t < t1 && std::isfinite(tr.gSustained[i])) {
      sum += tr.gSustained[i];
      ++n;
    }
  }
  REQUIRE(n > 0);
  return sum / n;
}

}  // namespace

// ---- filter units ---------------------------------------------------------------------

TEST_CASE("centred mean: zero phase, truncated at the ends, width 0 passes through") {
  CentredMean m;
  m.setWidth(0.0);
  Sample s;
  m.push(1.0, 5.0);
  REQUIRE(m.pop(s));
  CHECK(s.v == 5.0);

  m.setWidth(0.05);  // 60 Hz: the sample and one on each side
  const auto out = run(m, series(60.0, 1.0, [](double t) { return t >= 0.5 && t < 0.51 ? 4.0 : 1.0; }));
  REQUIRE(out.size() == 60);
  CHECK(out[29].v == Approx(2.0));  // neighbour of the one-sample bump at 30/60
  CHECK(out[30].v == Approx(2.0));
  CHECK(out[31].v == Approx(2.0));
  CHECK(out[32].v == Approx(1.0));
  CHECK(out[30].t == Approx(0.5));  // own timestamps
  CHECK(out.front().v == Approx(1.0));
}

TEST_CASE("sustained filter: removes short excursions, keeps long ones exactly, symmetric") {
  for (double fps : {60.0, 240.0}) {
    CAPTURE(fps);
    for (double len : {0.05, 0.1, 0.14}) {  // <= ~0.75 W: removed entirely
      CAPTURE(len);
      SustainedFilter up(0.2), down(0.2);
      CHECK(maxOf(run(up, series(fps, 2.0, [&](double t) { return t >= 1.0 && t < 1.0 + len ? 15.0 : 1.0; })))
            == Approx(1.0));
      const auto dip = run(down, series(fps, 2.0, [&](double t) { return t >= 1.0 && t < 1.0 + len ? -4.0 : 1.0; }));
      double lo = 1e9;
      for (const Sample& s : dip) lo = std::min(lo, s.v);
      CHECK(lo == Approx(1.0));
    }
    // >= ~1.25 W: the plateau's interior comes out unchanged.
    SustainedFilter f(0.2);
    const auto in = series(fps, 3.0, [](double t) { return t >= 1.0 && t < 1.4 ? 9.0 : 1.0; });
    const auto out = run(f, in);
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
      CHECK(out[i].t == in[i].t);
      if (in[i].t > 1.05 && in[i].t < 1.35) CHECK(out[i].v == Approx(9.0));
      if (in[i].t < 0.9 || in[i].t > 1.5) CHECK(out[i].v == Approx(1.0));
    }
  }
  // Odd symmetry: f(-x) == -f(x) for any input (no preferred direction, hence no bias).
  Gaussian g(3);
  std::vector<Sample> x, nx;
  for (int i = 0; i < 600; ++i) {
    const double v = 2.0 + g(1.0);
    x.push_back({i / 60.0, v});
    nx.push_back({i / 60.0, -v});
  }
  SustainedFilter a(0.2), b(0.2);
  const auto ya = run(a, x), yb = run(b, nx);
  REQUIRE(ya.size() == yb.size());
  for (std::size_t i = 0; i < ya.size(); ++i) CHECK(ya[i].v == Approx(-yb[i].v));
}

TEST_CASE("sustained filter: unbiased on noise, where open-then-close alone is not") {
  Gaussian g(11);
  std::vector<Sample> in;
  for (int i = 0; i < 60 * 60; ++i) in.push_back({i / 60.0, 1.0 + g(0.5)});
  SustainedFilter sus(0.2);
  ExcursionFilter oc(0.2);
  const auto a = run(sus, in);
  const auto b = run(oc, in);
  auto mean = [](const std::vector<Sample>& v) {
    double s = 0.0;
    for (const Sample& x : v) s += x.v;
    return s / static_cast<double>(v.size());
  };
  CHECK(mean(a) == Approx(1.0).epsilon(0.02));
  CHECK(mean(b) < 0.85);  // the old path: opening first pulls noise down
}

TEST_CASE("sustained filter: bounded delay and memory, width 0 passes through") {
  SustainedFilter off(0.0);
  Sample s;
  off.push(1.0, 3.0);
  REQUIRE(off.pop(s));
  CHECK(s.v == 3.0);

  SustainedFilter f(0.2);
  double worst = 0.0, lastOut = -1.0;
  for (int i = 0; i < 600; ++i) {
    const double t = i / 60.0;
    f.push(t, 1.0);
    while (f.pop(s)) {
      CHECK(s.t > lastOut);
      lastOut = s.t;
    }
    if (lastOut >= 0.0) worst = std::max(worst, t - lastOut);
  }
  CHECK(worst <= 0.025 + 0.4 + 3.0 / 60.0 + 1e-9);  // W / 8 + 2 W, a frame per look-ahead stage
  f.flush();
  CHECK(f.pending() == 0);

  SustainedFilter stuck(0.2);
  for (int i = 0; i < 3 * static_cast<int>(ExcursionFilter::kMaxPending); ++i) stuck.push(1.0, 1.0);
  CHECK(stuck.pending() <= 6 * ExcursionFilter::kMaxPending);
}

// ---- dual values in the pipeline ------------------------------------------------------

TEST_CASE("peak statistics are the live G at once; sustained ones follow ~2 W later") {
  FlightSpec spec;
  spec.nz = [](double t) {
    if (t < 1.0) return 1.0;
    if (t < 1.5) return 1.0 + 12.0 * (t - 1.0);  // 7 G
    if (t >= 2.5 && t < 2.6) return 11.0;        // 0.1 s bump
    if (t < 3.5) return 7.0;
    return 1.0;
  };
  spec.seconds = 5.0;
  spec.recordSnaps = true;
  const Flight f = flyMission(spec);
  // HUD MAX / MIN read the mission's peak values: never behind the live G (M2).
  double runningMax = -1e9, runningMin = 1e9;
  for (const MetricsSnapshot& s : f.snaps) {
    runningMax = std::max(runningMax, s.current.g);
    runningMin = std::min(runningMin, s.current.g);
    CHECK(s.mission.maxG == runningMax);
    CHECK(s.mission.minG == runningMin);
  }
  const MissionStats& st = f.report.stats;
  CHECK(st.maxG == f.liveMax);
  CHECK(st.maxG > 10.0);
  CHECK(st.maxGAtS == Approx(2.6).epsilon(0.03));
  // Onset extremes are the live ones (M1): the HUD's ONSET row, not a filtered estimate.
  CHECK(st.maxOnset == f.liveMaxOnset);
  CHECK(st.minOnset == f.liveMinOnset);
  REQUIRE(st.hasSustainedData);
  CHECK(st.sustainedMaxG == Approx(7.0).epsilon(0.01));
  CHECK(st.sustainedMaxGAtS > 1.5);
  CHECK(st.sustainedMaxGAtS < 3.5);
  CHECK(st.sustainedMinG == Approx(1.0).epsilon(0.01));
  CHECK(st.highGEpisodes == 1);  // the bump does not split or add an episode
  CHECK(st.longestHighGS == Approx(2.05).epsilon(0.05));
  // Duration / histogram invariants.
  CHECK(binSum(st) <= st.durationS + 1e-9);
  CHECK(binSum(st) > st.durationS - 0.2);
  // Trace: the live band keeps the bump, the sustained line and the high-G shading do not.
  const MissionTrace& tr = f.report.trace;
  CHECK(*std::max_element(tr.gMax.begin(), tr.gMax.end()) > 10.0f);
  for (std::size_t i = 0; i < tr.size(); ++i) {
    CHECK(std::isfinite(tr.gSustained[i]));  // flushed to the end of the mission
    CHECK(tr.gSustained[i] < 7.05f);
    if (tr.timeAt(i) > 2.0 && tr.timeAt(i) < 3.0) CHECK(tr.highG[i] == 1);
    if (tr.timeAt(i) > 4.0) CHECK(tr.highG[i] == 0);
  }
  CHECK(sustainedMean(tr, 2.0, 2.45) == Approx(7.0).epsilon(0.01));
}

TEST_CASE("H1: noisy input gives unbiased sustained values, and real turns keep their episode") {
  // 7 G held 3 s in level flight, velocity noise per axis.
  auto turn = [](double t) { return t > 1.5 && t < 4.5 ? 7.0 : 1.0; };
  struct Case {
    double sigma, fps, tolMax, tolMin;
  };
  // At 240 Hz the extremes are within the review's targets; at 60 Hz the bias is gone too (the
  // means), but the extremes of 3 s of noise still spread by up to ~0.3 G (before: 6.67 / 0.07).
  for (const Case& c : {Case{0.3, 240.0, 0.15, 0.2}, Case{0.3, 60.0, 0.35, 0.35}}) {
    CAPTURE(c.fps);
    FlightSpec spec;
    spec.nz = turn;
    spec.seconds = 6.0;
    spec.fps = c.fps;
    spec.noiseSigma = c.sigma;
    const Flight f = flyMission(spec);
    const MissionStats& st = f.report.stats;
    REQUIRE(st.hasSustainedData);
    CHECK(st.sustainedMaxG == Approx(7.0).epsilon(c.tolMax / 7.0));
    CHECK(st.sustainedMinG == Approx(1.0).epsilon(c.tolMin));
    CHECK(sustainedMean(f.report.trace, 2.0, 4.0) == Approx(7.0).epsilon(0.01));
    CHECK(sustainedMean(f.report.trace, 4.9, 6.0) == Approx(1.0).epsilon(0.06));
    CHECK(st.highGEpisodes == 1);
    // Peak values are the live G as shown: wider than the sustained ones.
    CHECK(st.maxG >= st.sustainedMaxG);
    CHECK(st.minG <= st.sustainedMinG);
  }
  // Very noisy 60 Hz data: the real 3 s turn stays one episode (before: none).
  FlightSpec heavy;
  heavy.nz = turn;
  heavy.seconds = 6.0;
  heavy.noiseSigma = 1.0;
  const Flight f = flyMission(heavy);
  CHECK(f.report.stats.highGEpisodes == 1);
  CHECK(f.report.stats.longestHighGS > 2.5);
}

TEST_CASE("H3: an excursion in progress when the mission starts is judged with the samples before it") {
  // 0.16 s at 15 G from 0.9 s; the mission starts at 1.0 s.
  FlightSpec spec;
  spec.nz = [](double t) { return t >= 0.9 && t < 1.06 ? 15.0 : 1.0; };
  spec.seconds = 3.0;
  spec.phase = [](double t) { return t < 1.0 ? MissionPhase::None : MissionPhase::Active; };
  const Flight f = flyMission(spec);
  const MissionStats& st = f.report.stats;
  CHECK(st.maxG > 10.0);  // peak: the HUD showed it during the mission
  REQUIRE(st.hasSustainedData);
  CHECK(st.sustainedMaxG < 3.0);  // before: 15
  CHECK(st.highGEpisodes == 0);   // before: 1
  // A mission that starts with the stream itself cannot see before it: its first 1.125 W are
  // unconfirmed.
  FlightSpec atStart = spec;
  atStart.nz = [](double t) { return t >= 0.05 && t < 0.21 ? 15.0 : 1.0; };
  atStart.phase = nullptr;
  const Flight g = flyMission(atStart);
  CHECK(g.report.stats.sustainedMaxG < 3.0);
  CHECK(g.report.stats.highGEpisodes == 0);
}

TEST_CASE("H3: a G mode change during a short bump records no sustained value of it") {
  FlightSpec spec;
  spec.nz = [](double t) { return t >= 1.95 && t < 2.08 ? 12.0 : 1.0; };
  spec.seconds = 4.0;
  bool switched = false;
  spec.hook = [&](double t, TelemetryPipeline& p) {
    if (!switched && t >= 2.0) {
      PipelineConfig cfg = p.config();
      cfg.gMode = GMode::Total;
      p.setConfig(cfg);
      switched = true;
    }
  };
  const Flight f = flyMission(spec);
  const MissionStats& st = f.report.stats;
  CHECK(f.report.trace.startS == Approx(2.0).epsilon(0.02));
  REQUIRE(st.hasSustainedData);
  CHECK(st.sustainedMaxG < 2.0);  // before: 12
  CHECK(st.highGEpisodes == 0);
  CHECK(st.sustainedMaxGAtS >= f.report.trace.startS);
}

TEST_CASE("H3: a pause between two short bumps does not join them") {
  FlightSpec spec;
  spec.nz = [](double t) { return (t >= 1.88 && t < 2.0) || (t >= 4.0 && t < 4.12) ? 8.0 : 1.0; };
  spec.seconds = 6.0;
  spec.phase = [](double t) { return t >= 2.0 && t < 4.0 ? MissionPhase::Paused : MissionPhase::Active; };
  const Flight f = flyMission(spec);
  CHECK(f.report.stats.sustainedMaxG < 2.0);  // before: 8 (0.12 + 0.12 s >= W across the pause)
  CHECK(f.report.stats.highGEpisodes == 0);
}

TEST_CASE("L5: changing W mid-episode does not split the episode") {
  FlightSpec spec;
  spec.nz = [](double t) { return t > 1.0 && t < 5.0 ? 7.0 : 1.0; };
  spec.seconds = 6.0;
  int changes = 0;
  spec.hook = [&](double t, TelemetryPipeline& p) {
    const double w = t < 2.5 ? 0.2 : (t < 3.5 ? 0.35 : 0.1);
    if (p.config().minExcursionS != w) {
      PipelineConfig cfg = p.config();
      cfg.minExcursionS = w;
      p.setConfig(cfg);
      ++changes;
    }
  };
  const Flight f = flyMission(spec);
  CHECK(changes == 2);
  CHECK(f.report.stats.highGEpisodes == 1);  // before: 3
  CHECK(f.report.stats.longestHighGS > 3.8);
  CHECK(f.report.minExcursionS == 0.1);  // the width in use at the end
}

TEST_CASE("invariants: sample counts and histogram with glitches, gaps and a teleport") {
  int frame = 0;
  TelemetryPipeline p;
  GProfile prof([](double t) { return 3.0 + 2.0 * std::sin(t); });
  std::uint64_t pushed = 0;
  for (double t = 0.0; t < 8.0; t += 1.0 / 60.0, ++frame) {
    if (t > 3.0 && t < 3.5) continue;  // gap
    RawFlightSample s = sampleAt(prof.trajectory(), t);
    if (frame % 97 == 0) s.velocity->z += 300.0;  // glitch
    if (frame == 400) s.position.x += 5000.0;      // teleport
    if (frame % 211 == 0) s.velocity->x = std::nan("");
    p.push(s);
    ++pushed;
  }
  const MetricsSnapshot snap = p.snapshot();
  const DataQuality& q = snap.quality;
  CHECK(q.accepted + q.rejectedNonFinite + q.rejectedTime + q.rejectedSpike == pushed);
  CHECK(snap.mission.acceptedSamples + snap.mission.rejectedSamples == pushed);
  CHECK(q.resetsGap == 1);
  CHECK(q.rejectedSpike > 0);
  CHECK(binSum(snap.mission) <= snap.mission.durationS + 1e-9);
  CHECK(binSum(snap.mission) > snap.mission.durationS - 1.0);
  CHECK(snap.mission.hasSustainedData);
}
