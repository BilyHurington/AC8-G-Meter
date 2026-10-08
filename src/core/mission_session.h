#pragma once
// MissionSession: the mission state machine around MissionTracker. Follows the phase reported
// by the source, opens a mission on entering Active, closes it on Ended (completed) or None
// (aborted), publishes the MissionReport (lastReport) and tells the consumers. Valid metrics
// reach the tracker directly (peak statistics) and through the MissionFeed (sustained ones).
#include <memory>
#include <string>
#include <vector>

#include "core/metrics.h"
#include "core/mission_feed.h"
#include "core/mission_tracker.h"
#include "core/pipeline.h"

namespace ft {

class MissionSession {
 public:
  // Consumers are notified of mission start / end; the list is the pipeline's (not owned).
  explicit MissionSession(const std::vector<IMetricsConsumer*>& consumers) : consumers_(consumers) {}

  struct Transition {
    bool changed = false;  // the phase differs from the previous sample's
    bool resumed = false;  // left Paused (game time stopped: the aircraft state may have jumped)
    bool started = false;  // a new mission was opened
  };
  // Phase of the next sample. Starts / finishes missions; cfg is recorded in the report.
  Transition setPhase(MissionPhase next, const PipelineConfig& cfg);
  MissionPhase phase() const { return phase_; }
  bool active() const { return phase_ == MissionPhase::Active; }

  void configure(const PipelineConfig& cfg) { feed_.configure(cfg, tracker_); }
  // An accepted sample in any phase but Paused. Mission time advances by missionDtS while
  // Active; valid metrics count in the mission (Active) and feed the sustained path (always).
  void add(const FlightMetrics& m, double stepDtS, double missionDtS);
  // Discontinuity in the G stream (gap, pause, teleport): the sustained path restarts
  // (MissionFeed::restart). endEpisode: the aircraft state jumped.
  void restartSustained(bool endEpisode) { feed_.restart(tracker_, true, endEpisode); }
  // G mode changed: the G statistics restart (MissionTracker::resetGStats).
  void resetGStats();
  // Pipeline reset: live statistics cleared, the mission stays open.
  void clear();

  MissionTracker& tracker() { return tracker_; }
  const MissionStats& stats() const { return tracker_.stats(); }
  const std::shared_ptr<const MissionReport>& lastReport() const { return lastReport_; }
  void setLabel(std::string label) { label_ = std::move(label); }

 private:
  void start(const PipelineConfig& cfg);
  void finish(bool aborted, const PipelineConfig& cfg);

  const std::vector<IMetricsConsumer*>& consumers_;
  MissionPhase phase_ = MissionPhase::None;
  MissionTracker tracker_;
  MissionFeed feed_;  // valid metrics -> excursion filter -> tracker
  std::shared_ptr<const MissionReport> lastReport_;
  std::string label_;
};

}  // namespace ft
