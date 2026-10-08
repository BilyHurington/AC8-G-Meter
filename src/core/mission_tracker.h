#pragma once
// Accumulates MissionStats and the resampled G timeline for one mission at a time and
// turns them into an immutable MissionReport. Knows nothing about phases: the pipeline
// decides when a mission starts, which samples count, and when it ends.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/metrics.h"

namespace ft {

class MissionTracker {
 public:
  // Opens a new mission with empty statistics.
  void start(double timelineHz);
  bool open() const { return open_; }

  void countAccepted() { ++stats_.acceptedSamples; }
  void countRejected() { ++stats_.rejectedSamples; }

  // Adds one valid sample. dtS is the time since the previous accepted sample; mission
  // time (and every "...AtS") is the sum of these, so pauses and gaps are excluded.
  // highGEntered: a high-G episode started on this sample.
  void add(const FlightMetrics& m, double dtS, bool highGEntered);

  // Closes the mission. Live stats stay readable until the next start().
  MissionReport finish(bool aborted);

  // Clears live statistics and timeline without closing the mission.
  void clearStats();

  const MissionStats& stats() const { return stats_; }

 private:
  // Bounds memory for absurdly long sessions (~28 h at 10 Hz).
  static constexpr std::size_t kMaxTimelinePoints = 1'000'000;

  MissionStats stats_;
  std::vector<float> timeline_;
  double timelineHz_ = 10.0;
  std::uint32_t nextId_ = 1;
  bool open_ = false;
};

}  // namespace ft
