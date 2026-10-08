#include "core/noise_estimator.h"

#include <algorithm>
#include <cmath>

namespace ft {

namespace {
constexpr double kGaussianNorm3Quartile = 1.101;  // lower quartile of |x|, x ~ N(0, I3)
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
  // and at entries from before a clock restart (later timestamps). nAttack / nInterval: how
  // many of them fall within the short windows.
  const double newestT = times_[entry(0)];
  int n = 0, nAttack = 0, nInterval = 0;
  for (; n < count_; ++n) {
    const double age = newestT - times_[entry(n)];
    if (age < 0.0 || age > kSpanS) break;
    if (age <= kAttackSpanS) ++nAttack;
    if (age <= kIntervalSpanS) ++nInterval;
  }
  // Once ready, keep the last estimate until enough fresh residuals (e.g. after a pause).
  if (ready_ && n < kMinResiduals) return;
  nAttack = std::min(std::max(nAttack, kMinAttackResiduals), n);
  nInterval = std::min(std::max(nInterval, kMinResiduals), n);

  for (int k = 0; k < n; ++k) scratch_[k] = residuals_[entry(k)];
  const double longQuartile = quantile(n, 0.25);
  for (int k = 0; k < nAttack; ++k) scratch_[k] = residuals_[entry(k)];
  sigma_ = std::max(longQuartile, quantile(nAttack, 0.25)) / kGaussianNorm3Quartile;
  if (n < kMinProvisionalResiduals) sigma_ = 0.0;  // a single glitch would dominate

  int m = 0;
  for (int k = 0; k < nInterval; ++k)
    if (intervals_[entry(k)] >= kNearDuplicateS) scratch_[m++] = intervals_[entry(k)];
  if (m == 0) {  // nothing but near-duplicates: take them as they are
    for (int k = 0; k < nInterval; ++k) scratch_[k] = intervals_[entry(k)];
    m = nInterval;
  }
  interval_ = quantile(m, 0.5);
  ready_ = n >= kMinResiduals;
}

double NoiseEstimator::quantile(int n, double q) {
  if (n == 0) return 0.0;
  const auto it = scratch_.begin() + static_cast<int>(q * n);
  std::nth_element(scratch_.begin(), it, scratch_.begin() + n);
  return *it;
}

}  // namespace ft
