#pragma once
// SustainedFilter: the signal behind the *sustained* mission statistics. Removes G excursions
// shorter than a width W, unbiased on noisy input:
//
//   pre  = centred moving mean of the input over [t - P/2, t + P/2]   (width P, zero phase)
//   out  = ( closing(opening(pre)) + opening(closing(pre)) ) / 2       (ExcursionFilter, width W)
//
// Why both steps:
//   - Morphological filters work on extremes, so on noisy input their output is biased: the
//     opening of noise is pulled down by the noise minima, the closing up by the maxima, and
//     whichever runs first wins (open-then-close read a 7 G plateau with 0.3 m/s velocity noise
//     at 60 Hz as 6.7 G, and level flight as 0.1 G). The average of the two orders is an odd
//     function of the input, so symmetric noise no longer biases it.
//   - The pre-smoother takes most of the noise out first. It is centred, so it does not shift
//     anything in time (the path is delayed anyway). It widens a rectangular bump by P, so the
//     cut-off becomes a soft one: bumps up to ~W - P are removed, bumps from ~W + P on are kept
//     at full height, in between partly.
//   - P is W / 4 on clean data (bumps up to 0.75 W removed, full height from 1.25 W). What noise
//     the averaging leaves still pushes the *extremes* outward (the highest of many noisy
//     samples), by about half the residual noise sigma. So on noisy input the caller widens P,
//     up to W / 2, until the predicted residual is at most kSustainedNoiseTargetG
//     (sustainedPreSmoothingS): a bias of ~0.1 G, for bumps up to only ~0.5 W removed and
//     full height from ~1.5 W. Noisier than that (60 Hz with 1 m/s of velocity noise) the bias
//     stays larger (~0.3 G at W / 2).
// Plateaus and steps held at least W (+ P / 2 on each side for the pre-smoother) come out
// exactly. Every pushed sample comes out once, in order, with its own timestamp, delayed by up
// to P / 2 + 2 W (<= W / 4 + 2 W). W <= 0 passes samples straight through.
#include <cstddef>

#include "core/excursion_filter.h"
#include "core/metrics.h"

namespace ft {

// Centred moving mean over [t - W/2, t + W/2] (plain mean of the samples in it), delayed by
// W / 2. Windows are truncated at the start of the stream and at flush().
class CentredMean {
 public:
  using Sample = ExcursionFilter::Sample;
  static constexpr std::size_t kMaxPending = ExcursionFilter::kMaxPending;

  // Changing the width completes the pending samples with the old one first. Resets the
  // adjustable range to this width.
  void setWidth(double widthS);
  double width() const { return 2.0 * half_; }
  // Keeps enough history for widths up to maxWidthS, so that adjustWidth() can change the
  // width without completing anything early: the samples not yet out use the new width.
  void setMaxWidth(double maxWidthS);
  void adjustWidth(double widthS);  // in (0, max width]
  void push(double t, double v);  // t must not decrease
  bool pop(Sample& out);
  void flush();  // completes every pending sample and starts afresh
  void reset();  // drops everything
  std::size_t pending() const { return buf_.size() - next_; }

 private:
  void emit();  // completes buf_[next_]

  double half_ = 0.0;
  double keepHalf_ = 0.0;  // history kept: max(half_, half of the max width)
  RingQueue<Sample> buf_;  // from the oldest sample a pending window needs to the newest
  std::size_t next_ = 0;   // index in buf_ of the oldest sample not yet emitted
  RingQueue<Sample> out_;
};

class SustainedFilter {
 public:
  using Sample = ExcursionFilter::Sample;

  explicit SustainedFilter(double widthS = 0.0) { setWidth(widthS); }
  // W in seconds. A change completes the pending samples with the old width first (see flush())
  // and sets the pre-smoothing width P to W / 4.
  void setWidth(double widthS);
  double width() const { return w_; }
  // P, clamped to [W / 4, W / 2]; applies from the samples not yet pre-smoothed, nothing is
  // completed early (see sustainedPreSmoothingS).
  void setPreSmoothing(double widthS);
  double preSmoothing() const { return pre_.width(); }
  static constexpr double kMinPreRatio = 0.25, kMaxPreRatio = 0.5;  // P / W

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

// Residual G noise the pre-smoothing aims at (1 sigma): sustained extremes ~0.1 G outward.
inline constexpr double kSustainedNoiseTargetG = 0.2;

// 1-sigma G noise of the mean of m consecutive least-squares slopes, each over n samples at
// interval dtS, per m/s of independent velocity noise: the live G path's raw G (a slope of n
// velocity samples, FlightMetrics::gRaw) after a pre-smoother of m samples.
double meanSlopeNoiseG(int n, int m, double dtS);

// Pre-smoothing width P for W on the live G path described by `filter`: W / 4 unless the input
// is velocity data with a measured noise (noiseReady); then the shortest P in [W / 4, W / 2]
// whose predicted residual noise (meanSlopeNoiseG with the acceleration window and the noise
// sigma) is at most kSustainedNoiseTargetG, or W / 2 when none is.
double sustainedPreSmoothingS(double widthS, const FilterState& filter);

}  // namespace ft
