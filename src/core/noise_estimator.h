#pragma once
// Robust estimate of the noise on a timestamped velocity stream, and of its sample interval.
//
// Each velocity sample is compared with the quadratic through its three predecessors: the
// residual is their third divided difference (any frame timing), normalised so that for white
// noise of per-axis sigma each component is ~sigma. Smooth motion adds only ~d^3v/dt^3 * dt^3
// (the line through two neighbours would read an 11 G/s onset as 0.04 m/s of noise at 60 Hz),
// and a low quantile of the residuals ignores the large ones from glitches, G steps and
// discontinuities. The pipeline feeds every finite, time-ordered sample (before its spike gate,
// which depends on this estimate), so the statistics see the input as it is.
//
// The statistic is the lower quartile of |r| per world axis (0.319 sigma for gaussian noise):
// a glitch disturbs four residuals, so even 10 % of glitched samples affect ~34 % of them, and
// the median would flip on an unlucky cluster. Over the last kSpanS it is taken per axis, and
// sigma is that of the noisiest axis: noise on one axis only (say a quantised vertical speed)
// must not be diluted by the clean ones, as a quantile of the 3-D norm was (it read 0.14 for
// 0.5 on one axis). Fast attack, slow release: the quartile over the last kAttackSpanS (at
// least kMinAttackResiduals per axis, the three axes pooled) replaces it when it reads more
// than kAttackRatio times as much, so more noise shows within ~0.1 s at high rates (~0.4 s at
// 60 Hz; on one axis only, or less than kAttackRatio times more: within ~1.5 s), less noise
// after ~0.5 s. The sample interval is the median interval over the last kIntervalSpanS (a
// rate change shows within ~0.4 s), ignoring near-duplicate timestamps.
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
  static constexpr int kMinAttackResiduals = 32;  // per axis: the attack window extends to this many
  // The short window overrides the long one only when it reads this much more noise.
  static constexpr double kAttackRatio = 1.5;
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

  // Largest lower quartile of the per-axis |residual| over the newest n entries.
  double axisQuartile(int n);

  // Ring of (time, normalised per-axis |residual|, interval) per sample with two neighbours.
  std::array<double, kCapacity> times_{}, intervals_{};
  std::array<std::array<double, kCapacity>, 3> residuals_{};
  std::array<double, 3 * kCapacity> scratch_{};
  int count_ = 0, head_ = 0;

  static constexpr std::size_t kNeighbours = 3;  // predecessors each residual needs
  int neighbours_ = 0;  // valid (newest) entries of prevT_ / prevV_, oldest first
  std::array<double, kNeighbours> prevT_{};
  std::array<Vec3, kNeighbours> prevV_{};

  bool ready_ = false;
  double sigma_ = 0.0, interval_ = 0.0;
  double lastRefreshT_ = 0.0;
};

}  // namespace ft
