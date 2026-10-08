#pragma once
// SustainedFilter: the signal behind the *sustained* mission statistics. Removes G excursions
// shorter than a width W, unbiased on noisy input:
//
//   pre  = centred moving mean of the input over [t - W/8, t + W/8]   (width W / 4, zero phase)
//   out  = ( closing(opening(pre)) + opening(closing(pre)) ) / 2       (ExcursionFilter, width W)
//
// Why both steps:
//   - Morphological filters work on extremes, so on noisy input their output is biased: the
//     opening of noise is pulled down by the noise minima, the closing up by the maxima, and
//     whichever runs first wins (open-then-close read a 7 G plateau with 0.3 m/s velocity noise
//     at 60 Hz as 6.7 G, and level flight as 0.1 G). The average of the two orders is an odd
//     function of the input, so symmetric noise no longer biases it.
//   - The pre-smoother takes most of the noise out first. It is centred, so it does not shift
//     anything in time (the path is delayed anyway). It widens a rectangular bump by W / 4, so
//     the cut-off becomes a soft one: bumps up to ~0.75 W are removed, bumps from ~1.25 W on are
//     kept at full height, in between partly.
// Plateaus and steps held at least W (+ W / 8 on each side for the pre-smoother) come out
// exactly. Every pushed sample comes out once, in order, with its own timestamp, delayed by up
// to W / 8 + 2 W. W <= 0 passes samples straight through.
#include <cstddef>

#include "core/excursion_filter.h"

namespace ft {

// Centred moving mean over [t - W/2, t + W/2] (plain mean of the samples in it), delayed by
// W / 2. Windows are truncated at the start of the stream and at flush().
class CentredMean {
 public:
  using Sample = ExcursionFilter::Sample;
  static constexpr std::size_t kMaxPending = ExcursionFilter::kMaxPending;

  // Changing the width completes the pending samples with the old one first.
  void setWidth(double widthS);
  double width() const { return 2.0 * half_; }
  void push(double t, double v);  // t must not decrease
  bool pop(Sample& out);
  void flush();  // completes every pending sample and starts afresh
  void reset();  // drops everything
  std::size_t pending() const { return buf_.size() - next_; }

 private:
  void emit();  // completes buf_[next_]

  double half_ = 0.0;
  RingQueue<Sample> buf_;  // from the oldest sample a pending window needs to the newest
  std::size_t next_ = 0;   // index in buf_ of the oldest sample not yet emitted
  RingQueue<Sample> out_;
};

class SustainedFilter {
 public:
  using Sample = ExcursionFilter::Sample;

  explicit SustainedFilter(double widthS = 0.0) { setWidth(widthS); }
  // W in seconds. A change completes the pending samples with the old width first (see flush()).
  void setWidth(double widthS);
  double width() const { return w_; }

  void push(double t, double v);  // t must not decrease
  bool pop(Sample& out);
  // Completes every pending sample (windows truncated, as at the end of a stream) and starts
  // afresh. Truncation at the end is safe: it can only remove an excursion in the last W, never
  // invent one. (At the *start* of a stream it is not: an excursion in progress there looks
  // sustained. The caller treats the first W of output after a restart as unconfirmed.)
  void flush();
  void reset();  // drops everything, pending samples included
  // Samples pushed but not yet available to pop().
  std::size_t pending() const { return pre_.pending() + up_.pending() + up_.ready(); }

 private:
  void forward();  // pre-smoothed samples on to the two excursion filters

  double w_ = 0.0;
  CentredMean pre_;
  ExcursionFilter up_;    // closing(opening(x))
  ExcursionFilter down_;  // fed -x: -opening(closing(x)) comes out
  RingQueue<Sample> ready_;
};

}  // namespace ft
