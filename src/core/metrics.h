#pragma once
// Output data model of the core layer: what the UI (and future consumers such as the
// pilot-physiology model) read. Plain value types, cheap to copy across threads.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math.h"
#include "core/sample.h"

namespace ft {

// Which scalar is reported as "G" (HUD value, min/max, onset, high-G timer).
enum class GMode : std::uint8_t {
  Nz,     // load factor along the body up axis (head-to-seat G felt by the pilot). Signed.
  Total,  // magnitude of the load-factor vector. Always >= 0.
};

// Instantaneous derived metrics for the latest accepted sample.
struct FlightMetrics {
  double t = 0.0;
  bool valid = false;          // false until the estimators have enough history
  double g = 1.0;              // filtered G according to GMode (the headline number)
  double gRaw = 1.0;           // same quantity before median/EMA smoothing
  Vec3 loadFactorBody{0.0, 0.0, 1.0};  // filtered (Nx forward, Ny right, Nz up), in G
  double gTotal = 1.0;         // |load factor vector|, in G
  double onsetRate = 0.0;      // d(g)/dt, G/s
  bool aoaValid = false;       // false when airspeed is too low for a meaningful AoA
  double aoaDeg = 0.0;         // angle of attack, positive = nose above velocity vector
  double sideslipDeg = 0.0;    // positive = air coming from the right
  double speed = 0.0;          // m/s (ground speed; AC8 has no wind model we can read)
  double altitude = 0.0;       // m, world Z
  bool inHighG = false;        // inside a sustained high-G episode (with hysteresis)
  double highGDurationS = 0.0; // length of the current high-G episode, 0 when not in one
};

// G histogram buckets: (-inf,-1) [-1,0) [0,1) [1,3) [3,5) [5,7) [7,9) [9,12) [12,15) [15,inf)
inline constexpr std::array<double, 9> kGBinEdges{-1.0, 0.0, 1.0, 3.0, 5.0, 7.0, 9.0, 12.0, 15.0};
inline constexpr int kGBinCount = static_cast<int>(kGBinEdges.size()) + 1;
int gBinIndex(double g);

// Aggregates over one mission. Times ("...AtS") are seconds since mission start.
struct MissionStats {
  bool hasData = false;
  bool hasGData = false;           // G extremes / onset / histogram hold at least one valid value
  double durationS = 0.0;          // flight time while Active (pauses excluded)
  double maxG = 0.0, maxGAtS = 0.0;
  double minG = 0.0, minGAtS = 0.0;
  double maxOnset = 0.0;           // most positive onset rate, G/s
  double minOnset = 0.0;           // most negative onset rate, G/s
  double maxAoaDeg = 0.0;
  double maxSpeed = 0.0;           // m/s
  double longestHighGS = 0.0;      // longest single sustained high-G episode
  double totalHighGS = 0.0;        // cumulative time in high-G episodes
  int highGEpisodes = 0;
  std::array<double, kGBinCount> timeInGBinS{};
  std::uint64_t acceptedSamples = 0;
  std::uint64_t rejectedSamples = 0;  // dropped as spikes or for bad timestamps / values
  std::uint64_t extremeGSamples = 0;  // accepted, |load factor| > PipelineConfig::extremeGThreshold
};

// Immutable record of a finished mission, published once per mission.
struct MissionReport {
  std::uint32_t id = 0;            // 1, 2, 3, ... per pipeline lifetime
  bool aborted = false;            // left Active for None without passing through Ended
  MissionStats stats;
  double timelineHz = 10.0;
  std::vector<float> gTimeline;    // `g` resampled at timelineHz across the mission
  // Mission time of gTimeline[0]. 0 unless the G mode changed mid-mission, which restarts
  // the timeline (and the other G statistics) at that point.
  double timelineStartS = 0.0;
  double extremeGThreshold = 15.0;  // the threshold stats.extremeGSamples was counted with
};

// Fixed-size ring of recent `g` values at a fixed rate (sparklines, future physiology).
struct RecentHistory {
  static constexpr int kCapacity = 200;
  double sampleHz = 20.0;          // kCapacity / sampleHz = 10 s window
  std::array<float, kCapacity> values{};
  int count = 0;                   // number of valid entries (<= kCapacity)
  int head = 0;                    // index where the next value will be written

  void push(float v);
  float at(int i) const;           // i in [0, count): 0 = oldest, count-1 = newest
  void clear() { count = 0; head = 0; }
};

// Counters describing input quality, for debugging and the mission summary.
struct DataQuality {
  std::uint64_t accepted = 0;
  std::uint64_t rejectedNonFinite = 0;   // NaN/Inf or degenerate attitude
  std::uint64_t rejectedTime = 0;        // dt <= 0 (duplicate / out-of-order timestamps)
  std::uint64_t rejectedSpike = 0;       // inconsistent with the recent motion (spike gate)
  // Informational, not a rejection: accepted samples whose |load factor| exceeded
  // PipelineConfig::extremeGThreshold. There is no G cap; these are reported as measured.
  std::uint64_t extremeG = 0;
  std::uint64_t resetsGap = 0;           // dt too large (hitch, pause, load) -> estimators reset
  std::uint64_t resetsTeleport = 0;      // position jump inconsistent with velocity -> reset
};

// Everything the UI needs for one frame.
struct MetricsSnapshot {
  FlightMetrics current;
  MissionPhase phase = MissionPhase::None;
  MissionStats mission;                           // live stats of current / most recent mission
  std::shared_ptr<const MissionReport> lastReport;  // most recent completed mission, or null
  RecentHistory recent;
  DataQuality quality;
};

}  // namespace ft
