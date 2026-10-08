#pragma once
// On-disk format of an archived mission (pure functions, no file I/O; see MissionArchive).
//
// Each mission is a pair of files sharing a stem "<YYYYMMDD-HHMMSS>_m<id>" (UTC save time,
// "-2", "-3", ... appended on a same-second collision):
//   <stem>.trace.csv  the MissionTrace, one row per bucket:
//                       t,g_mean,g_min,g_max,g_sustained,onset,aoa,speed,alt,high_g
//                     after '#' comment lines. Locale-independent numbers. g_mean / g_min /
//                     g_max are the live G (time-weighted mean, extremes), g_sustained the
//                     time-weighted mean of the sustained G (empty: none in that bucket);
//                     an empty aoa field means no valid AoA in that bucket.
//   <stem>.json       metadata: format / schema / app version, save time, label, G mode,
//                     thresholds and W (min_excursion_s), every MissionStats field (peak and
//                     sustained), the N-second average G extremes, sample counts, and the
//                     trace file name, rate, start time and row count. Written last, so its
//                     presence marks a complete pair.
//
// Schema history (older files load; missing values stay absent):
//   1  no sustained statistics (stats.max_g etc. were the filtered values; they load as peak
//      values with hasSustainedData = false), no g_sustained column, no average_g_windows
//      (recomputed from the trace by the archive), no min_excursion_s.
//   2  current.
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "core/metrics.h"

namespace ft {

inline constexpr int kMissionFileSchema = 2;
inline constexpr const char* kMissionFileFormat = "ac8-flight-telemetry-mission";
inline constexpr std::string_view kMissionJsonSuffix = ".json";
inline constexpr std::string_view kMissionTraceSuffix = ".trace.csv";

// The parts of a file stem the archive writes: "<YYYYMMDD-HHMMSS>_m<id>[-<n>]".
struct MissionFileStem {
  std::string stamp;            // "YYYYMMDD-HHMMSS" (UTC save time)
  std::uint32_t id = 0;         // MissionReport::id
  std::uint32_t collision = 1;  // 1 = no suffix, n >= 2 for the "-<n>" same-second suffix

  // Save order: time stamp, then id (ids grow within a process), then collision suffix.
  // Name order is not save order: "_m10" < "_m9" and "_m10-2" < "_m10".
  auto key() const { return std::tie(stamp, id, collision); }
};

// Parses `name` = "<stem><suffix>" when the stem has exactly the form the archive writes
// (ASCII digits only, no leading zeros, id within 32 bits). Anything else, such as other
// separators, drive letters, ':' (NTFS streams), "..", or extra characters, is rejected.
std::optional<MissionFileStem> parseMissionFileName(std::string_view name, std::string_view suffix);

// What the history list shows; read from the small JSON file only.
struct MissionIndexEntry {
  std::filesystem::path path;       // the .json file
  std::filesystem::path tracePath;  // the .trace.csv file
  std::int64_t savedAtUnix = 0;     // seconds since 1970-01-01 UTC
  std::string label;
  std::uint32_t id = 0;
  GMode gMode = GMode::Nz;
  double durationS = 0.0;
  bool hasGData = false;
  double maxG = 0.0, minG = 0.0;
  bool aborted = false;
};

// "20261008-140312" (UTC), for file names.
std::string missionFileStamp(std::int64_t unixS);
// "2026-10-08T14:03:12Z".
std::string isoTimeUtc(std::int64_t unixS);

std::string missionToJson(const MissionReport& report, std::int64_t savedAtUnix,
                          const std::string& traceFileName);
std::string traceToCsv(const MissionReport& report);

// Parses the metadata into `report` (everything but the trace columns: trace.hz / startS are
// set, the columns are left empty) and `entry` (path / tracePath relative to the JSON's
// directory, resolved by the caller). `traceRows` receives the declared row count.
// Returns false with *error on malformed or foreign files, including a trace file name that is
// not of the form "<stem>.trace.csv" written by the archive.
bool missionFromJson(std::string_view text, MissionReport& report, MissionIndexEntry& entry,
                     std::string& traceFileName, std::size_t& traceRows, std::string* error);
// Parses the CSV columns into `trace` (hz / startS untouched). Every field must be a finite
// number, except aoa and g_sustained, which may be empty or "nan". Schema 1 files (no
// g_sustained column) load with gSustained = NaN.
bool traceFromCsv(std::string_view text, MissionTrace& trace, std::string* error);

}  // namespace ft
