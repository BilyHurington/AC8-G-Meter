#include "sources/flight_model.h"

#include <algorithm>
#include <cmath>

namespace ft {

namespace {

constexpr double kMaxSubStepS = 1.0 / 240.0;

// Lift direction for a velocity direction when nothing better is known: "up" projected
// perpendicular to the velocity (falls back to an arbitrary perpendicular when vertical).
Vec3 perpendicularUp(const Vec3& vhat, const Vec3& hint) {
  Vec3 l = hint - vhat * dot(hint, vhat);
  if (length(l) < 1e-6) l = Vec3{1.0, 0.0, 0.0} - vhat * vhat.x;
  if (length(l) < 1e-6) l = Vec3{0.0, 1.0, 0.0} - vhat * vhat.y;
  return normalized(l);
}

// Minimal rotation taking unit vector a onto unit vector b, applied to x (x ⟂ a).
// This is parallel transport of the lift axis along the velocity direction: no Euler
// angles, so vertical and inverted flight need no special cases.
Vec3 transport(const Vec3& x, const Vec3& a, const Vec3& b) {
  const double c = dot(a, b);
  if (c <= -0.999999) return x;  // 180° flip within one sub-step cannot happen physically
  return x - (a + b) * (dot(b, x) / (1.0 + c));
}

}  // namespace

PointMassFlightModel::PointMassFlightModel(const FlightModelParams& params) : params_(params) {
  reset(FlightState{});
}

void PointMassFlightModel::reset(const FlightState& initial) {
  state_ = initial;
  const Vec3 vhat = normalized(state_.velocity);
  // Keep the caller's bank: derive the lift axis from the given attitude when sensible.
  const Vec3 hint = state_.attitude.isOrthonormal() ? state_.attitude.up : kWorldUp;
  liftDir_ = perpendicularUp(vhat, hint);
  throttle_ = 0.5;
  updateAttitudeAndTruth();
}

void PointMassFlightModel::translate(const Vec3& offset) { state_.position += offset; }

double PointMassFlightModel::aoaDegFor(double nz, double speed) const {
  const double ratio = params_.refSpeed / std::max(speed, 1.0);
  return std::clamp(params_.aoaPerG * nz * ratio * ratio, -params_.maxAoaDeg, params_.maxAoaDeg);
}

Vec3 PointMassFlightModel::specificForce(const Vec3& vel, const Vec3& lift, double nz,
                                         double throttle) const {
  const double v = std::max(length(vel), 1.0);
  const Vec3 vhat = vel / v;
  const double ratio = params_.refSpeed / v;
  const double thrustG = std::clamp(throttle, 0.0, 1.0) * params_.maxThrustG;
  const double dragG = params_.dragCoef * v * v + params_.inducedDragCoef * nz * nz * ratio * ratio;
  double longG = thrustG - dragG;

  // Speed limits are enforced through the force (not by rescaling the velocity) so the
  // trajectory stays exactly consistent with the reported specific force. Outside the
  // limits the tangential acceleration is pushed back towards the allowed band.
  const double gravityAlongG = dot(kGravityWorld, vhat) / kG0;
  const double tangentialG = longG + gravityAlongG;
  if (v <= params_.minSpeed) {
    const double needG = (params_.minSpeed - v) / kG0;  // recover at ~1 (m/s)/s per m/s
    if (tangentialG < needG) longG = needG - gravityAlongG;
  } else if (v >= params_.maxSpeed) {
    const double needG = (params_.maxSpeed - v) / kG0;
    if (tangentialG > needG) longG = needG - gravityAlongG;
  }
  return lift * (nz * kG0) + vhat * (longG * kG0);
}

void PointMassFlightModel::step(double dt, const ControlInput& input) {
  if (!(dt > 0.0)) return;
  const int n = std::max(1, static_cast<int>(std::ceil(dt / kMaxSubStepS - 1e-9)));
  const double h = dt / n;
  for (int i = 0; i < n; ++i) integrate(h, input);
  throttle_ = input.throttle;
  updateAttitudeAndTruth();
}

void PointMassFlightModel::integrate(double h, const ControlInput& input) {
  FlightState& s = state_;

  // Airframe response: the applied load factor follows the command at a limited rate.
  const double maxDelta = params_.nzSlewGPerS * h;
  s.appliedNz += std::clamp(input.nzCmd - s.appliedNz, -maxDelta, maxDelta);

  // Roll rotates the lift axis about the velocity vector. Positive = right wing down,
  // i.e. lift tilts towards the right axis (right = lift x velocity).
  const Vec3 vhat = normalized(s.velocity);
  const double roll = input.rollRateDegS * kDegToRad * h;
  const Vec3 right = cross(liftDir_, vhat);
  liftDir_ = liftDir_ * std::cos(roll) + right * std::sin(roll);

  // RK2 (midpoint). The lift axis is transported along with the velocity direction.
  const Vec3 a1 = specificForce(s.velocity, liftDir_, s.appliedNz, input.throttle) + kGravityWorld;
  const Vec3 vMid = s.velocity + a1 * (0.5 * h);
  const Vec3 liftMid = normalized(transport(liftDir_, vhat, normalized(vMid)));
  const Vec3 a2 = specificForce(vMid, liftMid, s.appliedNz, input.throttle) + kGravityWorld;

  s.position += vMid * h;
  s.velocity += a2 * h;
  s.t += h;

  // Transport + re-orthonormalize so rounding never accumulates.
  const Vec3 vhatNew = normalized(s.velocity);
  liftDir_ = perpendicularUp(vhatNew, transport(liftDir_, vhat, vhatNew));
}

void PointMassFlightModel::updateAttitudeAndTruth() {
  FlightState& s = state_;
  const double speed = length(s.velocity);
  const Vec3 vhat = normalized(s.velocity);

  // Body = flight-path frame pitched nose-up by AoA about the right axis.
  const double aoa = aoaDegFor(s.appliedNz, speed) * kDegToRad;
  s.attitude.forward = vhat * std::cos(aoa) + liftDir_ * std::sin(aoa);
  s.attitude.up = liftDir_ * std::cos(aoa) - vhat * std::sin(aoa);
  s.attitude.right = cross(s.attitude.up, s.attitude.forward);

  // Truth from the instantaneous specific force (what an ideal accelerometer measures),
  // so thrust/drag projected on the tilted body axes are included.
  const Vec3 f = specificForce(s.velocity, liftDir_, s.appliedNz, throttle_);
  s.trueNz = dot(f, s.attitude.up) / kG0;
  s.trueGTotal = length(f) / kG0;

  // Same convention as the core estimator.
  const Vec3 vb = s.attitude.toBody(s.velocity);
  s.trueAoaDeg = std::atan2(-vb.z, vb.x) * kRadToDeg;
  s.trueSideslipDeg = speed > 1e-9 ? std::asin(std::clamp(vb.y / speed, -1.0, 1.0)) * kRadToDeg : 0.0;
}

}  // namespace ft
