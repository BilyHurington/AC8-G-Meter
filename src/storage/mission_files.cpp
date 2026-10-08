#include "storage/mission_files.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef FT_APP_VERSION
#define FT_APP_VERSION "unknown"
#endif

namespace ft {
namespace {

using nlohmann::json;

struct UtcTime {
  int year, month, day, hour, minute, second;
};

// Calendar arithmetic from <chrono> only (no gmtime: not thread-safe, platform variants).
UtcTime utc(std::int64_t unixS) {
  using namespace std::chrono;
  const sys_seconds t{seconds{unixS}};
  const sys_days day = floor<days>(t);
  const year_month_day ymd{day};
  const hh_mm_ss<seconds> hms{t - day};
  return {static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
          static_cast<int>(static_cast<unsigned>(ymd.day())), static_cast<int>(hms.hours().count()),
          static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count())};
}

const char* gModeKey(GMode m) { return m == GMode::Total ? "total" : "nz"; }

// --- lenient readers: assign only when the key exists with a compatible type ---------

void readDouble(const json& obj, const char* key, double& out) {
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_number()) out = it->get<double>();
}

void readBool(const json& obj, const char* key, bool& out) {
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_boolean()) out = it->get<bool>();
}

template <typename T>
void readUnsigned(const json& obj, const char* key, T& out) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_integer()) return;
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
  if (it->is_number_unsigned()) {
    const std::uint64_t v = it->get<std::uint64_t>();
    if (v <= kMax) out = static_cast<T>(v);
  } else if (it->get<std::int64_t>() >= 0) {
    const auto v = static_cast<std::uint64_t>(it->get<std::int64_t>());
    if (v <= kMax) out = static_cast<T>(v);
  }
}

void readString(const json& obj, const char* key, std::string& out) {
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_string()) out = it->get<std::string>();
}

const json* section(const json& root, const char* key) {
  const auto it = root.find(key);
  return (it != root.end() && it->is_object()) ? &*it : nullptr;
}

// --- CSV numbers ------------------------------------------------------------------------

// std::to_chars / from_chars are locale independent (a German locale would write "1,5").
void appendNumber(std::string& out, float v, int decimals) {
  if (std::isnan(v)) return;  // empty field
  std::array<char, 48> buf{};
  auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, decimals);
  if (res.ec != std::errc{}) res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  out.append(buf.data(), res.ptr);
}

bool parseFloat(std::string_view field, float& out) {
  if (field.empty()) {
    out = std::numeric_limits<float>::quiet_NaN();
    return true;
  }
  if (field.front() == '+') field.remove_prefix(1);  // from_chars rejects '+'
  const auto res = std::from_chars(field.data(), field.data() + field.size(), out);
  return res.ec == std::errc{} && res.ptr == field.data() + field.size();
}

constexpr std::string_view kCsvHeader = "t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g";

}  // namespace

std::string missionFileStamp(std::int64_t unixS) {
  const UtcTime u = utc(unixS);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", u.year, u.month, u.day, u.hour, u.minute,
                u.second);
  return buf;
}

std::string isoTimeUtc(std::int64_t unixS) {
  const UtcTime u = utc(unixS);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", u.year, u.month, u.day, u.hour, u.minute,
                u.second);
  return buf;
}

std::string missionToJson(const MissionReport& r, std::int64_t savedAtUnix, const std::string& traceFileName) {
  const MissionStats& s = r.stats;
  using ojson = nlohmann::ordered_json;  // keeps the file in this (readable) key order
  ojson bins = ojson::array();
  for (double v : s.timeInGBinS) bins.push_back(v);
  ojson edges = ojson::array();
  for (double v : kGBinEdges) edges.push_back(v);
  // Doubles are written with round-trip precision, so a reload reproduces them exactly.
  const ojson root = {
      {"format", kMissionFileFormat},
      {"schema", kMissionFileSchema},
      {"app_version", FT_APP_VERSION},
      {"saved_at", isoTimeUtc(savedAtUnix)},
      {"saved_at_unix", savedAtUnix},
      {"label", r.label},
      {"mission_id", r.id},
      {"aborted", r.aborted},
      {"g_mode", gModeKey(r.gMode)},
      {"thresholds",
       {{"high_g_enter", r.highGEnter}, {"high_g_exit", r.highGExit}, {"extreme_g", r.extremeGThreshold}}},
      {"stats",
       {{"has_data", s.hasData},
        {"has_g_data", s.hasGData},
        {"duration_s", s.durationS},
        {"max_g", s.maxG},
        {"max_g_at_s", s.maxGAtS},
        {"min_g", s.minG},
        {"min_g_at_s", s.minGAtS},
        {"max_onset", s.maxOnset},
        {"min_onset", s.minOnset},
        {"max_aoa_deg", s.maxAoaDeg},
        {"max_speed_ms", s.maxSpeed},
        {"longest_high_g_s", s.longestHighGS},
        {"total_high_g_s", s.totalHighGS},
        {"high_g_episodes", s.highGEpisodes},
        {"g_bin_edges", edges},
        {"time_in_g_bin_s", bins}}},
      {"data_quality",
       {{"accepted_samples", s.acceptedSamples},
        {"rejected_samples", s.rejectedSamples},
        {"extreme_g_samples", s.extremeGSamples}}},
      {"trace",
       {{"file", traceFileName}, {"hz", r.trace.hz}, {"start_s", r.trace.startS}, {"count", r.trace.size()}}},
  };
  // error_handler_t::replace: an invalid UTF-8 label must not throw.
  return root.dump(2, ' ', false, ojson::error_handler_t::replace) + "\n";
}

std::string traceToCsv(const MissionReport& r) {
  const MissionTrace& tr = r.trace;
  std::string out;
  out.reserve(64 * tr.size() + 512);
  std::array<char, 32> hz{};
  const auto hzEnd = std::to_chars(hz.data(), hz.data() + hz.size(), tr.hz).ptr;  // shortest round-trip form
  out += "# AC8 Flight Telemetry mission trace (schema " + std::to_string(kMissionFileSchema) + "), mission " +
         std::to_string(r.id) + ", G mode " + gModeKey(r.gMode) + "\n";
  out += "# one row per 1/" + std::string(hz.data(), hzEnd) +
         " s of mission time; t = bucket start (s); g_mean = time-weighted mean,\n"
         "# g_min / g_max = extremes in the bucket; onset = largest |G/s|; aoa = max deg (empty: invalid);\n"
         "# speed m/s, alt m (bucket end); high_g = 1 inside a sustained high-G episode\n";
  out += kCsvHeader;
  out += '\n';
  const std::pair<const std::vector<float>*, int> columns[] = {
      {&tr.gMean, 4}, {&tr.gMin, 4}, {&tr.gMax, 4}, {&tr.onset, 3}, {&tr.aoaDeg, 3}, {&tr.speed, 3}, {&tr.altitude, 3}};
  for (std::size_t i = 0; i < tr.size(); ++i) {
    appendNumber(out, static_cast<float>(tr.timeAt(i)), 3);
    for (const auto& [column, decimals] : columns) {
      out += ',';
      appendNumber(out, (*column)[i], decimals);
    }
    out += tr.highG[i] ? ",1\n" : ",0\n";
  }
  return out;
}

bool missionFromJson(std::string_view text, MissionReport& r, MissionIndexEntry& entry,
                     std::string& traceFileName, std::size_t& traceRows, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  const json root = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded() || !root.is_object()) return fail("invalid JSON");
  std::string format;
  readString(root, "format", format);
  if (format != kMissionFileFormat) return fail("not a mission file");
  int schema = 0;
  readUnsigned(root, "schema", schema);
  if (schema < 1 || schema > kMissionFileSchema) return fail("unsupported schema version");
  const json* stats = section(root, "stats");
  const json* trace = section(root, "trace");
  if (!stats || !trace) return fail("missing stats or trace");

  r = MissionReport{};
  readString(root, "label", r.label);
  readUnsigned(root, "mission_id", r.id);
  readBool(root, "aborted", r.aborted);
  std::string mode;
  readString(root, "g_mode", mode);
  r.gMode = mode == "total" ? GMode::Total : GMode::Nz;
  if (const json* th = section(root, "thresholds")) {
    readDouble(*th, "high_g_enter", r.highGEnter);
    readDouble(*th, "high_g_exit", r.highGExit);
    readDouble(*th, "extreme_g", r.extremeGThreshold);
  }

  MissionStats& s = r.stats;
  readBool(*stats, "has_data", s.hasData);
  readBool(*stats, "has_g_data", s.hasGData);
  readDouble(*stats, "duration_s", s.durationS);
  readDouble(*stats, "max_g", s.maxG);
  readDouble(*stats, "max_g_at_s", s.maxGAtS);
  readDouble(*stats, "min_g", s.minG);
  readDouble(*stats, "min_g_at_s", s.minGAtS);
  readDouble(*stats, "max_onset", s.maxOnset);
  readDouble(*stats, "min_onset", s.minOnset);
  readDouble(*stats, "max_aoa_deg", s.maxAoaDeg);
  readDouble(*stats, "max_speed_ms", s.maxSpeed);
  readDouble(*stats, "longest_high_g_s", s.longestHighGS);
  readDouble(*stats, "total_high_g_s", s.totalHighGS);
  if (const auto it = stats->find("high_g_episodes"); it != stats->end() && it->is_number_integer())
    s.highGEpisodes = static_cast<int>(std::clamp<std::int64_t>(it->get<std::int64_t>(), 0, 1'000'000'000));
  if (const auto it = stats->find("time_in_g_bin_s"); it != stats->end() && it->is_array() &&
                                                      it->size() == s.timeInGBinS.size()) {
    for (std::size_t i = 0; i < s.timeInGBinS.size(); ++i)
      if ((*it)[i].is_number()) s.timeInGBinS[i] = (*it)[i].get<double>();
  }
  if (const json* q = section(root, "data_quality")) {
    readUnsigned(*q, "accepted_samples", s.acceptedSamples);
    readUnsigned(*q, "rejected_samples", s.rejectedSamples);
    readUnsigned(*q, "extreme_g_samples", s.extremeGSamples);
  }

  traceFileName.clear();
  readString(*trace, "file", traceFileName);
  // A bare file name only: never let a file reach outside the archive directory.
  if (traceFileName.empty() || traceFileName.find_first_of("/\\") != std::string::npos ||
      traceFileName == "." || traceFileName == "..")
    return fail("bad trace file name");
  readDouble(*trace, "hz", r.trace.hz);
  readDouble(*trace, "start_s", r.trace.startS);
  if (!(r.trace.hz > 0.0) || !std::isfinite(r.trace.hz) || !std::isfinite(r.trace.startS))
    return fail("bad trace rate");
  traceRows = 0;
  readUnsigned(*trace, "count", traceRows);

  entry = MissionIndexEntry{};
  if (const auto it = root.find("saved_at_unix"); it != root.end() && it->is_number_integer())
    entry.savedAtUnix = it->get<std::int64_t>();
  entry.label = r.label;
  entry.id = r.id;
  entry.gMode = r.gMode;
  entry.durationS = s.durationS;
  entry.hasGData = s.hasGData;
  entry.maxG = s.maxG;
  entry.minG = s.minG;
  entry.aborted = r.aborted;
  return true;
}

bool traceFromCsv(std::string_view text, MissionTrace& trace, std::string* error) {
  trace.clear();
  bool header = false;
  std::size_t lineNo = 0;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;
    if (!header) {
      if (line != kCsvHeader) {
        if (error) *error = "unexpected trace header";
        return false;
      }
      header = true;
      continue;
    }
    std::array<float, 9> v{};
    std::size_t n = 0;
    while (n < v.size()) {
      const std::size_t comma = line.find(',');
      if (!parseFloat(line.substr(0, comma), v[n])) break;
      ++n;
      if (comma == std::string_view::npos) {
        line = {};
        break;
      }
      line.remove_prefix(comma + 1);
    }
    if (n != v.size() || !line.empty()) {
      if (error) *error = "malformed trace row " + std::to_string(lineNo);
      return false;
    }
    trace.push({v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8] != 0.0f});
  }
  if (!header) {
    if (error) *error = "trace header missing";
    return false;
  }
  return true;
}

}  // namespace ft
