// Pipeline accuracy on analytic flight paths: load factor, onset rate, air data.
#include <doctest/doctest.h>

#include <random>

#include "flight_helpers.h"

using namespace ft;
using namespace ft::test;
using doctest::Approx;

TEST_CASE("level flight reads 1 G") {
  TelemetryPipeline p;
  fly(p, levelFlight(), 0.0, 1.0);
  const FlightMetrics m = p.snapshot().current;
  REQUIRE(m.valid);
  CHECK(m.g == Approx(1.0).epsilon(1e-6));
  CHECK(m.gRaw == Approx(1.0).epsilon(1e-6));
  CHECK(m.gTotal == Approx(1.0).epsilon(1e-6));
  CHECK(m.loadFactorBody.x == Approx(0.0).scale(1.0).epsilon(1e-6));
  CHECK(m.loadFactorBody.z == Approx(1.0).epsilon(1e-6));
  CHECK(m.onsetRate == Approx(0.0).scale(1.0).epsilon(1e-6));
  CHECK(m.speed == Approx(kSpeed));
  CHECK(m.altitude == Approx(3000.0));
}

TEST_CASE("estimates become valid after a few samples") {
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  int firstValid = -1;
  for (int i = 0; i < 20; ++i) {
    p.push(levelSample(i / 60.0));
    if (firstValid < 0 && c.last.valid) firstValid = i;
  }
  CHECK(firstValid >= 2);
  CHECK(firstValid <= 6);
}

TEST_CASE("inverted level flight reads -1 G (Nz) and +1 G (Total)") {
  TelemetryPipeline p;
  fly(p, levelFlight(Basis::fromUERotator(0, 0, 180)), 0.0, 1.0);
  CHECK(p.snapshot().current.g == Approx(-1.0).epsilon(1e-6));

  PipelineConfig cfg;
  cfg.gMode = GMode::Total;
  TelemetryPipeline total(cfg);
  fly(total, levelFlight(Basis::fromUERotator(0, 0, 180)), 0.0, 1.0);
  CHECK(total.snapshot().current.g == Approx(1.0).epsilon(1e-6));
}

TEST_CASE("free fall reads 0 G") {
  TelemetryPipeline p;
  auto fall = [](double t) {
    return State{{kSpeed * t, 0.0, 5000.0 - 0.5 * kG0 * t * t}, {kSpeed, 0.0, -kG0 * t}, Basis{}};
  };
  fly(p, fall, 0.0, 2.0);
  const FlightMetrics m = p.snapshot().current;
  CHECK(m.g == Approx(0.0).scale(1.0).epsilon(1e-6));
  CHECK(m.gTotal == Approx(0.0).scale(1.0).epsilon(1e-6));
}

TEST_CASE("coordinated 60 degree level turn reads 2 G with no side force") {
  TelemetryPipeline p;
  fly(p, levelTurn(60.0), 0.0, 3.0);
  const FlightMetrics m = p.snapshot().current;
  CHECK(m.g == Approx(2.0).epsilon(1e-3));
  CHECK(m.loadFactorBody.y == Approx(0.0).scale(1.0).epsilon(0.01));
  CHECK(m.gTotal == Approx(2.0).epsilon(1e-3));
  CHECK(m.aoaDeg == Approx(0.0).scale(1.0).epsilon(1e-6));

  PipelineConfig cfg;
  cfg.gMode = GMode::Total;
  TelemetryPipeline total(cfg);
  fly(total, levelTurn(60.0), 0.0, 3.0);
  CHECK(total.snapshot().current.g == Approx(2.0).epsilon(1e-3));
}

TEST_CASE("constant-speed vertical loop follows Nz = V^2/(R g) + cos(theta)") {
  const double radius = 1000.0;  // ~5.1 G at the bottom, ~3.1 G over the top
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  const double period = 2.0 * kPi * radius / kSpeed;
  double maxErr = 0.0;
  for (double t = 0.0; t < period; t += 1.0 / 60.0) {
    p.push(sampleAt(verticalLoop(radius), t));
    if (t > 0.5) maxErr = std::max(maxErr, std::abs(c.last.g - loopNz(radius, t)));
  }
  // Remaining error is filter lag (~0.15 s) times dNz/dt (<= 0.2 G/s).
  CHECK(maxErr < 0.05);
  const MissionStats& s = p.snapshot().mission;
  CHECK(s.maxG == Approx(loopNz(radius, 0.0)).epsilon(0.01));
  CHECK(s.minG == Approx(loopNz(radius, period / 2)).epsilon(0.01));
}

TEST_CASE("position-only input matches velocity input") {
  FlyOptions posOnly;
  posOnly.positionOnly = true;

  TelemetryPipeline turnV, turnP;
  fly(turnV, levelTurn(60.0), 0.0, 3.0);
  fly(turnP, levelTurn(60.0), 0.0, 3.0, posOnly);
  CHECK(turnP.snapshot().current.g == Approx(turnV.snapshot().current.g).epsilon(0.005));
  CHECK(turnP.snapshot().current.speed == Approx(kSpeed).epsilon(1e-3));

  TelemetryPipeline loopV, loopP;
  fly(loopV, verticalLoop(1000.0), 0.0, 10.0);
  fly(loopP, verticalLoop(1000.0), 0.0, 10.0, posOnly);
  CHECK(loopP.snapshot().current.g == Approx(loopV.snapshot().current.g).epsilon(0.005));
  CHECK(loopP.snapshot().mission.maxG == Approx(loopV.snapshot().mission.maxG).epsilon(0.005));
}

TEST_CASE("uneven frame time (30-144 fps jitter) stays accurate") {
  std::mt19937 rng(42);
  FlyOptions opts;
  opts.jitter = true;
  std::uniform_real_distribution<double> frame(1.0 / 144.0, 1.0 / 30.0);
  for (bool positionOnly : {false, true}) {
    CAPTURE(positionOnly);
    opts.positionOnly = positionOnly;
    TelemetryPipeline p;
    RecordingConsumer c;
    p.addConsumer(&c);
    double maxErr = 0.0;
    for (double t = 0.0; t < 4.0; t += frame(rng)) {
      p.push(sampleAt(levelTurn(60.0), t, opts));
      if (t > 0.5) maxErr = std::max(maxErr, std::abs(c.last.g - 2.0));
    }
    CHECK(maxErr < 0.01);
    CHECK(p.snapshot().quality.rejectedImplausible == 0);
  }
}

TEST_CASE("onset rate of a linear 1 -> 7 G ramp over 1.5 s is 4 G/s") {
  GProfile prof([](double t) { return t < 1.0 ? 1.0 : t < 2.5 ? 1.0 + 4.0 * (t - 1.0) : 7.0; });
  TelemetryPipeline p;
  RecordingConsumer c;
  p.addConsumer(&c);
  const double next = fly(p, prof.trajectory(), 0.0, 2.2);
  // Mid-ramp: the EMA lags but a lagged ramp has the same slope.
  CHECK(c.last.onsetRate == Approx(4.0).epsilon(0.03));
  CHECK(c.last.g == Approx(1.0 + 4.0 * (c.last.t - 1.0)).epsilon(0.1));
  fly(p, prof.trajectory(), next, 4.0);
  CHECK(c.last.g == Approx(7.0).epsilon(1e-3));
  CHECK(c.last.onsetRate == Approx(0.0).scale(1.0).epsilon(1e-3));
  CHECK(p.snapshot().mission.maxOnset == Approx(4.0).epsilon(0.03));
}

TEST_CASE("angle of attack and sideslip signs and values") {
  const double alpha = 6.0, beta = 3.0;
  const Basis att = Basis::fromUERotator(10.0, 30.0, 20.0);
  const double a = alpha * kDegToRad, b = beta * kDegToRad;
  // Body velocity below the nose (alpha > 0) and to the right (beta > 0).
  const Vec3 vBody = Vec3{std::cos(a) * std::cos(b), std::sin(b), -std::sin(a) * std::cos(b)} * kSpeed;
  const Vec3 vWorld = att.toWorld(vBody);
  TelemetryPipeline p;
  fly(p, [&](double t) { return State{vWorld * t, vWorld, att}; }, 0.0, 0.5);
  const FlightMetrics m = p.snapshot().current;
  REQUIRE(m.aoaValid);
  CHECK(m.aoaDeg == Approx(alpha).epsilon(1e-9));
  CHECK(m.sideslipDeg == Approx(beta).epsilon(1e-9));

  // Mirror: velocity above the nose and to the left.
  const Vec3 vMirror = att.toWorld({vBody.x, -vBody.y, -vBody.z});
  TelemetryPipeline q;
  fly(q, [&](double t) { return State{vMirror * t, vMirror, att}; }, 0.0, 0.5);
  CHECK(q.snapshot().current.aoaDeg == Approx(-alpha).epsilon(1e-9));
  CHECK(q.snapshot().current.sideslipDeg == Approx(-beta).epsilon(1e-9));
}

TEST_CASE("AoA is invalid below minSpeedForAoa and never enters mission max") {
  TelemetryPipeline p;
  const Vec3 v{8.0, 0.0, -6.0};  // 10 m/s, 36.9 deg "AoA"
  fly(p, [&](double t) { return State{v * t, v, Basis{}}; }, 0.0, 1.0);
  const MetricsSnapshot s = p.snapshot();
  CHECK(s.current.valid);
  CHECK_FALSE(s.current.aoaValid);
  CHECK(s.mission.maxAoaDeg == 0.0);
}
