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
  // Minimum window span, as a fraction of the window length. A fit over just a few frames
  // turns input noise into tens of G at high sample rates (3 velocity samples 1 ms apart with
  // 0.2 m/s of noise: ~14 G per axis), so:
  //   - after a reset, no acceleration is reported until the window spans this much
  //     (warm-up only: a window that later grows, e.g. a longer adaptive window, keeps
  //     reporting while it fills);
  //   - trimming keeps one sample older than the window while the rest span less than this,
  //     so frame-time jitter never shrinks the fit below it.
  static constexpr double kMinSpan = 0.6;

  void setWindows(double velocityWindowS, double positionWindowS) {
    velocityWindowS_ = velocityWindowS;
    positionWindowS_ = positionWindowS;
  }
  // Window length of the current input mode.
  double windowS() const { return mode_ == Mode::Position ? positionWindowS_ : velocityWindowS_; }
  void reset();

  // Appends a sample. Switching between velocity and position input resets the window
  // (the two estimates are not interchangeable mid-window). holdNewest (velocity input): the
  // sample is suspect (SpikeGate): it is used for predictions, but the estimates (velocity(),
  // acceleration(), ...) leave it out until the next add() or removeNewest().
  void add(double t, const Vec3& position, const std::optional<Vec3>& velocity, bool holdNewest = false);

  // Velocity input: removes the newest `count` samples (glitches found after the fact) and
  // re-estimates from the others. At least one sample is kept.
  void removeNewest(int count = 1);

  bool empty() const { return window_.size() == 0; }
  int size() const { return window_.size(); }  // samples in the window
  double oldestTime() const { return window_.size() > 0 ? window_.time(0) : lastT_; }
  double lastTime() const { return lastT_; }
  bool hasVelocity() const { return hasVelocity_; }
  Vec3 velocity() const { return velocity_; }          // m/s at the newest sample
  bool hasAcceleration() const { return hasAccel_; }
  Vec3 acceleration() const { return accel_; }         // m/s^2, world frame
  double accelerationTime() const { return accelT_; }  // instant acceleration() refers to

  // Velocity input: the velocity expected at time t from the newest sample and the current
  // least-squares acceleration trend (also used before acceleration() is reported; constant
  // velocity with a single sample), or nothing in position mode / with an empty window. Used
  // to reject spikes before they enter the window (one bad point would otherwise corrupt
  // every estimate it stays in).
  std::optional<Vec3> predictVelocity(double t) const;
  // Same, from the window without its newest `count` samples (nothing if that leaves none).
  // Tells whether the newest samples, rather than a new one, are the outliers.
  std::optional<Vec3> predictVelocityWithoutNewest(double t, int count = 1) const;

  // Position-only input: where the current fit puts the aircraft at time t (the fit
  // extrapolated, or the newest position + velocity * dt with only two samples), or
  // nothing in velocity mode / without enough samples.
  std::optional<Vec3> predictPosition(double t) const;
  // Position-only input: estimated per-axis noise of the positions around the fit (m),
  // 0 when the window has too few samples to tell (the fit is then exact).
  double fitResidualSigma() const { return residualSigma_; }

 private:
  enum class Mode { Empty, Velocity, Position };
  void estimateFromVelocity(bool holdNewest);
  void estimateFromPosition();
  // Velocity at time t extrapolated from the oldest n window samples (n >= 1).
  Vec3 extrapolateVelocity(int n, double t) const;

  double velocityWindowS_ = 0.08;
  double positionWindowS_ = 0.15;
  Mode mode_ = Mode::Empty;
  TimedWindow<Vec3> window_;  // velocities or positions depending on mode_
  double lastT_ = 0.0;
  Vec3 lastP_;
  bool hasVelocity_ = false;
  Vec3 velocity_;
  bool hasAccel_ = false;
  bool warmedUp_ = false;  // the window has spanned kMinSpan of its length since the reset
  Vec3 accel_;
  double accelT_ = 0.0;

  // Position fit p(s) = origin + c0 + c1 s + c2 s^2, s = (t - fitTMean_) / fitH_.
  bool hasFit_ = false;
  double fitTMean_ = 0.0, fitH_ = 1.0;
  Vec3 fitOrigin_, fitC0_, fitC1_, fitC2_;
  double residualSigma_ = 0.0;
};

}  // namespace ft
