#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "sources/flight_model.h"
#include "sources/simulated_source.h"

using namespace ft;

namespace {

constexpr double kDt = 1.0 / 240.0;

// Lift axis rotated by `bankDeg` about +X (positive = right wing down) for a north heading.
FlightState bankedState(double bankDeg, double speed = 230.0) {
  FlightState s;
  s.velocity = {speed, 0.0, 0.0};
  const double b = bankDeg * kDegToRad;
  s.attitude.forward = {1.0, 0.0, 0.0};
  s.attitude.up = {0.0, std::sin(b), std::cos(b)};
  s.attitude.right = cross(s.attitude.up, s.attitude.forward);
  return s;
}

// Throttle that balances drag in level flight at the given load factor.
double balancedThrottle(const PointMassFlightModel& m, double nz) {
  const FlightModelParams& p = m.params();
  const double v = length(m.state().velocity), r = p.refSpeed / v;
  return (p.dragCoef * v * v + p.inducedDragCoef * nz * nz * r * r) / p.maxThrustG;
}

}  // namespace

TEST_CASE("flight model: level cruise gives ~1 G and holds altitude") {
  SimulatedSourceOptions opt;
  opt.scenario = Scenario::LevelCruise;
  opt.dirty.frameJitter = 0.0;
  SimulatedSource src(opt);
  std::vector<RawFlightSample> samples;
  std::vector<FlightState> truth;
  src.generate(20.0, samples, &truth);
  REQUIRE(truth.size() > 1000);
  for (const FlightState& s : truth) {
    CHECK(s.trueNz == doctest::Approx(1.0).epsilon(0.02));
    CHECK(std::abs(s.position.z - 3000.0) < 20.0);
  }
}

TEST_CASE("flight model: steady level turn at bank phi gives nz ~ 1/cos(phi)") {
  for (double bank : {45.0, 60.0, 75.0}) {
    PointMassFlightModel m;
    FlightState init = bankedState(bank);
    const double n = 1.0 / std::cos(bank * kDegToRad);
    init.appliedNz = n;
    m.reset(init);
    ControlInput in{n, 0.0, balancedThrottle(m, n)};
    for (int i = 0; i < 240 * 10; ++i) m.step(kDt, in);
    const FlightState& s = m.state();
    // Body nz is lift * cos(AoA) (+ tiny thrust-drag term); AoA is a few degrees.
    const double aoa = m.aoaDegFor(n, length(s.velocity)) * kDegToRad;
    CHECK(s.trueNz == doctest::Approx(n * std::cos(aoa)).epsilon(0.01));
    CHECK(s.trueNz == doctest::Approx(n).epsilon(0.04));
    CHECK(std::abs(s.position.z - init.position.z) < 5.0);  // lift balances gravity
    CHECK(std::abs(s.velocity.z) < 0.5);
    // Bank is preserved by parallel transport while the heading turns.
    CHECK(std::abs(m.liftDir().z - std::cos(bank * kDegToRad)) < 0.01);
  }
}

TEST_CASE("flight model: frame stays orthonormal through a loop and a 360 deg roll") {
  PointMassFlightModel m;
  FlightState init;
  init.velocity = {250.0, 0.0, 0.0};
  m.reset(init);
  double maxClimb = 0.0, minUpZ = 1.0;
  const ControlInput loop{5.0, 0.0, 1.0};
  for (int i = 0; i < 240 * 40; ++i) {
    m.step(kDt, loop);
    const FlightState& s = m.state();
    REQUIRE(s.attitude.isOrthonormal(1e-9));
    REQUIRE(std::abs(dot(m.liftDir(), normalized(s.velocity))) < 1e-9);
    maxClimb = std::max(maxClimb, normalized(s.velocity).z);
    minUpZ = std::min(minUpZ, s.attitude.up.z);
  }
  CHECK(maxClimb > 0.999);  // went through vertical
  CHECK(minUpZ < -0.8);     // and inverted over the top

  PointMassFlightModel r;
  r.reset(FlightState{});
  double minLiftZ = 1.0;
  const ControlInput roll{1.0, 90.0, 0.5};
  for (int i = 0; i < 240 * 4; ++i) {
    r.step(kDt, roll);
    REQUIRE(r.state().attitude.isOrthonormal(1e-9));
    minLiftZ = std::min(minLiftZ, r.liftDir().z);
  }
  CHECK(minLiftZ < -0.99);              // passed through inverted
  CHECK(r.liftDir().z > 0.95);          // and back upright after 360 deg
}

TEST_CASE("flight model: load factor onset respects the slew limit") {
  PointMassFlightModel m;
  m.reset(FlightState{});
  const double slew = m.params().nzSlewGPerS;
  double prev = m.state().appliedNz;
  const ControlInput pull{9.0, 0.0, 1.0};
  for (int i = 0; i < 60; ++i) {  // 0.25 s
    m.step(kDt, pull);
    CHECK(m.state().appliedNz - prev <= slew * kDt + 1e-12);
    prev = m.state().appliedNz;
  }
  CHECK(m.state().appliedNz == doctest::Approx(1.0 + slew * 0.25).epsilon(0.01));
  for (int i = 0; i < 240; ++i) m.step(kDt, pull);
  CHECK(m.state().appliedNz == doctest::Approx(9.0));
}

TEST_CASE("flight model: truth AoA follows the AoA law, sideslip is zero") {
  PointMassFlightModel m;
  const FlightModelParams& p = m.params();
  FlightState init;
  init.velocity = {180.0, 0.0, 0.0};
  init.appliedNz = 4.0;
  m.reset(init);
  const ControlInput in{4.0, 30.0, 0.8};
  for (int i = 0; i < 240; ++i) m.step(kDt, in);
  const FlightState& s = m.state();
  const double v = length(s.velocity);
  const double expected = p.aoaPerG * 4.0 * (p.refSpeed / v) * (p.refSpeed / v);
  CHECK(s.trueAoaDeg == doctest::Approx(expected).epsilon(1e-6));
  CHECK(std::abs(s.trueSideslipDeg) < 1e-6);
  // Clamped at low speed / high G, and negative under negative G.
  CHECK(m.aoaDegFor(9.0, 70.0) == doctest::Approx(p.maxAoaDeg));
  CHECK(m.aoaDegFor(-2.0, 200.0) == doctest::Approx(-2.0 * p.aoaPerG));
}

TEST_CASE("flight model: speed stays within limits") {
  PointMassFlightModel m;
  m.reset(FlightState{});
  for (int i = 0; i < 240 * 30; ++i) m.step(kDt, {9.0, 0.0, 0.0});  // idle, max pull
  CHECK(length(m.state().velocity) > m.params().minSpeed - 5.0);
}

// Validates the model independently of the core estimators: differentiating the emitted
// velocity must reproduce the reported specific force.
TEST_CASE("flight model: finite-difference of emitted velocity reproduces trueNz") {
  SimulatedSourceOptions opt;
  opt.scenario = Scenario::DogfightMix;
  opt.seed = 7;
  SimulatedSource src(opt);
  std::vector<RawFlightSample> samples;
  std::vector<FlightState> truth;
  src.generate(60.0, samples, &truth);
  REQUIRE(samples.size() == truth.size());

  double sumSq = 0.0, maxErr = 0.0, maxNz = -99.0, minNz = 99.0;
  int n = 0;
  for (std::size_t i = 1; i + 1 < samples.size(); ++i) {
    const double dt = samples[i + 1].t - samples[i - 1].t;
    const Vec3 a = (*samples[i + 1].velocity - *samples[i - 1].velocity) / dt;
    const Vec3 f = a - kGravityWorld;
    const double nz = dot(f, samples[i].attitude.up) / kG0;
    const double err = nz - truth[i].trueNz;
    sumSq += err * err;
    maxErr = std::max(maxErr, std::abs(err));
    maxNz = std::max(maxNz, truth[i].trueNz);
    minNz = std::min(minNz, truth[i].trueNz);
    ++n;

    // AoA from the emitted velocity/attitude with the core's convention matches exactly.
    const Vec3 vb = samples[i].attitude.toBody(*samples[i].velocity);
    CHECK(std::atan2(-vb.z, vb.x) * kRadToDeg == doctest::Approx(truth[i].trueAoaDeg).epsilon(1e-9));
  }
  const double rms = std::sqrt(sumSq / n);
  MESSAGE("finite-difference nz: rms " << rms << " max " << maxErr << " range [" << minNz << ", "
                                       << maxNz << "]");
  CHECK(rms < 0.03);
  CHECK(maxErr < 0.2);   // only at slew-rate corners
  CHECK(maxNz > 7.0);    // the scenario really is a dogfight
  CHECK(minNz < -0.5);
}
