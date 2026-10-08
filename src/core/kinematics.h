#pragma once
// Velocity / acceleration estimation from timestamped samples.
//
//   velocity input : acceleration = least-squares slope of v(t) over the window.
//   position only  : least-squares quadratic fit of p(t) over the window; velocity is
//                    its derivative at the newest sample, acceleration its 2nd derivative.
//                    One fit instead of differentiating twice keeps lag and noise down.
// Both estimate the mean acceleration over the window, i.e. the acceleration at the
// window's mean sample time (accelerationTime()), ~windowS / 2 before the newest sample.
// Position-only input is much noisier to differentiate, so it gets its own (longer) window.
#include <optional>

#include "core/filters.h"
#include "core/math.h"

namespace ft {

class KinematicsEstimator {
 public:
  static constexpr int kMinPoints = 3;  // for an acceleration estimate
  // Position-only input: minimum window span, as a fraction of the window length, before
  // an acceleration is reported.
  static constexpr double kMinPositionSpan = 0.6;

  void setWindows(double velocityWindowS, double positionWindowS) {
    velocityWindowS_ = velocityWindowS;
    positionWindowS_ = positionWindowS;
  }
  // Window length of the current input mode.
  double windowS() const { return mode_ == Mode::Position ? positionWindowS_ : velocityWindowS_; }
  void reset();

  // Appends a sample. Switching between velocity and position input resets the window
  // (the two estimates are not interchangeable mid-window).
  void add(double t, const Vec3& position, const std::optional<Vec3>& velocity);

  bool empty() const { return window_.size() == 0; }
  bool hasVelocity() const { return hasVelocity_; }
  Vec3 velocity() const { return velocity_; }          // m/s at the newest sample
  bool hasAcceleration() const { return hasAccel_; }
  Vec3 acceleration() const { return accel_; }         // m/s^2, world frame
  double accelerationTime() const { return accelT_; }  // instant acceleration() refers to
  double lastTime() const { return lastT_; }
  Vec3 lastPosition() const { return lastP_; }

  // Velocity input: acceleration implied by the single step from the newest sample to
  // this one, or nothing if there is not enough state. Used to reject spikes before they
  // enter the window (one bad point would otherwise corrupt every estimate it stays in).
  std::optional<Vec3> stepAcceleration(double t, const std::optional<Vec3>& velocity) const;

  // Position-only input: where the current fit puts the aircraft at time t (the fit
  // extrapolated, or the newest position + velocity * dt with only two samples), or
  // nothing in velocity mode / without enough samples.
  std::optional<Vec3> predictPosition(double t) const;
  // Position-only input: estimated per-axis noise of the positions around the fit (m),
  // 0 when the window has too few samples to tell (the fit is then exact).
  double fitResidualSigma() const { return residualSigma_; }

 private:
  enum class Mode { Empty, Velocity, Position };
  void estimateFromVelocity();
  void estimateFromPosition();

  double velocityWindowS_ = 0.08;
  double positionWindowS_ = 0.15;
  Mode mode_ = Mode::Empty;
  TimedWindow<Vec3> window_;  // velocities or positions depending on mode_
  double lastT_ = 0.0;
  Vec3 lastP_;
  bool hasVelocity_ = false;
  Vec3 velocity_;
  bool hasAccel_ = false;
  Vec3 accel_;
  double accelT_ = 0.0;

  // Position fit p(s) = origin + c0 + c1 s + c2 s^2, s = (t - fitTMean_) / fitH_.
  bool hasFit_ = false;
  double fitTMean_ = 0.0, fitH_ = 1.0;
  Vec3 fitOrigin_, fitC0_, fitC1_, fitC2_;
  double residualSigma_ = 0.0;
};

}  // namespace ft
