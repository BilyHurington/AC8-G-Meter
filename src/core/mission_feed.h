#pragma once
// MissionFeed: the path from the pipeline's valid metrics to the MissionTracker. With
// PipelineConfig::minExcursionS > 0, G passes through an ExcursionFilter first, so bumps and
// dips shorter than that never reach the mission statistics (MAX / MIN G, histogram, onset
// extremes, high-G episodes) or the trace. The live G and the HUD's high-G timer are not
// affected. The filter works on the unsmoothed G (FlightMetrics::gRaw): the display EMA
// would stretch every bump by its tail, so a 0.15 s bump would survive a 0.2 s filter.
//
// Samples reach the tracker ~2 W late, with their own mission time, so "max G at" times stay
// exact. Onset rate and the high-G timer are recomputed from the filtered G; speed, AoA and
// altitude come from the same (delayed) sample. Mission duration is counted by the pipeline
// as before, undelayed. With minExcursionS = 0 samples pass straight through, unchanged.
#include "core/excursion_filter.h"
#include "core/filters.h"
#include "core/high_g_timer.h"
#include "core/mission_tracker.h"
#include "core/pipeline.h"

namespace ft {

class MissionFeed {
 public:
  // Thresholds and filter width. A width change first completes the pending samples (into
  // `mission`) with the old width.
  void configure(const PipelineConfig& cfg, MissionTracker& mission);
  // One sample with valid metrics, at the current mission time (after mission.addTime()).
  // liveHighGEntered: the live high-G timer's verdict, used as-is when the filter is off.
  void add(MissionTracker& mission, const FlightMetrics& m, double dtS, bool liveHighGEntered,
           double onsetWindowS);
  // Mission end: hands every pending sample to `mission`.
  void flush(MissionTracker& mission);
  // Drops pending samples and restarts the filter (mission start, G mode change, reset).
  void clear();

 private:
  struct Pending {
    FlightMetrics m;
    double dtS = 0.0;
  };
  void drain(MissionTracker& mission);

  ExcursionFilter filter_;
  RingQueue<Pending> pending_;  // payloads of the samples inside the filter, same order
  HighGTimer highG_;
  TimedWindow<double> onset_;
  double onsetWindowS_ = 0.15;
};

}  // namespace ft
