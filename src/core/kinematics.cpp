#include "core/kinematics.h"

#include <algorithm>
#include <cmath>

namespace ft {

void KinematicsEstimator::reset() {
  mode_ = Mode::Empty;
  window_.reset();
  hasVelocity_ = false;
  hasAccel_ = false;
  hasFit_ = false;
  velocity_ = {};
  accel_ = {};
  residualSigma_ = 0.0;
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
  window_.trim(windowS(), kMinPoints);
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
  accelT_ = window_.meanTime();
}

void KinematicsEstimator::estimateFromPosition() {
  const int n = window_.size();
  hasVelocity_ = n >= 2;
  hasAccel_ = false;
  hasFit_ = false;
  residualSigma_ = 0.0;
  if (n == 2) {
    velocity_ = window_.slope();
    return;
  }
  if (n < kMinPoints) return;

  // Fit p(s) = c0 + c1 s + c2 s^2 with s = (t - tMean) / h. Centring and scaling time to
  // ~[-1, 1] keeps the normal equations well conditioned; positions are taken relative
  // to the newest one so large world coordinates lose no precision.
  const double tMean = window_.meanTime();
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
  fitTMean_ = tMean;
  fitH_ = h;
  fitOrigin_ = origin;
  fitC1_ = (n * (y1 * s4 - y2 * s3) + y0 * (s2 * s3) - y1 * (s2 * s2)) / det;
  fitC2_ = (n * (y2 * s2 - y1 * s3) - y0 * (s2 * s2)) / det;
  fitC0_ = (y0 - fitC2_ * s2) / n;
  const double sLast = (window_.time(n - 1) - tMean) / h;
  velocity_ = (fitC1_ + fitC2_ * (2.0 * sLast)) / h;
  accel_ = fitC2_ * (2.0 / (h * h));
  accelT_ = tMean;
  hasFit_ = true;
  // A fit over just a few frames turns centimetres of position noise into tens of G:
  // report acceleration only once the window spans most of its length (at low frame
  // rates the kMinPoints samples always do).
  hasAccel_ = 2.0 * h >= kMinPositionSpan * positionWindowS_;

  // Noise estimate: residual sum of squares over the 3 (n - 3) degrees of freedom left.
  if (n > kMinPoints) {
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) {
      const Vec3 r = window_.value(i) - *predictPosition(window_.time(i));
      sumSq += dot(r, r);
    }
    residualSigma_ = std::sqrt(sumSq / (3.0 * (n - kMinPoints)));
  }
}

std::optional<Vec3> KinematicsEstimator::stepAcceleration(double t,
                                                          const std::optional<Vec3>& velocity) const {
  const double dt = t - lastT_;
  if (mode_ != Mode::Velocity || !velocity || dt <= 0.0) return std::nullopt;
  return (*velocity - velocity_) / dt;
}

std::optional<Vec3> KinematicsEstimator::predictPosition(double t) const {
  if (mode_ != Mode::Position || !hasVelocity_) return std::nullopt;
  if (!hasFit_) return lastP_ + velocity_ * (t - lastT_);
  const double s = (t - fitTMean_) / fitH_;
  return fitOrigin_ + fitC0_ + fitC1_ * s + fitC2_ * (s * s);
}

}  // namespace ft
