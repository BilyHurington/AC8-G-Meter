#include "core/excursion_filter.h"

#include <initializer_list>

namespace ft {
namespace {

// Round-off of summed frame times: a sample exactly W away is outside the window.
constexpr double kEpsS = 1e-9;

}  // namespace

void ExcursionFilter::Window::add(const Sample& s) {
  // Samples dominated by the new one can never be the extremum again.
  while (!dq.empty() && (max ? dq.back().v <= s.v : dq.back().v >= s.v)) dq.pop_back();
  dq.push_back(s);
}

void ExcursionFilter::Window::dropBefore(double t, bool inclusive) {
  while (!dq.empty() && (inclusive ? dq.front().t <= t : dq.front().t < t)) dq.pop_front();
}

void ExcursionFilter::setWidth(double widthS) {
  const double w = widthS > 0.0 ? widthS : 0.0;
  if (w == w_) return;
  flush();
  w_ = w;
}

void ExcursionFilter::push(double t, double v) {
  if (w_ <= 0.0) {
    out_.push_back({t, v});
    return;
  }
  // Stage 1, erosion over (t - W, t]: immediate.
  s1_.add({t, v});
  s1_.dropBefore(t - w_ + kEpsS, true);
  const double eroded = s1_.dq.front().v;
  // Stage 2, dilation over [t_j, t_j + W): sample j is complete once a sample at or past
  // t_j + W arrives (that sample itself is outside its window).
  while (!pending2_.empty() && (t >= pending2_.front() + w_ - kEpsS || pending2_.size() >= kMaxPending)) emit2();
  s2_.add({t, eroded});
  pending2_.push_back(t);
}

void ExcursionFilter::emit2() {
  const double tj = pending2_.front();
  pending2_.pop_front();
  s2_.dropBefore(tj, false);
  feed3({tj, s2_.dq.front().v});
}

void ExcursionFilter::feed3(const Sample& s) {
  // Stage 3, dilation over (t - W, t], then stage 4, erosion over [t_j, t_j + W).
  s3_.add(s);
  s3_.dropBefore(s.t - w_ + kEpsS, true);
  const double dilated = s3_.dq.front().v;
  while (!pending4_.empty() && (s.t >= pending4_.front() + w_ - kEpsS || pending4_.size() >= kMaxPending)) emit4();
  s4_.add({s.t, dilated});
  pending4_.push_back(s.t);
}

void ExcursionFilter::emit4() {
  const double tj = pending4_.front();
  pending4_.pop_front();
  s4_.dropBefore(tj, false);
  out_.push_back({tj, s4_.dq.front().v});
}

bool ExcursionFilter::pop(Sample& out) {
  if (out_.empty()) return false;
  out = out_.front();
  out_.pop_front();
  return true;
}

void ExcursionFilter::flush() {
  while (!pending2_.empty()) emit2();
  while (!pending4_.empty()) emit4();
  for (Window* s : {&s1_, &s2_, &s3_, &s4_}) s->dq.clear();
}

void ExcursionFilter::reset() {
  flush();
  out_.clear();
}

}  // namespace ft
