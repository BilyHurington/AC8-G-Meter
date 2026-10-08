#include "core/kinematics.h"

#include <algorithm>
#include <cmath>

namespace ft {

void KinematicsEstimator::reset() {
  mode_ = Mode::Empty;
  window_.reset();
  hasVelocity_ = false;
  hasAccel_ = false;
  velocity_ = {};
  accel_ = {};
}

void KinematicsEstimator::add(double t, const Vec3& position, const std::optional<Vec3>& velocity) {
  const Mode mode = velocity ? Mode::Velocity : Mode::Position;
  if (mode != mode_) {
    reset();
    mode_ = mode;
  }
  lastT_ = t;
  lastP_ = position;
  window_.push(t, velocity ? *velocity : position);
  window_.trim(windowS_, kMinPoints);
  if (mode_ == Mode::Velocity) {
    estimateFromVelocity();
  } else {
    estimateFromPosition();
  }
}

void KinematicsEstimator::estimateFromVelocity() {
  hasVelocity_ = true;
  velocity_ = window_.value(window_.size() - 1);
  hasAccel_ = window_.size() >= kMinPoints;
  accel_ = hasAccel_ ? window_.slope() : Vec3{};
}

void KinematicsEstimator::estimateFromPosition() {
  const int n = window_.size();
  hasVelocity_ = n >= 2;
  hasAccel_ = false;
  if (n == 2) {
    velocity_ = window_.slope();
    return;
  }
  if (n < kMinPoints) return;

  // Fit p(s) = c0 + c1 s + c2 s^2 with s = (t - tMean) / h. Centring and scaling time to
  // ~[-1, 1] keeps the normal equations well conditioned; positions are taken relative
  // to the newest one so large world coordinates lose no precision.
  double tMean = 0.0;
  for (int i = 0; i < n; ++i) tMean += window_.time(i);
  tMean /= n;
  const double h = std::max(0.5 * (window_.time(n - 1) - window_.time(0)), 1e-9);
  const Vec3 origin = window_.value(n - 1);

  double s2 = 0.0, s3 = 0.0, s4 = 0.0;
  Vec3 y0, y1, y2;
  for (int i = 0; i < n; ++i) {
    const double s = (window_.time(i) - tMean) / h;
    const Vec3 p = window_.value(i) - origin;
    s2 += s * s;
    s3 += s * s * s;
    s4 += s * s * s * s;
    y0 += p;
    y1 += p * s;
    y2 += p * (s * s);
  }
  // Cramer's rule on [[n,0,s2],[0,s2,s3],[s2,s3,s4]] (sum of s is 0 by construction).
  const double det = n * (s2 * s4 - s3 * s3) - s2 * s2 * s2;
  if (std::abs(det) < 1e-9) {
    velocity_ = window_.slope();
    return;
  }
  const Vec3 c1 = (n * (y1 * s4 - y2 * s3) + y0 * (s2 * s3) - y1 * (s2 * s2)) / det;
  const Vec3 c2 = (n * (y2 * s2 - y1 * s3) - y0 * (s2 * s2)) / det;
  const double sLast = (window_.time(n - 1) - tMean) / h;
  velocity_ = (c1 + c2 * (2.0 * sLast)) / h;
  accel_ = c2 * (2.0 / (h * h));
  hasAccel_ = true;
}

std::optional<Vec3> KinematicsEstimator::stepAcceleration(double t, const Vec3& position,
                                                          const std::optional<Vec3>& velocity) const {
  const double dt = t - lastT_;
  if (!hasVelocity_ || dt <= 0.0) return std::nullopt;
  if (mode_ == Mode::Velocity && velocity) return (*velocity - velocity_) / dt;
  // p = p0 + v0 dt + a dt^2 / 2  =>  a = 2 (p - p0 - v0 dt) / dt^2
  if (mode_ == Mode::Position && !velocity) return (position - lastP_ - velocity_ * dt) * (2.0 / (dt * dt));
  return std::nullopt;
}

}  // namespace ft
