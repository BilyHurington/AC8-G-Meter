#pragma once
// AdaptiveWindows: picks the filter windows of the live G path (least-squares acceleration
// window, EMA time constant, onset window) for the next sample, from the input kind and the
// measured velocity noise and sample rate (see PipelineConfig::adaptiveFiltering).
//
// The ideal velocity window T* = cbrt(12 sigma^2 dt / (adaptiveTargetG * g0)^2) follows
// increases at once and decreases with a ~1 s time constant ("fast attack, slow release").
// Only the window lengths change here, never estimator state: every window trims by time, so
// a new length applies from the next sample.
#include "core/metrics.h"
#include "core/noise_estimator.h"
#include "core/pipeline.h"

namespace ft {

class AdaptiveWindows {
 public:
  // The window never spans fewer than this many sample intervals (acceleration / onset), and
  // follows decreases of the ideal window with this time constant.
  static constexpr double kMinWindowSamples = 2.5;
  static constexpr double kOnsetMinSamples = 4.0;
  static constexpr double kReleaseTauS = 1.0;

  // Windows for the next sample. input: kind of the last accepted sample; stepDt: time since
  // the sample before it (drives the slow release).
  const FilterState& select(const PipelineConfig& cfg, const NoiseEstimator& noise, FilterState::Input input,
                            double stepDt);
  // The spike gate found the input noisier than the noise estimate says (scattered
  // rejections): take the longest adaptive window now instead of when the estimate has caught
  // up (~0.1 s); the slow release brings it back if the noise does not last.
  void noiseAboveGate(const PipelineConfig& cfg);
  // Forgets the ideal window (the noise estimate restarts with it).
  void reset() { idealWindowS_ = 0.0; }

  const FilterState& state() const { return state_; }

 private:
  double idealWindowS_ = 0.0;  // smoothed ideal velocity window T*, 0 until the noise is known
  FilterState state_;          // windows in use (published in the snapshot)
};

}  // namespace ft
