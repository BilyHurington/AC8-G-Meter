#include "storage/mission_recorder.h"

#include <exception>
#include <memory>

namespace ft {

void MissionArchiveRecorder::onMissionEnded(const std::shared_ptr<const MissionReport>& report) {
  if (!enabled() || !report || !report->stats.hasGData) return;
  try {
    archive_.saveAsync(report);  // shared with the pipeline: no copy
  } catch (const std::exception&) {
    // Out of memory for the job: the mission is lost, the game goes on.
  }
}

}  // namespace ft
