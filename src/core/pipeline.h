#pragma once
// TelemetryPipeline: RawFlightSample stream in -> MetricsSnapshot out.
//
//   push(sample)
//     -> sanitize (non-finite, time order, gaps, teleports)
//     -> noise / sample-rate estimate (NoiseEstimator) -> adaptive windows
//     -> spike gate (SpikeGate: glitches, look-back, jumps)
//     -> kinematics (acceleration from timestamped velocity / position, least squares)
//     -> load factor in body axes (attitude at the acceleration's time)
//     -> median + EMA smoothing -> G per GMode -> onset rate
//     -> AoA / sideslip, sustained high-G timer
//     -> mission state machine + statistics
//     -> consumers (IMetricsConsumer)
//
// Robust to variable frame time. Not thread-safe: call from one thread and hand the
// snapshot to the render thread via LatestValue<MetricsSnapshot>.
#include <memory>
#include <vector>

#include "core/metrics.h"
#include "core/sample.h"

namespace ft {

struct PipelineConfig {
  GMode gMode = GMode::Nz;
  double accelWindowS = 0.08;       // least-squares window for acceleration (velocity input)
  // Same for position-only input: differentiating noisy positions twice needs a longer
  // window (more lag, ~windowS / 2) to keep the G noise comparable to velocity input.
  double accelWindowPositionOnlyS = 0.15;
  double onsetWindowS = 0.15;       // least-squares window for dG/dt
  // EMA time constant applied to G (0 = off). With adaptive filtering, the upper limit of the
  // adaptive tau.
  double smoothingTauS = 0.05;
  // Adaptive filtering (velocity input). The acceleration is the least-squares slope of
  // N = T / dt velocity samples over a window T, whose standard error with per-axis velocity
  // noise sigma is sigma * sqrt(12 / (N T^2)). The window that keeps it at adaptiveTargetG:
  //   T* = cbrt(12 sigma^2 dt / (adaptiveTargetG * g0)^2)
  // with sigma and dt measured from the data (NoiseEstimator). T* follows increases at once
  // and decreases over ~1 s. Then
  //   window = T* within [2.5 dt, adaptiveMaxWindowS], EMA tau = adaptiveTauRatio * T* up to
  //   smoothingTauS, onset window = adaptiveOnsetRatio * window within [4 dt, onsetWindowS],
  // so the adaptive filter is never slower than the fixed one: clean or high-rate data gets a
  // short window and little lag (clean 60 Hz: ~40 ms instead of ~105 ms), heavy noise converges
  // to the fixed windows. accelWindowS and smoothingTauS are the fallback until the noise
  // estimate is ready (~16 samples), and all three windows are used as-is when this is off.
  // Position-only input always uses the fixed windows: its acceleration is a second
  // derivative of noisy positions, whose noise follows a different law (~sigma_p / T^2.5)
  // and is not measured by the velocity noise estimate.
  bool adaptiveFiltering = true;
  double adaptiveTargetG = 0.06;     // G noise (1 sigma per axis) the window is sized for
  double adaptiveTauRatio = 0.4;
  double adaptiveMaxWindowS = 0.08;  // = accelWindowS: never a longer window than the fixed one
  double adaptiveOnsetRatio = 3.0;
  // Median filter length in *samples* (odd, 1 = off). Removes the single bad output frame of a
  // glitch the spike gate only catches one sample later (SpikeGate look-back), for one sample
  // of delay; a median of N adds (N - 1) / 2 frames.
  int medianWindow = 3;
  double highGEnter = 6.0;          // sustained high-G episode starts at >= this
  double highGExit = 5.5;           // ... and ends below this (hysteresis)
  double maxGapS = 0.25;            // dt above this resets differentiators (hitch/pause)
  // Spike gate (velocity input, see SpikeGate), in G: a sample is a data glitch if its
  // velocity misses the value predicted from the recent least-squares trend by more than this
  // acceleration deviation over one step (or by more than the input's own noise allows,
  // whichever is larger). A game glitch such as a +300 m/s velocity error reads as ~1800 G at
  // 60 fps. A real step larger than this is accepted after SpikeGate::kRunLength samples
  // (DataQuality::resetsJump). Consistency only: there is no cap on the G value itself.
  double spikeDeviationG = 30.0;
  // Informational only: samples with |load factor| above this are counted in
  // DataQuality::extremeG / MissionStats::extremeGSamples. They are used like any other.
  double extremeGThreshold = 15.0;
  double teleportToleranceM = 50.0; // |dp - v*dt| above this is treated as a teleport
  double minSpeedForAoa = 20.0;     // m/s
  double timelineHz = 10.0;         // mission report timeline resolution
};

// Hook for anything that wants every metrics update (statistics exporters, and later
// the pilot physiology model: blackout / redout / G-LOC).
class IMetricsConsumer {
 public:
  virtual ~IMetricsConsumer() = default;
  // Called for every processed sample in every phase except Paused (so effects can keep
  // fading out on the results screen while Ended). Samples dropped before the estimators
  // (non-finite, bad timestamp, spike) produce no call.
  virtual void onMetrics(const FlightMetrics& metrics, MissionPhase phase) = 0;
  virtual void onMissionStarted() {}
  virtual void onMissionEnded(const MissionReport& /*report*/) {}
  // Pipeline reset / discontinuity (teleport, respawn). Not called for a velocity-only jump
  // (DataQuality::resetsJump), which may be a real manoeuvre.
  virtual void onReset() {}
};

class TelemetryPipeline {
 public:
  explicit TelemetryPipeline(const PipelineConfig& config = {});
  ~TelemetryPipeline();
  TelemetryPipeline(const TelemetryPipeline&) = delete;
  TelemetryPipeline& operator=(const TelemetryPipeline&) = delete;

  // Applies new tuning. Filter state is reset only if a fixed window/size parameter or the
  // G mode changed (not by the adaptive-filtering fields: windows trim by time). Mission
  // statistics are preserved, except that a G mode change clears the G-derived ones
  // (extremes, onset, histogram, high-G, timeline) and the recent history, since Nz and |n|
  // values must not be mixed. Duration, max speed, max AoA and sample counts are kept.
  void setConfig(const PipelineConfig& config);
  const PipelineConfig& config() const;

  void push(const RawFlightSample& sample);

  // Clears estimator state, live mission statistics and history. Keeps lastReport.
  void reset();

  // Cheap copy of the current state (a few KB). Safe to keep after further push() calls.
  MetricsSnapshot snapshot() const;

  // Consumers are not owned and must outlive the pipeline or be removed first.
  void addConsumer(IMetricsConsumer* consumer);
  void removeConsumer(IMetricsConsumer* consumer);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ft
