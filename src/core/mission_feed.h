#pragma once
// MissionFeed: the sustained-G path, from the pipeline's valid metrics to
// MissionTracker::addSustained (the peak statistics go to MissionTracker::add directly, undelayed).
//
//   live G -> SustainedFilter (PipelineConfig::minExcursionS = W) -> high-G episode timer
//          -> MissionTracker::addSustained, at the mission time of the sample it came from
//
// With W > 0 the filter works on the unsmoothed G (FlightMetrics::gRaw): the display EMA would
// stretch every bump by its tail. With W = 0 the live G passes straight through.
//
// The filter is fed continuously, in every phase but Paused, so that an excursion in progress
// when a mission starts is judged with the samples before it; the mission only takes outputs
// of samples pushed while it was Active. The stream restarts after a discontinuity (gap,
// pause, teleport, G mode change, reset) and when W changes: the outputs of the first W after
// a restart are unconfirmed (an excursion in progress there looks sustained), so they make
// no new sustained extremes and start no episode (an episode already running carries on, so
// changing W mid-mission does not split it).
#include <cstdint>

#include "core/excursion_filter.h"
#include "core/high_g_timer.h"
#include "core/mission_tracker.h"
#include "core/pipeline.h"
#include "core/sustained_filter.h"

namespace ft {

class MissionFeed {
 public:
  // Thresholds and W. A change of W completes the pending samples (into `mission`) with the
  // old width and restarts the stream; the current episode goes on.
  void configure(const PipelineConfig& cfg, MissionTracker& mission);
  // One sample with valid metrics, in any phase but Paused. stepDt: time since the previous
  // one. inMission: pushed while the mission is Active (after mission.addTime()).
  void add(MissionTracker& mission, const FlightMetrics& m, double stepDt, bool inMission);
  // A mission opened: its episodes start afresh (an episode running into it counts once
  // it is confirmed anew).
  void missionStarted() { ++epoch_; }
  // Mission end: hands every pending sample of it to `mission` (windows truncated, which can
  // only remove an excursion at the very end) and restarts the stream.
  void flush(MissionTracker& mission) { restart(mission, true, false); }
  // Discontinuity. keepPending: complete the pending samples into `mission` first (gap,
  // pause, teleport), else drop them (G mode change: other quantity). endEpisode: the aircraft
  // state jumped (teleport) or the values changed meaning.
  void restart(MissionTracker& mission, bool keepPending, bool endEpisode);
  // Drops everything (pipeline reset).
  void clear();

 private:
  struct Pending {
    double missionT = 0.0;  // mission time of the sample (inMission only)
    double missionDt = 0.0; // mission time it added
    std::uint32_t epoch = 0;
    bool inMission = false;
  };
  void drain(MissionTracker& mission);

  SustainedFilter filter_;
  RingQueue<Pending> pending_;  // payloads of the samples inside the filter, same order
  HighGTimer highG_;
  double streamT_ = 0.0;       // filter time: sum of stepDt over the samples pushed
  double confirmFromT_ = 0.0;  // outputs before this filter time are unconfirmed
  bool restarted_ = true;      // the next sample starts a new stream
  std::uint32_t epoch_ = 0;    // missions started
  std::uint32_t timerEpoch_ = 0;  // mission the episode timer belongs to
};

}  // namespace ft
