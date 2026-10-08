#pragma once
// GatedKinematics: the acceleration estimate behind the live G, with the spike gate in front of
// it. Owns the least-squares KinematicsEstimator, the attitude history spanning its window,
// the SpikeGate, and the last accepted sample (for dt and teleport checks).
//
// The gate decides on each sample before it enters the window (SpikeGate); this class applies
// the window side of every verdict. The pipeline does the bookkeeping (sample counts, resets,
// consumers) around it.
#include <optional>

#include "core/filters.h"
#include "core/kinematics.h"
#include "core/metrics.h"
#include "core/noise_estimator.h"
#include "core/pipeline.h"
#include "core/spike_gate.h"

namespace ft {

class GatedKinematics {
 public:
  // ---- last accepted sample: survives restart(), cleared by forget() ---------------------
  bool hasLast() const { return hasLast_; }
  double lastT() const { return lastT_; }
  FilterState::Input inputKind() const;
  void recordLast(const RawFlightSample& s);
  void forget() { hasLast_ = false; }

  // Restarts the acceleration window, its attitude history and the gate (a velocity jump
  // restarts only these; gaps and teleports restart them too).
  void restart();
  // Window lengths for the next sample (AdaptiveWindows).
  void setWindows(const FilterState& f, const PipelineConfig& cfg);

  // Position jump inconsistent with the motion (respawn, checkpoint). Position-only input is
  // measured against the same fit prediction as the spike gate.
  bool isTeleport(const RawFlightSample& s, double dt, double toleranceM) const;

  // Runs the spike gate. On DropPrevious the held samples (gate().resolved()) are removed
  // from the window here. On Reject the caller drops the sample; on Jump it restarts and calls
  // startFromRun(); otherwise it adds the sample with add().
  SpikeGate::Verdict check(const RawFlightSample& s, double dt, double spikeDeviationG, const NoiseEstimator& noise);
  // Signs of noise above what the estimate says (it lags increases), after `verdict`:
  // rejections that scatter instead of agreeing like a real jump's, or a gate busy with most
  // samples.
  bool noiseSuspected(const RawFlightSample& s, SpikeGate::Verdict verdict) const;
  // After Jump: the rejected run agrees on the new state, so the window starts from it (all but
  // its last sample, the one just checked, which the caller adds). The estimate is valid again
  // at once.
  void startFromRun();
  // Adds an accepted sample according to the gate's verdict (held samples stay out of the
  // published estimate until confirmed).
  void add(const RawFlightSample& s, SpikeGate::Verdict verdict);
  const SpikeGate& gate() const { return gate_; }

  // Load factor (specific force in G, what an accelerometer in the seat measures) in body axes,
  // projected on the attitude at the instant the acceleration estimate refers to; nothing until
  // the window has an estimate.
  std::optional<Vec3> loadFactorBody() const;
  // Speed, AoA and sideslip of `m` from the estimated velocity and the attitude of `s`.
  void airData(const RawFlightSample& s, double minSpeedForAoa, FlightMetrics& m) const;

 private:
  // Attitude at time t, interpolated between the two history samples around it.
  Basis attitudeAt(double t) const;

  KinematicsEstimator kinematics_;
  TimedWindow<Basis> attitudes_;  // attitude history spanning the kinematics window
  SpikeGate gate_;

  bool hasLast_ = false;
  double lastT_ = 0.0;
  Vec3 lastPos_;
  std::optional<Vec3> lastVel_;
};

}  // namespace ft
