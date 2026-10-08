// High-G timer, mission state machine, statistics, history, consumers and config.
#include <doctest/doctest.h>

#include <numeric>

#include "core/high_g_timer.h"
#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

namespace {
FlyOptions phase(MissionPhase ph) {
  FlyOptions o;
  o.phase = ph;
  return o;
}
}  // namespace

TEST_CASE("HighGTimer hysteresis") {
  HighGTimer h;
  h.setThresholds(6.0, 5.5);
  int entered = 0, exited = 0;
  for (double g : {5.7, 5.9, 6.1, 5.6, 5.9, 5.51, 6.3, 5.6, 5.4, 5.7, 5.9}) {
    const HighGTimer::Step s = h.update(g, 0.1);
    entered += s.entered ? 1 : 0;
    exited += s.exited ? 1 : 0;
  }
  CHECK(entered == 1);
  CHECK(exited == 1);
  CHECK_FALSE(h.active());
}

TEST_CASE("sustained high-G episodes: G hovering between exit and enter does not toggle") {
  GProfile prof([](double t) {
    if (t < 1.0) return 1.0;
    if (t < 1.5) return 1.0 + 11.0 * (t - 1.0);  // ramp to 6.5
    if (t < 2.0) return 6.5;
    if (t < 6.0) return 5.75 + 0.15 * std::sin(2.0 * kPi * 3.0 * t);  // 5.6 .. 5.9
    if (t < 7.0) return 1.0;
    return 5.7 + 0.25 * std::sin(2.0 * kPi * 2.0 * t);  // 5.45 .. 5.95, never >= 6
  });
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  const double t1 = fly(p, prof.trajectory(), 0.0, 4.0);
  // G crosses 6.0 at t = 1.45; the filtered value follows ~0.13 s later (LS window,
  // median and EMA lag), so the episode clock starts at ~1.58 s.
  CHECK(c.last.inHighG);
  CHECK(c.last.highGDurationS == Approx(4.0 - 1.58).epsilon(0.01));
  fly(p, prof.trajectory(), t1, 9.0);
  const MissionStats& s = p.snapshot().mission;
  CHECK(s.highGEpisodes == 1);
  CHECK(s.longestHighGS == Approx(6.0 - 1.58).epsilon(0.01));
  CHECK(s.totalHighGS == Approx(s.longestHighGS).epsilon(1e-6));
  CHECK_FALSE(c.last.inHighG);
  CHECK(c.last.highGDurationS == 0.0);
}

TEST_CASE("mission flow: start, pause excluded, one report, abort, ids") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  const Trajectory turn = levelTurn(60.0);

  double t = fly(p, turn, 0.0, 1.0, phase(MissionPhase::None));
  CHECK(c.started == 0);
  CHECK_FALSE(p.snapshot().mission.hasData);

  t = fly(p, turn, t, 4.0);
  CHECK(c.started == 1);
  // Paused with game time still advancing (worst case for a source): fully excluded.
  t = fly(p, turn, t, 6.0, phase(MissionPhase::Paused));
  t = fly(p, turn, t, 8.0);
  CHECK(p.snapshot().quality.resetsGap == 0);
  // Results screen for many frames, then menus, then results again: one report.
  t = fly(p, turn, t, 8.5, phase(MissionPhase::Ended));
  t = fly(p, levelFlight(), t, 9.0, phase(MissionPhase::None));
  t = fly(p, levelFlight(), t, 9.2, phase(MissionPhase::Ended));
  CHECK(c.ended == 1);

  MetricsSnapshot s = p.snapshot();
  REQUIRE(s.lastReport);
  const MissionReport& r = *s.lastReport;
  CHECK(r.id == 1);
  CHECK_FALSE(r.aborted);
  // 3 s + 2 s Active; the pause (and the resume step spanning it) is excluded.
  CHECK(r.stats.durationS == Approx(5.0).epsilon(0.01));
  CHECK(r.stats.durationS < 5.0 + 1e-9);
  CHECK(r.stats.maxG == Approx(2.0).epsilon(1e-3));
  CHECK(r.stats.maxGAtS <= r.stats.durationS);
  const double binSum = std::accumulate(r.stats.timeInGBinS.begin(), r.stats.timeInGBinS.end(), 0.0);
  // The histogram only covers valid samples: duration minus a few warm-up frames.
  CHECK(binSum <= r.stats.durationS + 1e-9);
  CHECK(binSum > r.stats.durationS - 0.2);
  CHECK(r.stats.timeInGBinS[gBinIndex(2.0)] == Approx(binSum));
  CHECK(r.timelineHz == 10.0);
  CHECK(std::abs(static_cast<double>(r.gTimeline.size()) - r.stats.durationS * 10.0) <= 2.0);
  CHECK(r.gTimeline.back() == Approx(2.0).epsilon(1e-3));
  CHECK(r.stats.acceptedSamples == 300);  // 5 s at 60 fps, Active only
  CHECK(r.stats.rejectedSamples == 0);
  CHECK(r.stats.maxSpeed == Approx(kSpeed));
  CHECK(s.mission.durationS == Approx(r.stats.durationS));  // live stats kept after end

  // Second mission, quit to menu without results screen.
  t = fly(p, turn, t, 10.0);
  CHECK(c.started == 2);
  CHECK(p.snapshot().mission.durationS < 1.0);  // live stats restarted
  fly(p, levelFlight(), t, 10.5, phase(MissionPhase::None));
  s = p.snapshot();
  CHECK(c.ended == 2);
  CHECK(s.lastReport->id == 2);
  CHECK(s.lastReport->aborted);
  CHECK(c.lastReportId == 2);
}

TEST_CASE("statistics accumulate only while Active") {
  GProfile prof([](double t) { return t < 2.0 ? 4.0 : 1.0; });
  TelemetryPipeline p;
  double t = fly(p, prof.trajectory(), 0.0, 2.0, phase(MissionPhase::None));  // 4 G in the menu
  CHECK(p.snapshot().current.g == Approx(4.0).epsilon(1e-3));
  t = fly(p, prof.trajectory(), t, 3.0, phase(MissionPhase::None));
  t = fly(p, prof.trajectory(), t, 4.0);
  const MissionStats s = p.snapshot().mission;
  CHECK(s.maxG == Approx(1.0).epsilon(1e-3));
  CHECK(s.maxOnset == Approx(0.0).scale(1.0).epsilon(1e-3));
  fly(p, prof.trajectory(), t, 4.5, phase(MissionPhase::Ended));
  CHECK(p.snapshot().lastReport->stats.durationS == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("consumer callbacks follow phases") {
  TelemetryPipeline p;
  RecordingConsumer c, removed;
  p.addConsumer(&c);
  p.addConsumer(&c);  // duplicate add is ignored
  p.addConsumer(&removed);
  p.removeConsumer(&removed);

  double t = fly(p, levelFlight(), 0.0, 0.5, phase(MissionPhase::None));
  CHECK(c.metricsNone == 30);
  t = fly(p, levelFlight(), t, 1.0);
  CHECK(c.metricsActive == 30);
  CHECK(c.started == 1);
  t = fly(p, levelFlight(), t, 1.5, phase(MissionPhase::Paused));
  CHECK(c.metrics == 60);  // nothing while Paused
  t = fly(p, levelFlight(), t, 2.0, phase(MissionPhase::Ended));
  CHECK(c.metricsEnded == 30);  // results screen keeps updating (effects can fade out)
  CHECK(c.metrics == 90);
  CHECK(c.ended == 1);
  CHECK(removed.metrics + removed.started + removed.ended == 0);

  p.reset();
  CHECK(c.resets == 1);
  CHECK(p.snapshot().lastReport);  // reset() keeps the last report
  CHECK_FALSE(p.snapshot().mission.hasData);
}

TEST_CASE("setConfig resets filters only on window changes and keeps mission stats") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  double t = fly(p, levelTurn(60.0), 0.0, 2.0);
  const MissionStats before = p.snapshot().mission;

  PipelineConfig cfg = p.config();
  cfg.highGEnter = 1.5;
  cfg.highGExit = 1.2;
  cfg.smoothingTauS = 0.2;
  p.setConfig(cfg);
  t = fly(p, levelTurn(60.0), t, t + 0.5 / 60.0);
  CHECK(c.last.valid);  // no reset
  CHECK(c.last.inHighG);

  cfg.medianWindow = 7;
  p.setConfig(cfg);
  t = fly(p, levelFlight(), t, t + 0.5 / 60.0);
  CHECK_FALSE(c.last.valid);  // resized -> restarted
  CHECK(p.config().medianWindow == 7);
  const MissionStats after = p.snapshot().mission;
  CHECK(after.maxG == Approx(before.maxG));
  CHECK(after.durationS >= before.durationS);
  CHECK(after.acceptedSamples == before.acceptedSamples + 2);
}

TEST_CASE("recent history is decimated by time, not by sample count") {
  for (double fps : {30.0, 60.0, 144.0, 500.0}) {
    CAPTURE(fps);
    FlyOptions o;
    o.fps = fps;
    TelemetryPipeline p;
    fly(p, levelFlight(), 0.0, 2.0, o);
    const RecentHistory& h = p.snapshot().recent;
    CHECK(std::abs(h.count - 40) <= 2);
    CHECK(h.at(h.count - 1) == Approx(1.0f));
  }
}

TEST_CASE("changing the G mode mid-mission restarts the G statistics only") {
  // Inverted level flight: Nz = -1, |n| = +1. Mixing them would give min -1 / max +1.
  const Trajectory inverted = levelFlight(Basis::fromUERotator(0, 0, 180));
  TelemetryPipeline p;
  double t = fly(p, inverted, 0.0, 2.0);
  MissionStats s = p.snapshot().mission;
  CHECK(s.minG == Approx(-1.0).epsilon(1e-6));
  CHECK(s.timeInGBinS[gBinIndex(-1.0)] > 1.9);

  PipelineConfig cfg = p.config();
  cfg.gMode = GMode::Total;
  p.setConfig(cfg);
  CHECK(p.snapshot().recent.count == 0);
  CHECK_FALSE(p.snapshot().mission.hasGData);  // UI shows "--", not a bogus 0.0
  CHECK(p.snapshot().mission.hasData);
  t = fly(p, inverted, t, 4.0);
  CHECK(p.snapshot().mission.hasGData);
  fly(p, inverted, t, 4.5, phase(MissionPhase::Ended));

  REQUIRE(p.snapshot().lastReport);
  const MissionReport& r = *p.snapshot().lastReport;
  s = r.stats;
  CHECK(s.minG == Approx(1.0).epsilon(1e-6));
  CHECK(s.maxG == Approx(1.0).epsilon(1e-6));
  CHECK(s.minGAtS > r.timelineStartS);
  CHECK(s.timeInGBinS[gBinIndex(-1.0)] == 0.0);
  CHECK(s.timeInGBinS[gBinIndex(1.0)] == Approx(2.0).epsilon(0.05));
  // Kept: flight time, speed, sample counts.
  CHECK(s.durationS == Approx(4.0).epsilon(0.03));
  CHECK(s.maxSpeed == Approx(kSpeed));
  CHECK(s.acceptedSamples == 240);
  // The timeline restarts where the mode changed.
  CHECK(r.timelineStartS == Approx(2.0).epsilon(0.03));
  CHECK(std::abs(static_cast<double>(r.gTimeline.size()) - (s.durationS - r.timelineStartS) * 10.0) <= 2.0);
  CHECK(r.gTimeline.front() == Approx(1.0f));
}
