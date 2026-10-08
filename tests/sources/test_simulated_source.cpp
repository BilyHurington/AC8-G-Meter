#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sources/simulated_source.h"

using namespace ft;

namespace {

// Note: MissionPhase comparisons use CHECK((a == b)) because ADL would find ft::toString
// (returning const char*) when doctest tries to print the operands.

SimulatedSourceOptions cleanOptions(Scenario scenario = Scenario::LevelCruise) {
  SimulatedSourceOptions opt;
  opt.scenario = scenario;
  opt.dirty.frameJitter = 0.0;
  return opt;
}

double maxGap(const std::vector<RawFlightSample>& s) {
  double gap = 0.0;
  for (std::size_t i = 1; i < s.size(); ++i) gap = std::max(gap, s[i].t - s[i - 1].t);
  return gap;
}

}  // namespace

TEST_CASE("simulated source: frame rate and jitter") {
  SimulatedSourceOptions opt;
  opt.frameRateHz = 60.0;
  opt.dirty.frameJitter = 0.15;
  SimulatedSource src(opt);
  std::vector<RawFlightSample> s;
  src.generate(10.0, s);
  CHECK(s.size() > 570);
  CHECK(s.size() < 630);
  double minDt = 1.0, maxDt = 0.0;
  for (std::size_t i = 1; i < s.size(); ++i) {
    const double dt = s[i].t - s[i - 1].t;
    minDt = std::min(minDt, dt);
    maxDt = std::max(maxDt, dt);
  }
  CHECK(minDt >= 0.85 / 60.0 - 1e-9);
  CHECK(maxDt <= 1.15 / 60.0 + 1e-9);
  CHECK(maxDt - minDt > 0.2 / 60.0);  // jitter actually happens
  CHECK(src.truth().t == doctest::Approx(10.0));  // generate() advances exactly
}

TEST_CASE("simulated source: same seed is deterministic, different seed differs") {
  SimulatedSourceOptions opt;
  opt.scenario = Scenario::DogfightMix;
  opt.dirty.positionNoiseM = 0.1;
  opt.dirty.dropProbability = 0.05;
  std::vector<RawFlightSample> a, b, c;
  SimulatedSource(opt).generate(20.0, a);
  SimulatedSource(opt).generate(20.0, b);
  opt.seed += 1;
  SimulatedSource(opt).generate(20.0, c);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i].t == b[i].t);
    REQUIRE(a[i].position == b[i].position);
    REQUIRE(a[i].attitude.up == b[i].attitude.up);
  }
  CHECK((a.size() != c.size() || a.back().position != c.back().position));
}

TEST_CASE("simulated source: pause freezes game time and reports Paused") {
  SimulatedSource src(cleanOptions());
  std::vector<RawFlightSample> out;
  for (int i = 0; i < 30; ++i) src.poll(1.0 / 60.0, out);
  REQUIRE(!out.empty());
  const double tPause = out.back().t;

  src.setPaused(true);
  out.clear();
  for (int i = 0; i < 30; ++i) src.poll(1.0 / 60.0, out);
  CHECK(out.size() == 30);
  for (const RawFlightSample& s : out) {
    CHECK((s.phase == MissionPhase::Paused));
    CHECK(s.t == tPause);
  }
  CHECK((src.missionPhase() == MissionPhase::Active));

  src.setPaused(false);
  out.clear();
  for (int i = 0; i < 10; ++i) src.poll(1.0 / 60.0, out);
  REQUIRE(!out.empty());
  CHECK((out.front().phase == MissionPhase::Active));
  CHECK(out.front().t - tPause < 1.5 / 60.0);  // resumes where it stopped, no catch-up burst
}

TEST_CASE("simulated source: time scale and catch-up cap") {
  for (double scale : {0.5, 2.0}) {
    SimulatedSource src(cleanOptions());
    src.setTimeScale(scale);
    std::vector<RawFlightSample> out;
    for (int i = 0; i < 120; ++i) src.poll(1.0 / 60.0, out);  // 2 s of real time
    CHECK(src.truth().t == doctest::Approx(2.0 * scale).epsilon(0.02));
  }
  SimulatedSource src(cleanOptions());
  std::vector<RawFlightSample> out;
  src.poll(10.0, out);  // host stalled for 10 s
  CHECK(src.truth().t <= 0.25 + 1e-9);
  CHECK(out.size() <= 16);
}

TEST_CASE("simulated source: hitch, teleport and spike injection") {
  SimulatedSource src(cleanOptions());
  std::vector<RawFlightSample> s;
  std::vector<FlightState> truth;
  src.generate(1.0, s, &truth);

  SUBCASE("hitch leaves a gap in game time") {
    src.injectHitch(0.5);
    src.generate(1.0, s, &truth);
    CHECK(maxGap(s) == doctest::Approx(0.5 + 1.0 / 60.0).epsilon(0.05));
  }
  SUBCASE("teleport jumps position but not velocity, and stays consistent") {
    const std::size_t before = s.size();
    src.injectTeleport();
    src.generate(1.0, s, &truth);
    const RawFlightSample& a = s[before - 1];
    const RawFlightSample& b = s[before];
    CHECK(length(b.position - a.position) > 3000.0);
    CHECK(length(*b.velocity - *a.velocity) < 1.0);
    for (std::size_t i = before; i < s.size(); ++i) CHECK(s[i].position == truth[i].position);
  }
  SUBCASE("spike corrupts exactly one sample") {
    const std::size_t before = s.size();
    src.injectSpike();
    src.generate(1.0, s, &truth);
    int corrupted = 0;
    for (std::size_t i = before; i < s.size(); ++i)
      if (length(*s[i].velocity - truth[i].velocity) > 250.0) ++corrupted;
    CHECK(corrupted == 1);
    CHECK(length(*s[before].velocity - truth[before].velocity) > 250.0);
  }
}

TEST_CASE("simulated source: dirty data drops, duplicates and noise") {
  SimulatedSourceOptions opt = cleanOptions();
  opt.dirty.dropProbability = 0.3;
  opt.dirty.duplicateProbability = 0.1;
  opt.dirty.positionNoiseM = 0.5;
  opt.provideVelocity = false;
  SimulatedSource src(opt);
  std::vector<RawFlightSample> s;
  std::vector<FlightState> truth;
  src.generate(10.0, s, &truth);
  int duplicates = 0;
  for (std::size_t i = 1; i < s.size(); ++i) duplicates += s[i].t == s[i - 1].t ? 1 : 0;
  CHECK(duplicates > 20);
  CHECK(s.size() < 600 * 0.9);
  double sumSq = 0.0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    CHECK(!s[i].velocity.has_value());
    sumSq += dot(s[i].position - truth[i].position, s[i].position - truth[i].position);
  }
  const double sigma = std::sqrt(sumSq / (3.0 * static_cast<double>(s.size())));
  CHECK(sigma == doctest::Approx(0.5).epsilon(0.15));
}

TEST_CASE("simulated source: mission phase control") {
  SimulatedSource src(cleanOptions());
  std::vector<RawFlightSample> s;
  src.quitToMenu();
  src.generate(0.1, s);
  CHECK((s.back().phase == MissionPhase::None));
  src.startMission();
  src.generate(0.1, s);
  CHECK((s.back().phase == MissionPhase::Active));
  src.endMission();
  src.generate(0.1, s);
  CHECK((s.back().phase == MissionPhase::Ended));
  CHECK((src.missionPhase() == MissionPhase::Ended));
}

TEST_CASE("simulated source: scripted scenarios stay sane and hit their targets") {
  for (Scenario sc : {Scenario::LevelCruise, Scenario::SustainedTurn, Scenario::PullUpPushOver,
                      Scenario::DogfightMix}) {
    const std::string name = scenarioName(sc);
    CAPTURE(name);
    SimulatedSourceOptions opt = cleanOptions(sc);
    opt.seed = 99;
    SimulatedSource src(opt);
    std::vector<RawFlightSample> s;
    std::vector<FlightState> truth;
    src.generate(120.0, s, &truth);
    double minAlt = 1e9, maxAlt = -1e9, maxNz = -99.0, minNz = 99.0, highGRun = 0.0, longest = 0.0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
      const FlightState& f = truth[i];
      minAlt = std::min(minAlt, f.position.z);
      maxAlt = std::max(maxAlt, f.position.z);
      maxNz = std::max(maxNz, f.trueNz);
      minNz = std::min(minNz, f.trueNz);
      highGRun = f.trueNz > 6.5 && i > 0 ? highGRun + (f.t - truth[i - 1].t) : 0.0;
      longest = std::max(longest, highGRun);
      CHECK(length(f.velocity) > 55.0);
    }
    MESSAGE(name << ": alt [" << minAlt << ", " << maxAlt << "] nz [" << minNz << ", "
                             << maxNz << "] longest >6.5G " << longest);
    CHECK(minAlt > 500.0);
    CHECK(maxAlt < 9500.0);
    switch (sc) {
      case Scenario::LevelCruise: CHECK(maxNz < 1.1); CHECK(minNz > 0.9); break;
      case Scenario::SustainedTurn: CHECK(longest > 7.0); CHECK(maxNz < 7.3); break;
      case Scenario::PullUpPushOver: CHECK(maxNz > 4.8); CHECK(minNz < -1.8); break;
      default: CHECK(maxNz > 8.0); CHECK(minNz < -1.0); break;
    }
  }
}
