#pragma once
// On-disk format of an archived mission (pure functions, no file I/O; see MissionArchive).
//
// Each mission is a pair of files sharing a stem "<YYYYMMDD-HHMMSS>_m<id>" (UTC save time,
// "-2", "-3", ... appended on a same-second collision):
//   <stem>.trace.csv  the MissionTrace, one row per bucket:
//                       t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g
//                     after '#' comment lines. Locale-independent numbers; an empty aoa
//                     field means no valid AoA in that bucket.
//   <stem>.json       metadata: format / schema / app version, save time, label, G mode,
//                     thresholds, every MissionStats field, sample counts, and the trace
//                     file name, rate, start time and row count. Written last, so its
//                     presence marks a complete pair.
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/metrics.h"

namespace ft {

inline constexpr int kMissionFileSchema = 1;
inline constexpr const char* kMissionFileFormat = "ac8-flight-telemetry-mission";

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
// Returns false with *error on malformed or foreign files.
bool missionFromJson(std::string_view text, MissionReport& report, MissionIndexEntry& entry,
                     std::string& traceFileName, std::size_t& traceRows, std::string* error);
// Parses the CSV columns into `trace` (hz / startS untouched).
bool traceFromCsv(std::string_view text, MissionTrace& trace, std::string* error);

}  // namespace ft
