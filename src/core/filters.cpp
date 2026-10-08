#include "core/filters.h"

#include <algorithm>
#include <cmath>

namespace ft {

void MedianFilter::setWindow(int window) {
  window = std::clamp(window, 1, kMaxWindow);
  if (window % 2 == 0) window = window + 1 <= kMaxWindow ? window + 1 : window - 1;
  window_ = window;
  reset();
}

double MedianFilter::update(double x) {
  buf_[head_] = x;
  head_ = (head_ + 1) % window_;
  if (count_ < window_) ++count_;
  if (count_ == 1) return x;

  // Order of the ring doesn't matter for a median; sort a copy of the filled part.
  std::array<double, kMaxWindow> tmp{};
  std::copy(buf_.begin(), buf_.begin() + count_, tmp.begin());
  std::sort(tmp.begin(), tmp.begin() + count_);
  const int mid = count_ / 2;
  // Only reachable with an even count while the window is still filling.
  return count_ % 2 == 1 ? tmp[mid] : 0.5 * (tmp[mid - 1] + tmp[mid]);
}

double Ema::update(double x, double dtS) {
  if (!initialised_ || tau_ <= 0.0) {
    value_ = x;
    initialised_ = true;
    return value_;
  }
  const double alpha = 1.0 - std::exp(-std::max(dtS, 0.0) / tau_);
  value_ += alpha * (x - value_);
  return value_;
}

}  // namespace ft
