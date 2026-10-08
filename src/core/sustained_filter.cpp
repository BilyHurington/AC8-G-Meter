#include "core/sustained_filter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/math.h"

namespace ft {
namespace {

constexpr double kEpsS = 1e-9;  // round-off of summed frame times

}  // namespace

// ---- CentredMean ------------------------------------------------------------------------

void CentredMean::setWidth(double widthS) {
  const double half = widthS > 0.0 ? 0.5 * widthS : 0.0;
  keepHalf_ = half;
  if (half == half_) return;
  flush();
  half_ = half;
}

void CentredMean::setMaxWidth(double maxWidthS) { keepHalf_ = std::max(half_, 0.5 * maxWidthS); }

void CentredMean::adjustWidth(double widthS) {
  if (half_ <= 0.0 || !(widthS > 0.0)) return;  // pass-through stays pass-through
  half_ = std::min(0.5 * widthS, keepHalf_);
  // A shorter window may complete pending samples now; push() does it on the next sample.
}

void CentredMean::push(double t, double v) {
  if (half_ <= 0.0) {
    out_.push_back({t, v});
    return;
  }
  // Sample j is complete once a sample past t_j + W/2 arrives (that one is outside its window).
  while (next_ < buf_.size() && (t > buf_.at(next_).t + half_ + kEpsS || pending() >= kMaxPending)) emit();
  buf_.push_back({t, v});
}

void CentredMean::emit() {
  const Sample c = buf_.at(next_);
  double sum = 0.0;
  int n = 0;
  for (std::size_t k = 0; k < buf_.size(); ++k) {
    const Sample& s = buf_.at(k);
    if (s.t > c.t + half_ + kEpsS) break;
    if (s.t < c.t - half_ - kEpsS) continue;
    sum += s.v;
    ++n;
  }
  out_.push_back({c.t, sum / n});  // n >= 1: c itself
  ++next_;
  // Samples no later window reaches (and, at an absurd input rate, the oldest beyond a bound).
  const double from = next_ < buf_.size() ? buf_.at(next_).t - keepHalf_ - kEpsS : c.t;
  while (next_ > 0 && (buf_.front().t < from || buf_.size() > 2 * kMaxPending)) {
    buf_.pop_front();
    --next_;
  }
}

bool CentredMean::pop(Sample& out) {
  if (out_.empty()) return false;
  out = out_.front();
  out_.pop_front();
  return true;
}

void CentredMean::flush() {
  while (next_ < buf_.size()) emit();
  buf_.clear();
  next_ = 0;
}

void CentredMean::reset() {
  buf_.clear();
  next_ = 0;
  out_.clear();
}

// ---- SustainedFilter --------------------------------------------------------------------

void SustainedFilter::setWidth(double widthS) {
  const double w = widthS > 0.0 ? widthS : 0.0;
  if (w == w_) return;
  flush();
  w_ = w;
  pre_.setWidth(kMinPreRatio * w);
  pre_.setMaxWidth(kMaxPreRatio * w);
  up_.setWidth(w);
  down_.setWidth(w);
}

void SustainedFilter::setPreSmoothing(double widthS) {
  if (w_ <= 0.0) return;
  pre_.adjustWidth(std::clamp(widthS, kMinPreRatio * w_, kMaxPreRatio * w_));
}

void SustainedFilter::push(double t, double v) {
  pre_.push(t, v);
  forward();
}

void SustainedFilter::forward() {
  Sample s;
  while (pre_.pop(s)) {
    up_.push(s.t, s.v);
    down_.push(s.t, -s.v);
  }
  // Both excursion filters see the same timestamps, so they complete samples in lockstep.
  Sample a, b;
  while (up_.ready() > 0 && down_.ready() > 0) {
    up_.pop(a);
    down_.pop(b);
    ready_.push_back({a.t, 0.5 * (a.v - b.v)});
  }
}

bool SustainedFilter::pop(Sample& out) {
  if (ready_.empty()) return false;
  out = ready_.front();
  ready_.pop_front();
  return true;
}

void SustainedFilter::flush() {
  pre_.flush();
  forward();
  up_.flush();
  down_.flush();
  forward();
}

void SustainedFilter::reset() {
  pre_.reset();
  up_.reset();
  down_.reset();
  ready_.clear();
}

// ---- pre-smoothing width ----------------------------------------------------------------

double meanSlopeNoiseG(int n, int m, double dtS) {
  if (n < 2 || m < 1 || !(dtS > 0.0)) return 0.0;
  // Least-squares slope over n samples: sum k_i v_i with k_i = (i - c) / (dt * S).
  const double c = 0.5 * (n - 1);
  const double sxx = n * (static_cast<double>(n) * n - 1.0) / 12.0;
  // The mean of m slopes shifted by one sample each weighs sample j by
  // w_j = (K(min(j, n - 1) + 1) - K(max(0, j - m + 1))) / m, K = prefix sums of k.
  thread_local std::vector<double> prefix;
  prefix.assign(static_cast<std::size_t>(n) + 1, 0.0);
  for (int i = 0; i < n; ++i) prefix[static_cast<std::size_t>(i) + 1] = prefix[static_cast<std::size_t>(i)] + (i - c);
  double sum2 = 0.0;
  for (int j = 0; j < n + m - 1; ++j) {
    const int hi = std::min(j, n - 1) + 1, lo = std::max(0, j - m + 1);
    const double w = (prefix[static_cast<std::size_t>(hi)] - prefix[static_cast<std::size_t>(lo)]) / m;
    sum2 += w * w;
  }
  return std::sqrt(sum2) / (sxx * dtS * kG0);
}

double sustainedPreSmoothingS(double widthS, const FilterState& filter) {
  if (!(widthS > 0.0)) return 0.0;
  const double minP = SustainedFilter::kMinPreRatio * widthS, maxP = SustainedFilter::kMaxPreRatio * widthS;
  const double rate = filter.sampleRateHz;
  if (filter.input != FilterState::Input::Velocity || !filter.noiseReady || !(rate > 0.0) ||
      !(filter.velocityNoiseMS > 0.0) || !(filter.accelWindowS > 0.0))
    return minP;
  const double dt = 1.0 / rate;
  // Samples in the acceleration window (span <= window) and in a centred mean of width P.
  const int n = std::max(2, static_cast<int>(std::floor(filter.accelWindowS * rate + 1e-6)) + 1);
  auto samples = [&](double p) { return 2 * static_cast<int>(std::floor(0.5 * p * rate + 1e-6)) + 1; };
  const double target = kSustainedNoiseTargetG / filter.velocityNoiseMS;  // per m/s
  int lo = samples(minP), hi = samples(maxP);
  if (meanSlopeNoiseG(n, lo, dt) <= target) return minP;
  if (meanSlopeNoiseG(n, hi, dt) > target) return maxP;
  // Smallest odd m in (lo, hi] that meets the target (the noise falls with m); lo and hi odd.
  while (hi - lo > 2) {
    const int mid = lo + 2 * ((hi - lo) / 4);  // odd, strictly between
    if (meanSlopeNoiseG(n, mid, dt) <= target)
      hi = mid;
    else
      lo = mid;
  }
  // A width that holds hi samples with a quarter interval of slack on each side.
  return std::clamp((hi - 0.5) * dt, minP, maxP);
}

}  // namespace ft
