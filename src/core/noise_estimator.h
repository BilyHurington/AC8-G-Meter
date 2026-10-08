#pragma once
// Robust estimate of the noise on a timestamped velocity stream, and of its sample interval.
//
// Each velocity sample is compared with the straight line through its two neighbours:
//   r = v1 - (v0 + (v2 - v0) w),  w = (t1 - t0) / (t2 - t0)
// For white noise of per-axis sigma each component of r has variance
// sigma^2 (1 + w^2 + (1 - w)^2), so the normalised residual is ~sigma per axis whatever the
// frame timing. Smooth motion adds only ~jerk * dt^2 / 2 (it is nearly a straight line over
// two frames), and the *median* of the 3-D residual norm over the last kSpanS ignores the
// few large residuals from glitches, G steps and discontinuities. The median of the norm of
// a 3-D gaussian is 1.538 sigma.
//
// The medians are refreshed every kRefreshS (on every sample until ready()), from a
// preallocated scratch copy: O(1) per sample, no allocation.
#include <array>

#include "core/math.h"

namespace ft {

class NoiseEstimator {
 public:
  static constexpr int kCapacity = 2048;    // residuals kept (bounds kSpanS above ~1000 Hz)
  static constexpr double kSpanS = 2.0;     // statistics cover the last kSpanS of samples
  static constexpr double kRefreshS = 0.1;  // median refresh period once ready
  static constexpr int kMinResiduals = 16;  // for ready()

  void reset();
  // Discontinuity (gap, teleport, respawn): forgets the neighbours so no residual spans it,
  // but keeps the statistics, which describe the source rather than the aircraft state.
  void restartSequence() { neighbours_ = 0; }
  void add(double t, const Vec3& velocity);

  // True once kMinResiduals residuals have been seen. Stays true across restartSequence().
  bool ready() const { return ready_; }
  // Per-axis velocity noise sigma, m/s. Provisional (from fewer samples) before ready().
  double sigma() const { return sigma_; }
  // Median sample interval, s (robust to drops, duplicates and hitches). 0 until known.
  double sampleInterval() const { return interval_; }

 private:
  void refresh();
  double median(int n);  // of scratch_[0, n), reorders it

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
