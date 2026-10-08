#include "core/gated_kinematics.h"

#include <algorithm>
#include <cmath>

#include "core/sanitizer.h"

namespace ft {

FilterState::Input GatedKinematics::inputKind() const {
  if (!hasLast_) return FilterState::Input::None;
  return lastVel_ ? FilterState::Input::Velocity : FilterState::Input::PositionOnly;
}

void GatedKinematics::recordLast(const RawFlightSample& s) {
  hasLast_ = true;
  lastT_ = s.t;
  lastPos_ = s.position;
  lastVel_ = s.velocity;
}

void GatedKinematics::restart() {
  kinematics_.reset();
  attitudes_.reset();
  gate_.clear();
}

void GatedKinematics::setWindows(const FilterState& f, const PipelineConfig& cfg) {
  const bool velocityInput = f.input != FilterState::Input::PositionOnly;
  // Adaptive: keep the samples of the longest window, so that a raise applies at once.
  kinematics_.setWindows(velocityInput ? f.accelWindowS : cfg.accelWindowS, cfg.accelWindowPositionOnlyS,
                         f.adaptive ? cfg.adaptiveMaxWindowS : 0.0);
}

bool GatedKinematics::isTeleport(const RawFlightSample& s, double dt, double toleranceM) const {
  if (!s.velocity) {
    if (const std::optional<Vec3> predicted = kinematics_.predictPosition(s.t))
      return length(s.position - *predicted) > toleranceM;
  }
  return ft::isTeleport(lastPos_, lastVel_, s, dt, toleranceM);
}

SpikeGate::Verdict GatedKinematics::check(const RawFlightSample& s, double dt, double spikeDeviationG,
                                          const NoiseEstimator& noise) {
  const SpikeGate::Verdict verdict = gate_.check(s, dt, kinematics_, spikeDeviationG, noise.sigma(), noise.ready());
  if (verdict == SpikeGate::Verdict::DropPrevious) {
    // The newest window samples, held by the gate, were a glitch.
    const int count = gate_.resolved();
    kinematics_.removeNewest(count);
    for (int i = 0; i < count; ++i) attitudes_.popNewest();
  }
  return verdict;
}

bool GatedKinematics::noiseSuspected(const RawFlightSample& s, SpikeGate::Verdict verdict) const {
  const bool scattered = verdict == SpikeGate::Verdict::Noise ||
                         (verdict == SpikeGate::Verdict::Reject && gate_.runScatters());
  return s.velocity && (scattered || gate_.busy());
}

void GatedKinematics::startFromRun() {
  const auto& run = gate_.run();
  for (std::size_t i = 0; i + 1 < run.size(); ++i) add(run[i], SpikeGate::Verdict::Accept);
}

void GatedKinematics::add(const RawFlightSample& s, SpikeGate::Verdict verdict) {
  if (verdict == SpikeGate::Verdict::Confirm) kinematics_.commitHeld(gate_.resolved());
  if (verdict == SpikeGate::Verdict::Hold || verdict == SpikeGate::Verdict::Confirm)
    kinematics_.addHeld(s.t, *s.velocity);
  else
    kinematics_.add(s.t, s.position, s.velocity);
  attitudes_.push(s.t, s.attitude);
  attitudes_.trimBefore(kinematics_.oldestTime());
}

std::optional<Vec3> GatedKinematics::loadFactorBody() const {
  if (!kinematics_.hasAcceleration()) return std::nullopt;
  const Vec3 n = (kinematics_.acceleration() - kGravityWorld) / kG0;
  return attitudeAt(kinematics_.accelerationTime()).toBody(n);
}

// The acceleration is a window average that refers to ~windowS / 2 ago; projecting it on the
// newest attitude would turn every roll into spurious side force (Ny). Linear interpolation of
// the axes + re-orthonormalisation is plenty for frame-to-frame rotations; if the step is too
// large for that, the nearer sample is used.
Basis GatedKinematics::attitudeAt(double t) const {
  const int n = attitudes_.size();
  if (n == 1) return attitudes_.value(0);
  int i = 1;
  while (i < n - 1 && attitudes_.time(i) < t) ++i;
  const double t0 = attitudes_.time(i - 1), t1 = attitudes_.time(i);
  const double w = std::clamp((t - t0) / (t1 - t0), 0.0, 1.0);
  const Basis& a = attitudes_.value(i - 1);
  const Basis& b = attitudes_.value(i);
  Basis lerp{a.forward + (b.forward - a.forward) * w, a.right + (b.right - a.right) * w, a.up + (b.up - a.up) * w};
  if (!sanitizeAttitude(lerp)) return w < 0.5 ? a : b;
  return lerp;
}

void GatedKinematics::airData(const RawFlightSample& s, double minSpeedForAoa, FlightMetrics& m) const {
  const Vec3 v = kinematics_.hasVelocity() ? kinematics_.velocity() : Vec3{};
  m.speed = length(v);
  m.aoaValid = kinematics_.hasVelocity() && m.speed >= minSpeedForAoa;
  if (!m.aoaValid) return;
  const Vec3 vb = s.attitude.toBody(v);
  // Nose above the velocity vector <=> velocity points below the nose (vb.z < 0).
  m.aoaDeg = std::atan2(-vb.z, vb.x) * kRadToDeg;
  // Sideslip sign: a velocity component along +right means the aircraft is moving to the right
  // of where its nose points, so the relative wind (-v) arrives from the right. Positive
  // v.right therefore gives positive sideslip ("air from the right").
  m.sideslipDeg = std::asin(std::clamp(vb.y / m.speed, -1.0, 1.0)) * kRadToDeg;
}

}  // namespace ft
