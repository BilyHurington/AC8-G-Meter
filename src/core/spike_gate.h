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
//
// Look-back (velocity input, once the noise is measured): a glitch below the gate threshold
// would get into the window and show for a few frames. So a sample that misses the prediction
// by more than the *suspect bound* (an acceleration change of kSuspectG in one step plus a
// noise margin, at most half the gate threshold) is *held*: it enters the window, but the
// published estimate stays the one from before it (KinematicsEstimator::addHeld). The next
// sample decides, by its deviation e from the trend of the window without the held samples,
// compared with those of the held samples:
//   - |e| <= half the smallest of theirs: back on the old trend, so the held samples were a
//     glitch: they are removed (DropPrevious), and the new sample used;
//   - e within half of it from the newest held deviation: still at the held level, as after a
//     real velocity step (a one-frame G impulse) or a longer glitch: held as well, up to
//     kMaxLookBack samples. At the limit the level is confirmed (Confirm): the estimate is
//     published as of the first held sample, so that a short window still contains the step,
//     and the later samples show from the next frame;
//   - e further out in the same direction (by half the suspect bound): a real change of
//     acceleration (a G step): confirmed, everything is used (Accept);
//   - otherwise the samples scatter: noise the gate underestimated: everything is used (Noise).
// So real G steps cost at most one frame of delay, one-frame G impulses kMaxLookBack frames
// (shown in full), and glitches of up to kMaxLookBack samples never reach the display; a
// longer burst reads as a real two-way velocity step (with 10 % of the samples glitched at
// random, about once per 10^4 samples). Noise is never suspect once measured.
//
// A sample that fails the gate is rejected. kRunLength rejections in a row are either a real
// jump (the rejected samples agree with each other: a respawn, or a step of more than
// spikeDeviationG in one frame), from which the estimators restart, or input noisier than the
// gate assumed (they scatter): the latest sample is then used (Noise). Two rejections in a row
// that deviate in different directions already show such noise (runScatters()), and so does a
// busy gate (busy(): most recent samples held or rejected), while real glitches are sparse:
// the pipeline then lengthens its window at once, before the noise estimate has caught up.
#include <array>
#include <optional>

#include "core/kinematics.h"
#include "core/sample.h"

namespace ft {

class SpikeGate {
 public:
  static constexpr int kRunLength = 3;
  static constexpr int kMaxLookBack = 3;  // glitches of up to this many samples are removed
  // Suspect bound, velocity input: an acceleration change of this many G in one step (plus
  // kSuspectSigmas times the measured noise), at most half the gate threshold. Below it a
  // glitch shows as at most ~kSuspectG / 2 in a 3-sample window; real G steps above it cost one
  // frame of delay.
  static constexpr double kSuspectG = 5.0;
  static constexpr double kSuspectSigmas = 8.0;
  // busy(): at least kBusyEvents of the last kBusyWindow samples were held or rejected (a
  // one-frame G impulse is held kMaxLookBack times: fewer).
  static constexpr int kBusyWindow = 8;
  static constexpr int kBusyEvents = 4;

  enum class Verdict {
    Accept,        // consistent (or nothing to compare with yet): use it; confirms held samples
    Hold,          // suspect: add it with KinematicsEstimator::addHeld (estimates unchanged)
    Confirm,       // the held samples are real: KinematicsEstimator::commitHeld, then addHeld it
    DropPrevious,  // the held samples (resolved()) were a glitch: remove them, use this one
    Reject,        // glitch: drop this sample
    Jump,          // a run of agreeing rejections: restart the estimators from run()
    Noise,         // scattered rejections or held samples: noise above the gate's assumption; use it
  };

  // spikeDeviationG: PipelineConfig::spikeDeviationG. velocityNoiseSigma: the input's measured
  // per-axis velocity noise (NoiseEstimator::sigma()); noiseKnown: that measurement is final
  // (NoiseEstimator::ready()). The look-back needs it: only known noise is never suspect.
  // The caller applies the verdict to `kinematics` before the next check.
  Verdict check(const RawFlightSample& s, double dt, const KinematicsEstimator& kinematics,
                double spikeDeviationG, double velocityNoiseSigma, bool noiseKnown);

  // After Jump: the run of rejected samples, oldest first, ending with the sample just checked.
  const std::array<RawFlightSample, kRunLength>& run() const { return run_; }
  // Rejections in the current run (after Reject: the sample just checked is run()[runSize()-1]).
  int runSize() const { return runSize_; }
  // After Reject: the rejections of the current run deviate in different directions, so they
  // are noise the gate underestimates rather than the start of a jump.
  bool runScatters() const { return runSize_ >= 2 && scatters_; }
  // Most of the recent samples were held or rejected: the input is noisier than the noise
  // estimate says (it lags noise increases), or glitches are not sparse.
  bool busy() const { return busyEvents_ >= kBusyEvents; }
  // Held samples: the newest held() window samples (after Hold, the sample just checked is the
  // held()-th).
  int held() const { return held_; }
  // After DropPrevious or Confirm: how many held samples (the newest window samples) it
  // concerns (1 - kMaxLookBack).
  int resolved() const { return resolved_; }
  // Forgets the current run and the held samples (the estimators restarted).
  void clear() {
    runSize_ = 0;
    held_ = 0;
  }

 private:
  Verdict decide(const RawFlightSample& s, double dt, const KinematicsEstimator& kinematics,
                 double spikeDeviationG, double velocityNoiseSigma, bool noiseKnown);
  // Held samples and suspects (velocity input, known noise); nothing if the gate decides.
  std::optional<Verdict> lookBack(const RawFlightSample& s, double miss, double limit, double bound,
                                  const KinematicsEstimator& kinematics);
  Verdict hold(const RawFlightSample& s);
  bool runAgrees(double limit) const;

  std::array<RawFlightSample, kRunLength> run_{};
  std::array<Vec3, kRunLength> runMiss_{};  // deviation of each rejected sample from its prediction
  int runSize_ = 0;
  bool scatters_ = false;
  std::array<double, kMaxLookBack> heldT_{};
  std::array<Vec3, kMaxLookBack> heldV_{};
  int held_ = 0;
  int resolved_ = 0;
  std::array<bool, kBusyWindow> events_{};  // ring: held or rejected
  int eventIndex_ = 0, busyEvents_ = 0;
};

}  // namespace ft
