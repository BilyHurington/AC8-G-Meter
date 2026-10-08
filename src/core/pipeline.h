#pragma once
// TelemetryPipeline: RawFlightSample stream in -> MetricsSnapshot out.
//
//   push(sample)
//     -> sanitize (non-finite, time order, gaps, teleports)
//     -> kinematics (acceleration from timestamped velocity / position, least squares)
//     -> load factor in body axes -> median + EMA smoothing -> onset rate
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
  double accelWindowS = 0.08;       // least-squares window for acceleration
  double onsetWindowS = 0.15;       // least-squares window for dG/dt
  double smoothingTauS = 0.10;      // EMA time constant applied to G (0 = off)
  int medianWindow = 5;             // samples, odd, 1 = off. Removes single-sample spikes.
  double highGEnter = 6.0;          // sustained high-G episode starts at >= this
  double highGExit = 5.5;           // ... and ends below this (hysteresis)
  double maxGapS = 0.25;            // dt above this resets differentiators (hitch/pause)
  double plausibleGLimit = 15.0;    // |G| above this is treated as a data glitch
  double teleportToleranceM = 50.0; // |dp - v*dt| above this is treated as a teleport
  double minSpeedForAoa = 20.0;     // m/s
  double timelineHz = 10.0;         // mission report timeline resolution
};

// Hook for anything that wants every metrics update (statistics exporters, and later
// the pilot physiology model: blackout / redout / G-LOC).
class IMetricsConsumer {
 public:
  virtual ~IMetricsConsumer() = default;
  // Called for every accepted sample while phase is Active or None (not Paused).
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

  // Applies new tuning. Filter state is reset only if a window/size parameter changed;
  // mission statistics are always preserved.
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
