#include "core/noise_estimator.h"

#include <algorithm>
#include <cmath>

namespace ft {

namespace {
constexpr double kGaussianAbsQuartile = 0.3186;  // lower quartile of |x|, x ~ N(0, 1)
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
  if (neighbours_ > 0 && t <= prevT_[kNeighbours - 1]) neighbours_ = 0;  // not after the previous sample
  if (neighbours_ == kNeighbours) {
    // Third divided difference of (prevT_, prevV_) and (t, velocity): sum c_i v_i with
    // c_i = 1 / prod_{j != i} (t_i - t_j). Exactly 0 for any quadratic v(t) (constant jerk);
    // normalised by sqrt(sum c_i^2) it is ~N(0, sigma^2) per axis for white noise. Velocities
    // relative to the newest one (sum c_i = 0) keep large speeds from costing precision.
    const std::array<double, kNeighbours + 1> ts{prevT_[0], prevT_[1], prevT_[2], t};
    Vec3 sum;
    double norm = 0.0;
    for (std::size_t i = 0; i < ts.size(); ++i) {
      double c = 1.0;
      for (std::size_t j = 0; j < ts.size(); ++j)
        if (j != i) c /= ts[i] - ts[j];
      if (i < kNeighbours) sum += (prevV_[i] - velocity) * c;
      norm += c * c;
    }
    const Vec3 r = sum / std::sqrt(norm);
    times_[head_] = t;
    residuals_[0][head_] = std::abs(r.x);
    residuals_[1][head_] = std::abs(r.y);
    residuals_[2][head_] = std::abs(r.z);
    intervals_[head_] = t - prevT_[kNeighbours - 1];
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    // A clock restart makes t < lastRefreshT_: refresh then as well.
    if (!ready_ || t - lastRefreshT_ >= kRefreshS || t < lastRefreshT_) {
      lastRefreshT_ = t;
      refresh();
    }
  }
  for (std::size_t i = 0; i + 1 < kNeighbours; ++i) {
    prevT_[i] = prevT_[i + 1];
    prevV_[i] = prevV_[i + 1];
  }
  prevT_[kNeighbours - 1] = t;
  prevV_[kNeighbours - 1] = velocity;
  neighbours_ = std::min(neighbours_ + 1, static_cast<int>(kNeighbours));
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

  // Long window: the noisiest axis. Its quartile has a relative standard error of ~1.8 / sqrt(n)
  // (gaussian noise), and the largest of three such estimates exceeds the true value by ~0.85
  // of that: divided out, so isotropic noise does not read high. Short window: the three axes
  // pooled; it overrides the long one only when clearly larger, so that its own scatter (~18 %)
  // does not bias steady noise upwards either.
  const double longQuartile = axisQuartile(n) / (1.0 + 1.52 / std::sqrt(static_cast<double>(n)));
  int m = 0;
  for (const auto& axis : residuals_)
    for (int k = 0; k < nAttack; ++k) scratch_[m++] = axis[entry(k)];
  const double attackQuartile = quantile(m, 0.25);
  sigma_ = (attackQuartile > kAttackRatio * longQuartile ? attackQuartile : longQuartile) / kGaussianAbsQuartile;
  if (n < kMinProvisionalResiduals) sigma_ = 0.0;  // a single glitch would dominate

  m = 0;
  for (int k = 0; k < nInterval; ++k)
    if (intervals_[entry(k)] >= kNearDuplicateS) scratch_[m++] = intervals_[entry(k)];
  if (m == 0) {  // nothing but near-duplicates: take them as they are
    for (int k = 0; k < nInterval; ++k) scratch_[k] = intervals_[entry(k)];
    m = nInterval;
  }
  interval_ = quantile(m, 0.5);
  ready_ = n >= kMinResiduals;
}

double NoiseEstimator::axisQuartile(int n) {
  double worst = 0.0;
  for (const auto& axis : residuals_) {
    for (int k = 0; k < n; ++k) scratch_[k] = axis[entry(k)];
    worst = std::max(worst, quantile(n, 0.25));
  }
  return worst;
}

double NoiseEstimator::quantile(int n, double q) {
  if (n == 0) return 0.0;
  const auto it = scratch_.begin() + static_cast<int>(q * n);
  std::nth_element(scratch_.begin(), it, scratch_.begin() + n);
  return *it;
}

}  // namespace ft
