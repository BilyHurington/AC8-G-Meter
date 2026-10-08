#include "core/sample_ledger.h"

namespace ft {

void SampleLedger::setPhase(MissionPhase phase, bool changed) {
  counting_ = phase == MissionPhase::Active;
  if (changed) {
    heldInMission_.fill(false);
    runInMission_.fill(false);
  }
}

void SampleLedger::reject(std::uint64_t DataQuality::*counter) {
  ++(quality_.*counter);
  if (counting_) mission_.countRejected();
}

void SampleLedger::rejectSpike(int runSize) {
  reject(&DataQuality::rejectedSpike);
  runInMission_[static_cast<std::size_t>(runSize - 1)] = counting_;
}

void SampleLedger::accept(SpikeGate::Verdict verdict, int gateHeld) {
  ++quality_.accepted;
  if (counting_) mission_.countAccepted();
  if (verdict == SpikeGate::Verdict::Hold) heldInMission_[static_cast<std::size_t>(gateHeld - 1)] = counting_;
}

void SampleLedger::countExtremeG() {
  ++quality_.extremeG;
  if (counting_) mission_.countExtremeG();
}

void SampleLedger::heldWereGlitches(int count) {
  for (int i = count - 1; i >= 0; --i) recountAsRejected(heldInMission_[static_cast<std::size_t>(i)]);
}

void SampleLedger::runWasReal() {
  for (std::size_t i = 0; i + 1 < runInMission_.size(); ++i) recountAsAccepted(runInMission_[i]);
}

void SampleLedger::recountAsRejected(bool inMission) {
  --quality_.accepted;
  ++quality_.rejectedSpike;
  if (inMission && mission_.open()) {
    mission_.uncountAccepted();
    mission_.countRejected();
  }
}

void SampleLedger::recountAsAccepted(bool inMission) {
  --quality_.rejectedSpike;
  ++quality_.accepted;
  if (inMission && mission_.open()) {
    mission_.uncountRejected();
    mission_.countAccepted();
  }
}

}  // namespace ft
