#pragma once
// TelemetryPipeline: RawFlightSample stream in -> MetricsSnapshot out.
//
//   push(sample)
//     -> sanitize (non-finite, time order, gaps, teleports)
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
  double smoothingTauS = 0.05;      // EMA time constant applied to G (0 = off)
  // Median filter length in *samples* (odd, 1 = off); removes single-sample spikes. Its
  // delay is (N - 1) / 2 frames, so it depends on frame rate: ~33 ms at 60 fps for N = 5.
  int medianWindow = 5;
  double highGEnter = 6.0;          // sustained high-G episode starts at >= this
  double highGExit = 5.5;           // ... and ends below this (hysteresis)
  double maxGapS = 0.25;            // dt above this resets differentiators (hitch/pause)
  // |load factor| above this is treated as a data glitch: the sample's G is not used and
  // the last valid filtered values are held for it.
  double plausibleGLimit = 15.0;
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
  // (non-finite, bad timestamp, spike) produce no call; a sample whose G was implausible
  // produces one with the held values.
  virtual void onMetrics(const FlightMetrics& metrics, MissionPhase phase) = 0;
  virtual void onMissionStarted() {}
  virtual void onMissionEnded(const MissionReport& /*report*/) {}
  virtual void onReset() {}  // pipeline reset / discontinuity (teleport, respawn)
};

class TelemetryPipeline {
 public:
  explicit TelemetryPipeline(const PipelineConfig& config = {});
  ~TelemetryPipeline();
  TelemetryPipeline(const TelemetryPipeline&) = delete;
  TelemetryPipeline& operator=(const TelemetryPipeline&) = delete;

  // Applies new tuning. Filter state is reset only if a window/size parameter or the G
  // mode changed. Mission statistics are preserved, except that a G mode change clears
  // the G-derived ones (extremes, onset, histogram, high-G, timeline) and the recent
  // history, since Nz and |n| values must not be mixed. Duration, max speed, max AoA and
  // sample counts are kept.
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
