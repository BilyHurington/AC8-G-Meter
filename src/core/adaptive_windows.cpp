#include "core/adaptive_windows.h"

#include <algorithm>
#include <cmath>

#include "core/math.h"

namespace ft {

const FilterState& AdaptiveWindows::select(const PipelineConfig& cfg, const NoiseEstimator& noise,
                                           FilterState::Input input, double stepDt) {
  FilterState& f = state_;
  const double dt = noise.sampleInterval();
  const double minWindowS = std::max(kMinWindowSamples * dt, cfg.adaptiveMinWindowS);
  f.input = input;
  const bool velocityInput = input != FilterState::Input::PositionOnly;
  if (velocityInput && noise.ready() && dt > 0.0 && cfg.adaptiveTargetG > 0.0) {
    const double sigma = noise.sigma();
    const double target = cfg.adaptiveTargetG * kG0;
    // T* only matters up to where both the window and tau are at their limits.
    const double maxUseful = std::max(
        {cfg.adaptiveMaxWindowS, cfg.adaptiveTauRatio > 0.0 ? cfg.smoothingTauS / cfg.adaptiveTauRatio : 0.0,
         minWindowS});
    const double ideal = std::clamp(std::cbrt(12.0 * sigma * sigma * dt / (target * target)), minWindowS, maxUseful);
    // Fast attack, slow release: more noise needs the longer window now, while cleaner data
    // can wait to be sure. Tracked even while adaptive filtering is off, so switching it on
    // is seamless.
    if (ideal >= idealWindowS_)
      idealWindowS_ = ideal;
    else
      idealWindowS_ += (1.0 - std::exp(-stepDt / kReleaseTauS)) * (ideal - idealWindowS_);
  }
  f.sampleRateHz = dt > 0.0 ? 1.0 / dt : 0.0;
  f.velocityNoiseMS = noise.sigma();
  f.noiseReady = noise.ready();
  f.adaptive = cfg.adaptiveFiltering && velocityInput && idealWindowS_ > 0.0;
  if (f.adaptive) {
    // The sample floor wins at very low rates (< ~31 Hz with the default limit).
    f.accelWindowS = std::max(std::min(idealWindowS_, cfg.adaptiveMaxWindowS), minWindowS);
    f.smoothingTauS = std::clamp(cfg.adaptiveTauRatio * idealWindowS_, 0.0, cfg.smoothingTauS);
    f.onsetWindowS =
        std::max(std::min(cfg.adaptiveOnsetRatio * f.accelWindowS, cfg.onsetWindowS), kOnsetMinSamples * dt);
  } else {
    f.accelWindowS = velocityInput ? cfg.accelWindowS : cfg.accelWindowPositionOnlyS;
    f.smoothingTauS = cfg.smoothingTauS;
    f.onsetWindowS = cfg.onsetWindowS;
  }
  return f;
}

void AdaptiveWindows::noiseAboveGate(const PipelineConfig& cfg) {
  if (idealWindowS_ > 0.0) idealWindowS_ = std::max(idealWindowS_, cfg.adaptiveMaxWindowS);
}

}  // namespace ft
