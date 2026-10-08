#pragma once
// SampleLedger: counts every sample pushed outside a pause exactly once, as accepted or as
// rejected, in DataQuality and, while a mission is Active, in its MissionStats.
//
// The spike gate may revise its verdict on a sample later (look-back: held samples were a
// glitch; jump: a rejected run is used after all), and the counts follow, so that
// accepted + rejected always equals the samples pushed. For that the ledger remembers, for each
// sample the gate may still reclassify, whether it was counted in the open mission. A phase
// change forgets that: samples from before it were not counted in the mission as it is now.
#include <array>
#include <cstdint>

#include "core/metrics.h"
#include "core/mission_tracker.h"
#include "core/spike_gate.h"

namespace ft {

class SampleLedger {
 public:
  explicit SampleLedger(MissionTracker& mission) : mission_(mission) {}

  // Samples count in the mission while it is Active.
  void setPhase(MissionPhase phase, bool changed);

  DataQuality& quality() { return quality_; }
  const DataQuality& quality() const { return quality_; }
  // Clears the counters (pipeline reset).
  void reset() { quality_ = {}; }

  // A sample dropped for `counter` (non-finite, bad timestamp).
  void reject(std::uint64_t DataQuality::*counter);
  // The spike gate rejected the sample just checked, the runSize-th of its current run.
  void rejectSpike(int runSize);
  // An accepted sample. gateHeld: SpikeGate::held() after the verdict (for Hold).
  void accept(SpikeGate::Verdict verdict, int gateHeld);
  // Informational: an accepted sample above PipelineConfig::extremeGThreshold.
  void countExtremeG();
  // The gate's newest `count` held samples (already counted as accepted) were a glitch.
  void heldWereGlitches(int count);
  // The gate's rejected run (all but the sample just checked) was a real jump.
  void runWasReal();

 private:
  void recountAsRejected(bool inMission);
  void recountAsAccepted(bool inMission);

  MissionTracker& mission_;
  DataQuality quality_;
  bool counting_ = false;  // phase is Active
  std::array<bool, SpikeGate::kMaxLookBack> heldInMission_{};
  std::array<bool, SpikeGate::kRunLength> runInMission_{};
};

}  // namespace ft
