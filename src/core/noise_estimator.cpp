#include "core/noise_estimator.h"

#include <algorithm>
#include <cmath>

namespace ft {

namespace {
constexpr double kGaussianNorm3Median = 1.538;  // median of |x|, x ~ N(0, I3)
}

void NoiseEstimator::reset() {
  count_ = 0;
  head_ = 0;
  neighbours_ = 0;
  ready_ = false;
  sigma_ = 0.0;
  interval_ = 0.0;
  lastRefreshT_ = 0.0;
}

void NoiseEstimator::add(double t, const Vec3& velocity) {
  if (neighbours_ > 0 && t <= t1_) neighbours_ = 0;  // not after the previous sample
  if (neighbours_ == 2) {
    const double w = (t1_ - t0_) / (t - t0_);
    const Vec3 r = v1_ - (v0_ + (velocity - v0_) * w);
    times_[head_] = t;
    residuals_[head_] = length(r) / std::sqrt(1.0 + w * w + (1.0 - w) * (1.0 - w));
    intervals_[head_] = t - t1_;
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    // A clock restart makes t < lastRefreshT_: refresh then as well.
    if (!ready_ || t - lastRefreshT_ >= kRefreshS || t < lastRefreshT_) {
      lastRefreshT_ = t;
      refresh();
    }
  }
  t0_ = t1_;
  v0_ = v1_;
  t1_ = t;
  v1_ = velocity;
  neighbours_ = std::min(neighbours_ + 1, 2);
}

void NoiseEstimator::refresh() {
  // Entries within kSpanS of the newest one, newest first. Stops at older-than-span entries
  // and at entries from before a clock restart (later timestamps).
  const int newest = (head_ - 1 + kCapacity) % kCapacity;
  int n = 0;
  for (; n < count_; ++n) {
    const int i = (newest - n + kCapacity) % kCapacity;
    const double age = times_[newest] - times_[i];
    if (age < 0.0 || age > kSpanS) break;
    scratch_[n] = residuals_[i];
  }
  // Once ready, keep the last estimate until enough fresh residuals (e.g. after a pause).
  if (ready_ && n < kMinResiduals) return;
  sigma_ = median(n) / kGaussianNorm3Median;
  for (int k = 0; k < n; ++k) scratch_[k] = intervals_[(newest - k + kCapacity) % kCapacity];
  interval_ = median(n);
  ready_ = n >= kMinResiduals;
}

double NoiseEstimator::median(int n) {
  if (n == 0) return 0.0;
  const auto mid = scratch_.begin() + n / 2;
  std::nth_element(scratch_.begin(), mid, scratch_.begin() + n);
  return *mid;
}

}  // namespace ft
