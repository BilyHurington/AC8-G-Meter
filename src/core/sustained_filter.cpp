#include "core/sustained_filter.h"

namespace ft {
namespace {

constexpr double kEpsS = 1e-9;  // round-off of summed frame times

}  // namespace

// ---- CentredMean ------------------------------------------------------------------------

void CentredMean::setWidth(double widthS) {
  const double half = widthS > 0.0 ? 0.5 * widthS : 0.0;
  if (half == half_) return;
  flush();
  half_ = half;
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
  const double from = next_ < buf_.size() ? buf_.at(next_).t - half_ - kEpsS : c.t;
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
  pre_.setWidth(0.25 * w);
  up_.setWidth(w);
  down_.setWidth(w);
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

}  // namespace ft
