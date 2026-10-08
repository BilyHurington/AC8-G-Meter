#pragma once
// Accumulates MissionStats and the fixed-rate MissionTrace for one mission at a time and
// turns them into an immutable MissionReport. Knows nothing about phases: the pipeline
// decides when a mission starts, which samples count, and when it ends.
#include <cstddef>
#include <cstdint>

#include "core/metrics.h"

namespace ft {

class MissionTracker {
 public:
  // Opens a new mission with empty statistics. traceHz <= 0 records no trace.
  void start(double traceHz);
  bool open() const { return open_; }

  void countAccepted() { ++stats_.acceptedSamples; }
  void countRejected() { ++stats_.rejectedSamples; }
  void countExtremeG() { ++stats_.extremeGSamples; }
  // Undo a count when the spike gate revises its verdict on a sample (SpikeGate look-back, jump).
  void uncountAccepted() { if (stats_.acceptedSamples > 0) --stats_.acceptedSamples; }
  void uncountRejected() { if (stats_.rejectedSamples > 0) --stats_.rejectedSamples; }

  // Advances mission time by the game time since the previous accepted sample. Called for
  // every accepted Active sample, valid metrics or not, so estimator warm-up after a reset
  // never shortens the mission. Pauses are excluded because game time stops while paused.
  void addTime(double dtS);

  // Adds one sample with valid metrics at mission time atS (<= the current mission time, and
  // not decreasing; the pipeline passes samples delayed by its excursion filter). dtS weights
  // the histogram and high-G totals. highGEntered: an episode started here.
  // In the trace, the sample stands for the mission time since the previous valid sample
  // (so warm-up after a reset is filled with the first value after it, never left empty).
  void add(const FlightMetrics& m, double dtS, bool highGEntered, double atS);
  // Same, at the current mission time (call addTime first).
  void add(const FlightMetrics& m, double dtS, bool highGEntered) { add(m, dtS, highGEntered, stats_.durationS); }

  // Closes the mission. Live stats stay readable until the next start().
  MissionReport finish(bool aborted);

  // Clears live statistics and trace without closing the mission.
  void clearStats();

  // Clears everything derived from `g` (extremes, onset, histogram, high-G statistics,
  // trace) but keeps duration, max speed, max AoA and sample counts. Used when the
  // G mode changes mid-mission: Nz and |n| values are not comparable. The trace then
  // restarts at the current mission time (MissionTrace::startS).
  void resetGStats();

  const MissionStats& stats() const { return stats_; }

  // Bounds memory for absurdly long sessions: ~14 h at 20 Hz, 29 bytes per bucket.
  static constexpr std::size_t kMaxTracePoints = 1'000'000;

 private:
  // The bucket being filled (index trace_.size()).
  struct Bucket {
    bool any = false;
    double weightS = 0.0, gSum = 0.0;
    TracePoint p;
  };

  void restartTrace(double startS);
  void traceSample(const FlightMetrics& m, double now);
  void mergeIntoBucket(const FlightMetrics& m, double weightS);
  void closeBucket();

  MissionStats stats_;
  MissionTrace trace_;
  Bucket bucket_;
  double tracedToS_ = 0.0;  // mission time the trace covers so far
  std::uint32_t nextId_ = 1;
  bool open_ = false;
};

}  // namespace ft
