#pragma once
// Spike gate: checks each sample for consistency with the recent motion *before* it enters the
// kinematics window. The acceleration is a least-squares fit over that window, so a single bad
// sample would bias every estimate for as long as it stays in it. Consistency only: the G value
// itself is never capped.
//   velocity input: the distance of the new velocity from the one predicted by the window
//     (newest sample + least-squares trend; constant velocity with a single sample), against
//     the larger of spikeDeviationG * g0 * dt (an acceleration change no aircraft makes in one
//     step), a multiple of the measured velocity noise, and a floor.
//   position only : the distance from where the window's quadratic fit predicts the aircraft,
//     against a threshold scaled to the fit's residual noise. (A two-point second difference
//     would amplify position noise by ~4 / dt^2: 1 cm of jitter reads as ~15 G at 60 fps.)
// Look-back (velocity input): a glitch below the threshold gets into the window, where it
// biases the next estimates and makes the next good sample look wrong. So a sample that misses
// the prediction by more than half the threshold is *suspect*, and:
//   - if it fits the trend from the window without its newest sample (or two; within half
//     the threshold), those were a glitch: they are removed (DropPrevious), and the new
//     sample accepted;
//   - else, if it passes the gate, it is used, but the published estimate leaves it out for
//     this frame (Hold): it may be a glitch the next sample will expose, or the start of a
//     real step (15 - 30 G in one frame), which the next sample confirms by moving further
//     away from the old trend. Real steps cost one frame of delay; noise is never suspect.
// A sample that fails the gate is rejected. kRunLength rejections in a row are
// either a real jump (the rejected samples agree with each other: a respawn, or a step of
// more than spikeDeviationG in one frame), from which the estimators restart, or input
// noisier than the gate assumed (they scatter): the latest sample is then accepted as is.
#include <array>

#include "core/kinematics.h"
#include "core/sample.h"

namespace ft {

class SpikeGate {
 public:
  static constexpr int kRunLength = 3;
  static constexpr int kMaxLookBack = 2;  // glitches of up to this many samples are removed

  enum class Verdict {
    Accept,        // consistent (or nothing to compare with yet, or noise): use it
    Hold,          // suspect: use it, but publish the estimate without it until the next sample
    DropPrevious,  // the newest dropCount() window samples were a glitch: remove them, use this one
    Reject,        // glitch: drop this sample
    Jump,          // a run of agreeing rejections: restart the estimators from run()
  };

  // spikeDeviationG: PipelineConfig::spikeDeviationG. velocityNoiseSigma: the input's measured
  // per-axis velocity noise (NoiseEstimator::sigma()); noiseKnown: that measurement is final
  // (NoiseEstimator::ready()). The look-back needs it: only known noise is never suspect.
  Verdict check(const RawFlightSample& s, double dt, const KinematicsEstimator& kinematics,
                double spikeDeviationG, double velocityNoiseSigma, bool noiseKnown);

  // After Jump: the run of rejected samples, oldest first, ending with the sample just checked.
  const std::array<RawFlightSample, kRunLength>& run() const { return run_; }
  // After DropPrevious: how many of the newest window samples to remove (1 - kMaxLookBack).
  int dropCount() const { return dropCount_; }
  // Forgets the current run (the estimators restarted).
  void clear() { runSize_ = 0; }

 private:
  bool runAgrees(double limit) const;

  std::array<RawFlightSample, kRunLength> run_{};
  int runSize_ = 0;
  int dropCount_ = 0;
};

}  // namespace ft
