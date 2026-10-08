#pragma once
// MissionArchiveRecorder: saves every finished mission to a MissionArchive straight from the
// pipeline (IMetricsConsumer::onMissionEnded, i.e. on the game thread), so saving never
// depends on the UI drawing a frame or noticing the report: two missions ending between two
// frames, or a pipeline recreated with ids restarting at 1, are all saved.
//
// The host registers it with the pipeline (addConsumer) and keeps it alive until it is
// removed or the pipeline is destroyed. MissionArchive::saveAsync only queues the job, so the
// game thread never waits on file I/O. The report is shared with the pipeline (the same
// object as MetricsSnapshot::lastReport), never copied, so SaveStatus::report can be matched
// to the report on screen by pointer.
#include <atomic>

#include "core/pipeline.h"
#include "storage/mission_archive.h"

namespace ft {

class MissionArchiveRecorder final : public IMetricsConsumer {
 public:
  explicit MissionArchiveRecorder(MissionArchive& archive, bool enabled = true) : archive_(archive), enabled_(enabled) {}

  // Mirrors Settings::history.saveMissions. Any thread (the UI updates it every frame).
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  void onMetrics(const FlightMetrics&, MissionPhase) override {}
  // Saves missions with G data (aborted ones too) while enabled.
  void onMissionEnded(const std::shared_ptr<const MissionReport>& report) override;

 private:
  MissionArchive& archive_;
  std::atomic<bool> enabled_;
};

}  // namespace ft
