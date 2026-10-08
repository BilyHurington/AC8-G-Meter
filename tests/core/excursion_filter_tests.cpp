// ExcursionFilter (opening + closing over a time width) and its use for mission statistics.
#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "core/excursion_filter.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {

using Sample = ExcursionFilter::Sample;

std::vector<Sample> runFilter(ExcursionFilter& f, const std::vector<Sample>& in) {
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

// 1 G with one rectangular excursion to `level` over [t0, t0 + d), sampled every dt.
std::vector<Sample> bump(double dt, double t0, double d, double level, double total = 2.0) {
  std::vector<Sample> v;
  for (int i = 0; i * dt < total; ++i) {
    const double t = i * dt;
    v.push_back({t, t >= t0 - 1e-9 && t < t0 + d - 1e-9 ? level : 1.0});
  }
  return v;
}

double maxOf(const std::vector<Sample>& v) {
  return std::max_element(v.begin(), v.end(), [](auto& a, auto& b) { return a.v < b.v; })->v;
}
double minOf(const std::vector<Sample>& v) {
  return std::min_element(v.begin(), v.end(), [](auto& a, auto& b) { return a.v < b.v; })->v;
}

struct MissionRun {
  MissionReport report;
  double liveMaxG = -1e9, liveMinG = 1e9;
};

// Flies `nz(t)` (level attitude) as an Active mission for `seconds`, then the results screen.
MissionRun flyMission(std::function<double(double)> nz, double seconds, double minExcursionS, bool jitter = false) {
  PipelineConfig cfg;
  cfg.minExcursionS = minExcursionS;
  TelemetryPipeline p(cfg);
  RecordingConsumer c;
  p.addConsumer(&c);
  GProfile prof(std::move(nz));
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> frame(1.0 / 144.0, 1.0 / 30.0);
  MissionRun r;
  double t = 0.0;
  while (t < seconds) {
    p.push(sampleAt(prof.trajectory(), t));
    if (c.last.valid && t > 0.3) {
      r.liveMaxG = std::max(r.liveMaxG, c.last.g);
      r.liveMinG = std::min(r.liveMinG, c.last.g);
    }
    t += jitter ? frame(rng) : 1.0 / 60.0;
  }
  FlyOptions ended;
  ended.phase = MissionPhase::Ended;
  fly(p, prof.trajectory(), t, t + 0.2, ended);
  REQUIRE(p.snapshot().lastReport);
  r.report = *p.snapshot().lastReport;
  return r;
}

double binSum(const MissionStats& s) { return std::accumulate(s.timeInGBinS.begin(), s.timeInGBinS.end(), 0.0); }

}  // namespace

TEST_CASE("excursion filter: width 0 passes samples through without delay") {
  ExcursionFilter f(0.0);
  Sample s;
  f.push(1.0, 7.0);
  REQUIRE(f.pop(s));
  CHECK(s.t == 1.0);
  CHECK(s.v == 7.0);
  CHECK(f.pending() == 0);
}

TEST_CASE("excursion filter: removes bumps and dips shorter than W, keeps longer ones exactly") {
  const double dt = 1.0 / 60.0;
  for (int n : {1, 5, 11}) {  // 11 samples = 0.183 s < 0.2 s
    CAPTURE(n);
    ExcursionFilter f(0.2);
    const auto in = bump(dt, 0.5, n * dt, 20.0);
    const auto out = runFilter(f, in);
    REQUIRE(out.size() == in.size());
    CHECK(maxOf(out) == 1.0);
    ExcursionFilter g(0.2);
    CHECK(minOf(runFilter(g, bump(dt, 0.5, n * dt, -4.0))) == 1.0);
  }
  for (int n : {12, 13, 30}) {  // 12 samples = 0.2 s: a plateau of exactly W survives
    CAPTURE(n);
    ExcursionFilter f(0.2);
    const auto in = bump(dt, 0.5, n * dt, 12.0);
    const auto out = runFilter(f, in);
    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
      CHECK(out[i].t == in[i].t);  // original timestamps, original order
      CHECK(out[i].v == in[i].v);  // plateaus and steps preserved exactly
    }
  }
  // A step is preserved, and so is a slow ramp.
  std::vector<Sample> ramp;
  for (int i = 0; i < 120; ++i) ramp.push_back({i * dt, i < 60 ? 2.0 + 0.1 * i : 8.0});
  ExcursionFilter f(0.2);
  const auto out = runFilter(f, ramp);
  for (std::size_t i = 0; i < ramp.size(); ++i) CHECK(out[i].v == Approx(ramp[i].v));
}

TEST_CASE("excursion filter: uneven sample intervals, bounded delay, flush") {
  std::mt19937 rng(5);
  std::uniform_real_distribution<double> frame(1.0 / 144.0, 1.0 / 30.0);
  std::vector<Sample> in;
  for (double t = 0.0; t < 3.0; t += frame(rng)) {
    double v = 1.0;
    if (t >= 1.0 && t < 1.1) v = 15.0;  // 0.1 s: removed
    if (t >= 2.0 && t < 2.4) v = 9.0;   // 0.4 s: kept
    in.push_back({t, v});
  }
  ExcursionFilter f(0.2);
  std::vector<Sample> out;
  Sample s;
  double worstDelay = 0.0;
  for (const Sample& x : in) {
    f.push(x.t, x.v);
    while (f.pop(s)) out.push_back(s);
    // Everything older than 2 W (plus a frame per look-ahead stage) has come out.
    if (!out.empty()) worstDelay = std::max(worstDelay, x.t - out.back().t);
  }
  CHECK(worstDelay < 0.4 + 2.0 / 30.0 + 1e-9);  // each look-ahead stage waits for one more frame
  CHECK(f.pending() > 0);
  f.flush();
  CHECK(f.pending() == 0);
  while (f.pop(s)) out.push_back(s);
  REQUIRE(out.size() == in.size());
  CHECK(maxOf(out) == 9.0);
  for (std::size_t i = 0; i < in.size(); ++i) CHECK(out[i].t == in[i].t);

  // Memory stays bounded even if time stands still.
  ExcursionFilter stuck(0.2);
  for (int i = 0; i < 3 * static_cast<int>(ExcursionFilter::kMaxPending); ++i) stuck.push(1.0, 1.0);
  CHECK(stuck.pending() <= 2 * ExcursionFilter::kMaxPending);
}

TEST_CASE("mission: a 0.15 s 20 G bump inside a 5 G turn is not the mission max") {
  auto nz = [](double t) {
    if (t < 1.0) return 1.0;
    if (t < 1.5) return 1.0 + 8.0 * (t - 1.0);  // to 5 G
    if (t >= 3.0 && t < 3.15) return 20.0;
    return 5.0;
  };
  const MissionRun run = flyMission(nz, 5.0, 0.2);
  const MissionStats& s = run.report.stats;
  CHECK(run.liveMaxG > 15.0);  // the live G (HUD) still shows the bump
  CHECK(s.maxG == Approx(5.0).epsilon(0.01));
  CHECK(s.highGEpisodes == 0);  // threshold 6 G: the bump creates no episode
  CHECK(*std::max_element(run.report.trace.gMax.begin(), run.report.trace.gMax.end()) == Approx(5.0f).epsilon(0.01));
  CHECK(s.maxOnset < 20.0);  // the 5 G ramp is 8 G/s; the bump's edges (~±300 G/s) are gone

  // Filter off: the bump counts, exactly as the live G showed it.
  const MissionRun off = flyMission(nz, 5.0, 0.0);
  CHECK(off.report.stats.maxG == off.liveMaxG);
  CHECK(off.report.stats.maxG > 15.0);
  CHECK(off.report.stats.highGEpisodes == 1);
}

TEST_CASE("mission: a 0.3 s plateau at 12 G is kept, at the right mission time") {
  auto nz = [](double t) { return t >= 2.0 && t < 2.3 ? 12.0 : 1.0; };
  const MissionRun run = flyMission(nz, 4.0, 0.2);
  const MissionStats& s = run.report.stats;
  CHECK(s.maxG == Approx(12.0).epsilon(0.01));
  CHECK(s.maxGAtS > 2.0);
  CHECK(s.maxGAtS < 2.4);  // mission time = flight time here (the mission starts at t = 0)
  CHECK(s.highGEpisodes == 1);
  CHECK(s.longestHighGS == Approx(0.3).epsilon(0.15));
}

TEST_CASE("mission: a 0.1 s dip to -4 G in level flight is not the mission min") {
  auto nz = [](double t) { return t >= 2.0 && t < 2.1 ? -4.0 : 1.0; };
  const MissionRun run = flyMission(nz, 4.0, 0.2);
  CHECK(run.liveMinG < -2.0);
  CHECK(run.report.stats.minG == Approx(1.0).epsilon(0.01));
}

TEST_CASE("mission: the filter delay loses nothing at the mission end, with uneven frames") {
  // The last 0.3 s before the results screen are at 7 G: still inside the filter when the
  // mission ends, so only the end-of-mission flush can count them.
  for (bool jitter : {false, true}) {
    CAPTURE(jitter);
    auto nz = [](double t) { return t >= 2.7 ? 7.0 : 1.0; };
    const MissionRun on = flyMission(nz, 3.0, 0.2, jitter);
    const MissionRun off = flyMission(nz, 3.0, 0.0, jitter);
    const MissionStats& s = on.report.stats;
    CHECK(s.durationS == off.report.stats.durationS);  // mission time is not delayed
    CHECK(binSum(s) == Approx(binSum(off.report.stats)).epsilon(1e-9));
    CHECK(binSum(s) > s.durationS - 0.2);
    CHECK(s.maxG == Approx(7.0).epsilon(0.01));
    CHECK(s.maxGAtS > 2.7);
    CHECK(s.maxGAtS <= s.durationS);
    CHECK(s.highGEpisodes == 1);
    CHECK(on.report.trace.endS() >= s.durationS - 1e-9);  // the last bucket may be partial
    CHECK(on.report.trace.endS() < s.durationS + 1.0 / on.report.trace.hz);
    CHECK(on.report.trace.gMax.back() == Approx(7.0f).epsilon(0.01));
    CHECK(s.acceptedSamples == off.report.stats.acceptedSamples);
  }
}
