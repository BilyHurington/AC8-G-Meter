#pragma once
// Analytic / numerically integrated flight trajectories for pipeline tests.
// Deliberately independent of src/sources so core tests only depend on core.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <ostream>  // doctest prints shared_ptr results via std::operator<<, which needs the complete ostream
#include <random>
#include <utility>

#include "core/pipeline.h"

namespace ft::test {

struct State {
  Vec3 position;
  Vec3 velocity;
  Basis attitude;
};
using Trajectory = std::function<State(double t)>;

struct FlyOptions {
  double fps = 60.0;
  bool positionOnly = false;      // omit velocity -> pipeline differentiates positions
  bool jitter = false;            // random frame time in [1/144, 1/30] s
  MissionPhase phase = MissionPhase::Active;
};

inline RawFlightSample sampleAt(const Trajectory& traj, double t, const FlyOptions& o = {}) {
  const State s = traj(t);
  RawFlightSample r;
  r.t = t;
  r.position = s.position;
  if (!o.positionOnly) r.velocity = s.velocity;
  r.attitude = s.attitude;
  r.phase = o.phase;
  return r;
}

// Pushes samples of `traj` for t in [t0, t1). Returns the time of the next sample so
// consecutive calls continue seamlessly.
inline double fly(TelemetryPipeline& p, const Trajectory& traj, double t0, double t1,
                  const FlyOptions& o = {}, std::mt19937* rng = nullptr) {
  std::uniform_real_distribution<double> dist(1.0 / 144.0, 1.0 / 30.0);
  // Fixed rate uses t0 + i/fps (no accumulated round-off, exact sample counts).
  double t = t0;
  for (int i = 1; t < t1 - 1e-9; ++i) {
    p.push(sampleAt(traj, t, o));
    t = (o.jitter && rng) ? t + dist(*rng) : t0 + i / o.fps;
  }
  return t;
}

inline constexpr double kSpeed = 200.0;  // m/s

// Straight and level along +X (optionally inverted / with a fixed attitude).
inline Trajectory levelFlight(Basis attitude = {}) {
  return [attitude](double t) {
    return State{{kSpeed * t, 0.0, 3000.0}, {kSpeed, 0.0, 0.0}, attitude};
  };
}

// Constant-speed coordinated level turn to the right at the given bank angle.
// Centripetal acceleration g*tan(bank) => Nz = 1/cos(bank).
inline Trajectory levelTurn(double bankDeg) {
  const double ac = kG0 * std::tan(bankDeg * kDegToRad);
  const double r = kSpeed * kSpeed / ac;
  const double w = kSpeed / r;
  return [=](double t) {
    const double psi = w * t;
    return State{{r * std::sin(psi), r * (1.0 - std::cos(psi)), 3000.0},
                 {kSpeed * std::cos(psi), kSpeed * std::sin(psi), 0.0},
                 Basis::fromUERotator(0.0, psi * kRadToDeg, bankDeg)};
  };
}

// Constant-speed inside loop in the X-Z plane, starting at the bottom.
// theta = pitch = w t; Nz = V^2 / (R g0) + cos(theta).
inline Trajectory verticalLoop(double radius) {
  const double w = kSpeed / radius;
  return [=](double t) {
    const double th = w * t;
    return State{{radius * std::sin(th), 0.0, 3000.0 + radius * (1.0 - std::cos(th))},
                 {kSpeed * std::cos(th), 0.0, kSpeed * std::sin(th)},
                 Basis::fromUERotator(th * kRadToDeg, 0.0, 0.0)};
  };
}
inline double loopNz(double radius, double t) {
  return kSpeed * kSpeed / (radius * kG0) + std::cos(kSpeed / radius * t);
}

// Level attitude, forward speed kSpeed, vertical acceleration chosen so that Nz(t)
// follows `nz` exactly. Velocity/position are integrated numerically with fine
// substeps; queries must be made with non-decreasing t.
class GProfile {
 public:
  explicit GProfile(std::function<double(double)> nz) : nz_(std::move(nz)) {}

  State at(double t) {
    constexpr double kStep = 1e-4;
    while (t_ < t) {
      const double h = std::min(kStep, t - t_);
      const double a = (nz_(t_ + 0.5 * h) - 1.0) * kG0;  // midpoint rule
      z_ += vz_ * h + 0.5 * a * h * h;
      vz_ += a * h;
      t_ += h;
    }
    return {{kSpeed * t, 0.0, 3000.0 + z_}, {kSpeed, 0.0, vz_}, Basis{}};
  }
  Trajectory trajectory() {
    return [this](double t) { return at(t); };
  }

 private:
  std::function<double(double)> nz_;
  double t_ = 0.0, z_ = 0.0, vz_ = 0.0;
};

// Barrel-roll style "rolling pull": the specific force has constant magnitude nG and
// rotates about the flight direction at rollRate (rad/s); the attitude keeps it in the
// body forward-up plane, so the true side force Ny is exactly 0. Speed ~kSpeed.
inline Trajectory rollingPull(double nG, double rollRate) {
  const double a = nG * kG0, w = rollRate, r = a / (w * w);
  return [=](double t) {
    const double c = std::cos(w * t), s = std::sin(w * t);
    const Vec3 vel{kSpeed, a / w * s, -a / w * c - kG0 * t};
    const Vec3 force{0.0, a * c, a * s};  // specific force = acceleration - gravity
    Basis b;
    b.forward = normalized(vel);
    b.up = normalized(force - b.forward * dot(force, b.forward));
    b.right = cross(b.up, b.forward);
    return State{{kSpeed * t, -r * c, 3000.0 - r * s - 0.5 * kG0 * t * t}, vel, b};
  };
}
// True Nz of rollingPull at time t (the part of the specific force normal to the path).
inline double rollingPullNz(const Trajectory& traj, double nG, double rollRate, double t) {
  const Vec3 force{0.0, nG * kG0 * std::cos(rollRate * t), nG * kG0 * std::sin(rollRate * t)};
  return dot(force, traj(t).attitude.up) / kG0;
}

// Deterministic gaussian noise (Box-Muller on mt19937, whose output is fully specified;
// std::normal_distribution's is implementation-defined).
struct Gaussian {
  std::mt19937 rng;
  explicit Gaussian(std::uint32_t seed) : rng(seed) {}
  double operator()(double sigma) {
    const double u1 = (static_cast<double>(rng()) + 1.0) / 4294967297.0;
    const double u2 = static_cast<double>(rng()) / 4294967296.0;
    return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
  }
  Vec3 vec(double sigma) { return {(*this)(sigma), (*this)(sigma), (*this)(sigma)}; }
};

inline RawFlightSample levelSample(double t, MissionPhase phase = MissionPhase::Active) {
  RawFlightSample r;
  r.t = t;
  r.position = {kSpeed * t, 0.0, 3000.0};
  r.velocity = Vec3{kSpeed, 0.0, 0.0};
  r.phase = phase;
  return r;
}

// Records every callback.
struct RecordingConsumer : IMetricsConsumer {
  int metrics = 0, metricsActive = 0, metricsNone = 0, metricsEnded = 0;
  int started = 0, ended = 0, resets = 0;
  std::uint32_t lastReportId = 0;
  FlightMetrics last;
  void onMetrics(const FlightMetrics& m, MissionPhase phase) override {
    ++metrics;
    if (phase == MissionPhase::Active) ++metricsActive;
    if (phase == MissionPhase::None) ++metricsNone;
    if (phase == MissionPhase::Ended) ++metricsEnded;
    last = m;
  }
  void onMissionStarted() override { ++started; }
  void onMissionEnded(const std::shared_ptr<const MissionReport>& r) override {
    ++ended;
    lastReportId = r->id;
  }
  void onReset() override { ++resets; }
};

}  // namespace ft::test
