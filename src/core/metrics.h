#pragma once
// Output data model of the core layer: what the UI (and future consumers such as the
// pilot-physiology model) read. Plain value types, cheap to copy across threads.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

// One bucket of a MissionTrace (1 / hz of mission time).
struct TracePoint {
  float gMean = 1.0f;          // time-weighted mean of `g`
  float gMin = 1.0f, gMax = 1.0f;  // extremes: a one-frame peak survives the downsampling
  float onset = 0.0f;          // onset rate with the largest magnitude in the bucket, G/s
  float aoaDeg = 0.0f;         // largest valid AoA, NaN when no sample had a valid AoA
  float speed = 0.0f;          // m/s, last sample of the bucket
  float altitude = 0.0f;       // m, last sample of the bucket
  bool highG = false;          // some sample was inside a sustained high-G episode
};

// Fixed-rate record of a mission's valid metrics for the post-mission chart and the archive.
// Bucket i covers mission time [startS + i / hz, startS + (i + 1) / hz). Struct of arrays:
// every column has size() entries.
struct MissionTrace {
  double hz = 20.0;
  // Mission time of bucket 0. 0 unless the G mode changed mid-mission, which restarts the
  // trace (and the other G statistics) at that point.
  double startS = 0.0;
  std::vector<float> gMean, gMin, gMax, onset, aoaDeg, speed, altitude;
  std::vector<std::uint8_t> highG;  // 0 / 1

  std::size_t size() const { return gMean.size(); }
  bool empty() const { return gMean.empty(); }
  double timeAt(std::size_t i) const { return startS + static_cast<double>(i) / hz; }
  double endS() const { return timeAt(size()); }
  void push(const TracePoint& p);
  TracePoint at(std::size_t i) const;
  void clear();
};

// Immutable record of a finished mission, published once per mission.
struct MissionReport {
  std::uint32_t id = 0;            // 1, 2, 3, ... per pipeline lifetime
  bool aborted = false;            // left Active for None without passing through Ended
  MissionStats stats;
  MissionTrace trace;
  // Configuration in use when the mission ended (what `g` and the high-G figures mean).
  GMode gMode = GMode::Nz;
  double highGEnter = 6.0;
  double highGExit = 5.5;
  double extremeGThreshold = 15.0;  // the threshold stats.extremeGSamples was counted with
  std::string label;                // host-provided name (map, scenario, replay file); may be empty
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
  // Inconsistent with the recent motion (spike gate), including held samples the gate's
  // look-back removed after the fact (moved here from `accepted`). Rejected samples a velocity
  // jump then restarts from are moved to `accepted`. accepted + rejected* = samples pushed
  // outside pauses.
  std::uint64_t rejectedSpike = 0;
  // Informational, not a rejection: accepted samples whose |load factor| exceeded
  // PipelineConfig::extremeGThreshold. There is no G cap; these are reported as measured.
  std::uint64_t extremeG = 0;
  std::uint64_t resetsGap = 0;           // dt too large (hitch, pause, load) -> estimators reset
  std::uint64_t resetsTeleport = 0;      // position jump inconsistent with velocity -> reset
  // Velocity jump the position still agrees with (a step of more than spikeDeviationG in one
  // frame): only the acceleration window restarts; no onReset, the high-G episode goes on.
  std::uint64_t resetsJump = 0;
};

// The filter windows in use right now (see PipelineConfig::adaptiveFiltering).
struct FilterState {
  enum class Input : std::uint8_t { None, Velocity, PositionOnly };
  Input input = Input::None;     // kind of the last accepted sample (None before any)
  double sampleRateHz = 0.0;     // measured input rate (velocity input), 0 until known
  double velocityNoiseMS = 0.0;  // measured per-axis velocity noise sigma, m/s
  bool noiseReady = false;       // rate and noise measured; before that they are provisional
  double accelWindowS = 0.0;     // least-squares acceleration window
  double smoothingTauS = 0.0;    // EMA time constant on G
  double onsetWindowS = 0.0;     // least-squares window for the onset rate
  bool adaptive = false;         // windows follow rate and noise (else the fixed config values)
};

// Everything the UI needs for one frame.
struct MetricsSnapshot {
  FlightMetrics current;
  MissionPhase phase = MissionPhase::None;
  MissionStats mission;                           // live stats of current / most recent mission
  std::shared_ptr<const MissionReport> lastReport;  // most recent completed mission, or null
  RecentHistory recent;
  DataQuality quality;
  FilterState filter;
};

}  // namespace ft
