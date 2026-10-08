#include "core/mission_session.h"

#include <utility>

namespace ft {

MissionSession::Transition MissionSession::setPhase(MissionPhase next, const PipelineConfig& cfg) {
  Transition tr;
  const MissionPhase prev = phase_;
  phase_ = next;
  tr.changed = next != prev;
  tr.resumed = prev == MissionPhase::Paused && next != MissionPhase::Paused;
  switch (next) {
    case MissionPhase::Active:
      if (!tracker_.open()) {
        start(cfg);
        tr.started = true;
      }
      break;
    case MissionPhase::Ended:
      if (tracker_.open()) finish(false, cfg);
      break;
    case MissionPhase::None:
      if (tracker_.open()) finish(true, cfg);
      break;
    case MissionPhase::Paused:
      break;
  }
  return tr;
}

void MissionSession::start(const PipelineConfig& cfg) {
  tracker_.start(cfg.traceHz);
  feed_.missionStarted();  // the sustained path keeps its history: no restart
  for (std::size_t i = 0; i < consumers_.size(); ++i) consumers_[i]->onMissionStarted();
}

void MissionSession::finish(bool aborted, const PipelineConfig& cfg) {
  feed_.flush(tracker_);  // the samples still inside the excursion filter
  MissionReport r = tracker_.finish(aborted);
  r.gMode = cfg.gMode;
  r.highGEnter = cfg.highGEnter;
  r.highGExit = cfg.highGExit;
  r.extremeGThreshold = cfg.extremeGThreshold;
  r.minExcursionS = cfg.minExcursionS > 0.0 ? cfg.minExcursionS : 0.0;
  r.label = label_;
  lastReport_ = std::make_shared<const MissionReport>(std::move(r));
  for (std::size_t i = 0; i < consumers_.size(); ++i) consumers_[i]->onMissionEnded(lastReport_);
}

void MissionSession::add(const FlightMetrics& m, double stepDtS, double missionDtS) {
  if (active()) {
    tracker_.addTime(missionDtS);
    if (m.valid) tracker_.add(m, stepDtS);
  }
  if (m.valid) feed_.add(tracker_, m, stepDtS, active());
}

void MissionSession::resetGStats() {
  if (tracker_.open()) tracker_.resetGStats();
  feed_.restart(tracker_, false, true);  // pending values are of the other G mode
}

void MissionSession::clear() {
  tracker_.clearStats();
  feed_.clear();
}

}  // namespace ft
