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
  void countExtremeG() { ++stats_.extremeGSamples; }

  // Advances mission time by the game time since the previous accepted sample. Called for
  // every accepted Active sample, valid metrics or not, so estimator warm-up after a reset
  // never shortens the mission. Pauses are excluded because game time stops while paused.
  void addTime(double dtS);

  // Adds one sample with valid metrics, at the current mission time (call addTime first).
  // dtS weights the histogram and high-G totals. highGEntered: an episode started here.
  void add(const FlightMetrics& m, double dtS, bool highGEntered);

  // Closes the mission. Live stats stay readable until the next start().
  MissionReport finish(bool aborted);

  // Clears live statistics and timeline without closing the mission.
  void clearStats();

  // Clears everything derived from `g` (extremes, onset, histogram, high-G statistics,
  // timeline) but keeps duration, max speed, max AoA and sample counts. Used when the
  // G mode changes mid-mission: Nz and |n| values are not comparable. The timeline then
  // restarts at the current mission time (MissionReport::timelineStartS).
  void resetGStats();

  const MissionStats& stats() const { return stats_; }

 private:
  // Bounds memory for absurdly long sessions (~28 h at 10 Hz).
  static constexpr std::size_t kMaxTimelinePoints = 1'000'000;

  MissionStats stats_;
  std::vector<float> timeline_;
  double timelineHz_ = 10.0;
  double timelineStartS_ = 0.0;
  std::uint32_t nextId_ = 1;
  bool open_ = false;
};

}  // namespace ft
