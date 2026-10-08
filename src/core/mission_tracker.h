#pragma once
// Accumulates MissionStats and the fixed-rate MissionTrace for one mission at a time and
// turns them into an immutable MissionReport. Knows nothing about phases: the pipeline
// decides when a mission starts, which samples count, and when it ends.
//
// Two inputs (see MissionStats for the "dual values"):
//   add()          live valid metrics at the current mission time: peak extremes, onset,
//                  histogram, AoA, speed and the live trace columns;
//   addSustained() the sustained G (MissionFeed), at the mission time of the sample it came
//                  from (~2 W ago): sustained extremes, high-G episodes, and the trace's
//                  gSustained / highG columns.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/metrics.h"

namespace ft {

// One output of the sustained-G path (MissionFeed).
struct SustainedSample {
  double g = 1.0;
  double atS = 0.0;   // mission time of the sample it stands for
  double dtS = 0.0;   // mission time since the previous one (weights the high-G totals)
  // Within W of a restart of the filter (gap, pause, G mode change, reset), where an
  // excursion in progress cannot be told from a sustained one: no new extremes, no new
  // episode, not traced (the next confirmed value fills the trace back).
  bool confirmed = true;
  bool inHighG = false;        // inside a high-G episode of the sustained G
  bool highGEntered = false;   // the episode started here
  double highGDurationS = 0.0; // length of the current episode, 0 when not in one
};

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

  // Live valid metrics at the current mission time (call addTime first). dtS weights the
  // histogram. In the trace, the sample stands for the mission time since the previous valid
  // sample (so warm-up after a reset is filled with the first value after it, never left empty).
  void add(const FlightMetrics& m, double dtS);
  // One sustained-G value (atS <= the current mission time, not decreasing).
  void addSustained(const SustainedSample& s);

  // Closes the mission. Live stats stay readable until the next start().
  MissionReport finish(bool aborted);

  // Clears live statistics and trace without closing the mission.
  void clearStats();

  // Clears everything derived from `g` (extremes, onset, histogram, sustained and high-G
  // statistics, trace) but keeps duration, max speed, max AoA and sample counts. Used when the
  // G mode changes mid-mission: Nz and |n| values are not comparable. The trace then restarts
  // at the current mission time (MissionTrace::startS).
  void resetGStats();

  const MissionStats& stats() const { return stats_; }

  // Bounds memory for absurdly long sessions: ~14 h at 20 Hz, 33 bytes per bucket.
  static constexpr std::size_t kMaxTracePoints = 1'000'000;

  // Splits the time a sample stands for over fixed-rate buckets. Each sample covers the
  // mission time since the previous one, (tracedTo, now]; the part in each bucket is merged
  // with its share as weight, and a bucket is closed when a sample reaches its end. A sample
  // with no duration still counts for the open bucket (weight 0).
  class Bucketer {
   public:
    void restart(double startS);
    // merge(weightS) adds the sample to the open bucket; close() completes it.
    template <typename Merge, typename Close>
    void add(double now, const MissionTrace& grid, Merge&& merge, Close&& close);
    // Completes a partial last bucket.
    template <typename Close>
    void finish(Close&& close) {
      if (any_ && index_ < kMaxTracePoints) closeOpen(close);
    }
    std::size_t index() const { return index_; }  // index of the open bucket

   private:
    template <typename Close>
    void closeOpen(Close&& close) {
      close();
      any_ = false;
      ++index_;
    }
    std::size_t index_ = 0;
    double tracedToS_ = 0.0;
    bool any_ = false;  // something was merged into the open bucket
  };

 private:
  // The live bucket being filled (index trace_.size()).
  struct Bucket {
    double weightS = 0.0, gSum = 0.0;
    TracePoint p;
  };
  // The sustained bucket being filled (index sustainedG_.size()).
  struct SustainedBucket {
    double weightS = 0.0, gSum = 0.0, lastG = 1.0;
    bool highG = false;
  };

  void restartTrace(double startS);
  void mergeIntoBucket(const FlightMetrics& m, double weightS, bool first);
  void closeBucket();
  void mergeSustained(const SustainedSample& s, double weightS);
  void closeSustained();

  MissionStats stats_;
  MissionTrace trace_;
  Bucketer live_, sustained_;
  Bucket bucket_;
  SustainedBucket sBucket_;
  // Sustained columns, filled behind the live ones; merged into the trace at finish().
  std::vector<float> sustainedG_;
  std::vector<std::uint8_t> sustainedHighG_;
  std::uint32_t nextId_ = 1;
  bool open_ = false;
};

}  // namespace ft
