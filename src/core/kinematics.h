#pragma once
// Velocity / acceleration estimation from timestamped samples.
//
//   velocity input : acceleration = least-squares slope of v(t) over the window.
//   position only  : least-squares quadratic fit of p(t) over the window; velocity is
//                    its derivative at the newest sample, acceleration its 2nd derivative.
//                    One fit instead of differentiating twice keeps lag and noise down.
// Both estimate the mean acceleration over the window, i.e. they lag ~windowS / 2.
#include <optional>

#include "core/filters.h"
#include "core/math.h"

namespace ft {

class KinematicsEstimator {
 public:
  static constexpr int kMinPoints = 3;  // for an acceleration estimate

  void setWindow(double windowS) { windowS_ = windowS; }
  void reset();

  // Appends a sample. Switching between velocity and position input resets the window
  // (the two estimates are not interchangeable mid-window).
  void add(double t, const Vec3& position, const std::optional<Vec3>& velocity);

  bool empty() const { return window_.size() == 0; }
  bool hasVelocity() const { return hasVelocity_; }
  Vec3 velocity() const { return velocity_; }          // m/s at the newest sample
  bool hasAcceleration() const { return hasAccel_; }
  Vec3 acceleration() const { return accel_; }         // m/s^2, world frame
  double lastTime() const { return lastT_; }
  Vec3 lastPosition() const { return lastP_; }

  // Acceleration implied by the single step from the newest sample to this one, or
  // nothing if there is not enough state. Used to reject spikes before they enter the
  // window (one bad point would otherwise corrupt every estimate it stays in).
  std::optional<Vec3> stepAcceleration(double t, const Vec3& position,
                                       const std::optional<Vec3>& velocity) const;

 private:
  enum class Mode { Empty, Velocity, Position };
  void estimateFromVelocity();
  void estimateFromPosition();

  double windowS_ = 0.08;
  Mode mode_ = Mode::Empty;
  TimedWindow<Vec3> window_;  // velocities or positions depending on mode_
  double lastT_ = 0.0;
  Vec3 lastP_;
  bool hasVelocity_ = false;
  Vec3 velocity_;
  bool hasAccel_ = false;
  Vec3 accel_;
};

}  // namespace ft
