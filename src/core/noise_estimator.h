#pragma once
// Robust estimate of the noise on a timestamped velocity stream, and of its sample interval.
//
// Each velocity sample is compared with the straight line through its two neighbours:
//   r = v1 - (v0 + (v2 - v0) w),  w = (t1 - t0) / (t2 - t0)
// For white noise of per-axis sigma each component of r has variance
// sigma^2 (1 + w^2 + (1 - w)^2), so the normalised residual is ~sigma per axis whatever the
// frame timing. Smooth motion adds only ~jerk * dt^2 / 2 (it is nearly a straight line over
// two frames), and a low quantile of the 3-D residual norm ignores the large residuals from
// glitches, G steps and discontinuities. The pipeline feeds every finite, time-ordered sample
// (before its spike gate, which depends on this estimate), so the statistics see the input as
// it is.
//
// The statistic is the lower quartile of the residual norms (1.101 sigma for gaussian noise):
// a glitch disturbs three residuals, so even 10 % of glitched samples affect ~27 % of them,
// and the median would flip on an unlucky cluster. Fast attack, slow release: sigma is the
// larger of the quartiles over the last kSpanS and over the last kAttackSpanS (at least
// kMinAttackResiduals), so more noise shows within ~0.1 s, less noise after ~0.5 s. The sample
// interval is the median interval over the last kIntervalSpanS (a rate change shows within
// ~0.4 s), ignoring near-duplicate timestamps.
//
// The quantiles are refreshed every kRefreshS (on every sample until ready()), from a
// preallocated scratch copy: O(1) per sample, no allocation.
#include <array>

#include "core/math.h"

namespace ft {

class NoiseEstimator {
 public:
  static constexpr int kCapacity = 2048;        // residuals kept (bounds kSpanS above ~1000 Hz)
  static constexpr double kSpanS = 2.0;         // long statistics window
  static constexpr double kAttackSpanS = 0.1;   // short window: noise increases show fast
  static constexpr double kIntervalSpanS = 0.5; // sample interval window
  // Intervals shorter than this are near-duplicate timestamps, not the sample rate.
  static constexpr double kNearDuplicateS = 2.5e-4;
  static constexpr double kRefreshS = 0.02;     // quantile refresh period once ready
  // For ready(); also the minimum count behind the interval median.
  static constexpr int kMinResiduals = 16;
  static constexpr int kMinAttackResiduals = 32;  // the attack window extends to this many
  static constexpr int kMinProvisionalResiduals = 8;  // for a provisional sigma() before ready()

  void reset();
  // Discontinuity (gap, pause): forgets the neighbours so no residual spans it, but keeps the
  // statistics, which describe the source rather than the aircraft state.
  void restartSequence() { neighbours_ = 0; }
  void add(double t, const Vec3& velocity);

  // True once kMinResiduals residuals have been seen. Stays true across restartSequence().
  bool ready() const { return ready_; }
  // Per-axis velocity noise sigma, m/s. Provisional (from fewer samples) before ready(), and 0
  // until kMinProvisionalResiduals residuals have been seen.
  double sigma() const { return sigma_; }
  // Median sample interval, s (robust to drops, duplicates and hitches). 0 until known.
  double sampleInterval() const { return interval_; }

 private:
  void refresh();
  double quantile(int n, double q);  // of scratch_[0, n), reorders it
  int entry(int age) const { return (head_ - 1 - age + 2 * kCapacity) % kCapacity; }  // 0 = newest

  // Ring of (time, normalised residual, interval) per sample with two neighbours.
  std::array<double, kCapacity> times_{}, residuals_{}, intervals_{};
  std::array<double, kCapacity> scratch_{};
  int count_ = 0, head_ = 0;

  int neighbours_ = 0;  // valid entries in (t0, v0), (t1, v1)
  double t0_ = 0.0, t1_ = 0.0;
  Vec3 v0_, v1_;

  bool ready_ = false;
  double sigma_ = 0.0, interval_ = 0.0;
  double lastRefreshT_ = 0.0;
};

}  // namespace ft
