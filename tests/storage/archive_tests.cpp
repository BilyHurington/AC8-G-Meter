// MissionArchive: save / list / load round trip, atomicity, corrupt files, retention,
// name collisions, concurrency, shutdown, non-ASCII paths.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/windowed_g.h"
#include "storage/mission_archive.h"
#include "storage/mission_files.h"

using namespace ft;
namespace fs = std::filesystem;
using doctest::Approx;

namespace {

fs::path freshDir(const std::u8string& name) {
  const fs::path p = fs::path(FT_TEST_TMP_DIR) / "storage" / fs::path(name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

std::shared_ptr<MissionReport> makeReport(std::uint32_t id, std::size_t buckets = 500) {
  auto r = std::make_shared<MissionReport>();
  r->id = id;
  r->label = "Simulator: DogfightMix \xC3\x9C \xE7\xA9\xBA";  // non-ASCII survives
  r->gMode = GMode::Total;
  r->highGEnter = 6.5;
  r->highGExit = 5.75;
  r->extremeGThreshold = 15.0;
  r->minExcursionS = 0.25;
  MissionStats& s = r->stats;
  s.hasData = s.hasGData = true;
  s.durationS = 25.123456789012345;
  s.maxG = 8.765432101234567;
  s.maxGAtS = 12.0 / 7.0;
  s.minG = -1.0 / 3.0;
  s.minGAtS = 3.14159;
  s.maxOnset = 11.1;
  s.minOnset = -9.87654321;
  s.hasSustainedData = true;
  s.sustainedMaxG = 8.123456789;
  s.sustainedMaxGAtS = 12.5 / 7.0;
  s.sustainedMinG = -0.25 / 3.0;
  s.sustainedMinGAtS = 3.5;
  s.maxAoaDeg = 17.5;
  s.maxSpeed = 312.25;
  s.longestHighGS = 4.2;
  s.totalHighGS = 6.9;
  s.highGEpisodes = 3;
  for (std::size_t i = 0; i < s.timeInGBinS.size(); ++i) s.timeInGBinS[i] = 0.1 * static_cast<double>(i) + 1e-7;
  s.acceptedSamples = 1507;
  s.rejectedSamples = 3;
  s.extremeGSamples = 1;
  r->trace.hz = 20.0;
  r->trace.startS = 0.75;
  for (std::size_t i = 0; i < buckets; ++i) {
    const float x = static_cast<float>(i);
    TracePoint p;
    p.gMean = 1.0f + 4.0f * std::sin(x * 0.05f);
    p.gMin = p.gMean - 0.3f;
    p.gMax = p.gMean + 0.7f;
    // The sustained path lags: the last buckets have none.
    p.gSustained = i + 8 >= buckets ? std::numeric_limits<float>::quiet_NaN() : p.gMean - 0.1f;
    p.onset = 3.0f * std::cos(x * 0.05f);
    p.aoaDeg = i % 7 == 0 ? std::numeric_limits<float>::quiet_NaN() : 2.0f + 0.01f * x;
    p.speed = 250.0f + 0.1f * x;
    p.altitude = 3000.0f + 1.5f * x;
    p.highG = p.gMean > 4.0f;
    r->trace.push(p);
  }
  r->windowed = computeWindowedAverages(r->trace);
  return r;
}

std::size_t countFiles(const fs::path& dir, const std::string& suffix) {
  std::size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) ++n;
  }
  return n;
}

void writeFile(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

void makeOld(const fs::path& p) {
  fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(1));
}

LoadedMission loadNow(MissionArchive& a, const MissionIndexEntry& e) {
  auto f = a.loadAsync(e);
  REQUIRE(f.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  return f.get();
}

void checkSameStats(const MissionStats& a, const MissionStats& b) {
  CHECK(a.hasData == b.hasData);
  CHECK(a.hasGData == b.hasGData);
  CHECK(a.durationS == b.durationS);
  CHECK(a.maxG == b.maxG);
  CHECK(a.maxGAtS == b.maxGAtS);
  CHECK(a.minG == b.minG);
  CHECK(a.minGAtS == b.minGAtS);
  CHECK(a.maxOnset == b.maxOnset);
  CHECK(a.minOnset == b.minOnset);
  CHECK(a.hasSustainedData == b.hasSustainedData);
  CHECK(a.sustainedMaxG == b.sustainedMaxG);
  CHECK(a.sustainedMaxGAtS == b.sustainedMaxGAtS);
  CHECK(a.sustainedMinG == b.sustainedMinG);
  CHECK(a.sustainedMinGAtS == b.sustainedMinGAtS);
  CHECK(a.maxAoaDeg == b.maxAoaDeg);
  CHECK(a.maxSpeed == b.maxSpeed);
  CHECK(a.longestHighGS == b.longestHighGS);
  CHECK(a.totalHighGS == b.totalHighGS);
  CHECK(a.highGEpisodes == b.highGEpisodes);
  CHECK(a.timeInGBinS == b.timeInGBinS);
  CHECK(a.acceptedSamples == b.acceptedSamples);
  CHECK(a.rejectedSamples == b.rejectedSamples);
  CHECK(a.extremeGSamples == b.extremeGSamples);
}

void checkSameWindows(const std::vector<WindowedG>& a, const std::vector<WindowedG>& b) {
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].windowS == b[i].windowS);
    CHECK(a[i].valid == b[i].valid);
    if (!a[i].valid) continue;
    CHECK(a[i].maxAvgG == Approx(b[i].maxAvgG).epsilon(1e-4));
    CHECK(a[i].maxAvgStartS == Approx(b[i].maxAvgStartS));
    CHECK(a[i].minAvgG == Approx(b[i].minAvgG).epsilon(1e-4));
    CHECK(a[i].minAvgStartS == Approx(b[i].minAvgStartS));
  }
}

void checkSameTrace(const MissionTrace& a, const MissionTrace& b) {
  CHECK(a.hz == b.hz);
  CHECK(a.startS == b.startS);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    const TracePoint p = a.at(i), q = b.at(i);
    CHECK(std::abs(p.gMean - q.gMean) <= 1e-3f);
    CHECK(std::abs(p.gMin - q.gMin) <= 1e-3f);
    CHECK(std::abs(p.gMax - q.gMax) <= 1e-3f);
    CHECK(std::isnan(p.gSustained) == std::isnan(q.gSustained));
    if (!std::isnan(p.gSustained)) CHECK(std::abs(p.gSustained - q.gSustained) <= 1e-3f);
    CHECK(std::abs(p.onset - q.onset) <= 1e-3f);
    CHECK(std::isnan(p.aoaDeg) == std::isnan(q.aoaDeg));
    if (!std::isnan(p.aoaDeg)) CHECK(std::abs(p.aoaDeg - q.aoaDeg) <= 1e-3f);
    CHECK(std::abs(p.speed - q.speed) <= 1e-3f);
    CHECK(std::abs(p.altitude - q.altitude) <= 1e-3f);
    CHECK(p.highG == q.highG);
  }
}

}  // namespace

TEST_CASE("save -> list -> load round trip") {
  const fs::path dir = freshDir(u8"roundtrip");
  MissionArchive archive(dir);
  const auto report = makeReport(3);
  archive.saveAsync(report);
  archive.flush();

  const SaveStatus st = archive.lastSave();
  CHECK(st.state == SaveStatus::State::Saved);
  CHECK(st.report == report);
  CHECK(st.error.empty());
  CHECK(fs::exists(st.path));
  const std::string name = st.path.filename().string();
  CHECK(name.size() == std::string("20261008-140312_m3.json").size());
  CHECK(name.substr(15) == "_m3.json");

  const MissionIndex index = archive.index();
  REQUIRE(index.entries.size() == 1);
  CHECK(index.version >= 1);
  CHECK_FALSE(index.refreshing);
  const MissionIndexEntry& e = index.entries[0];
  CHECK(e.path == st.path);
  CHECK(e.tracePath.filename().string() == name.substr(0, name.size() - 5) + ".trace.csv");
  CHECK(e.label == report->label);
  CHECK(e.id == 3);
  CHECK(e.gMode == GMode::Total);
  CHECK(e.durationS == report->stats.durationS);
  CHECK(e.maxG == report->stats.maxG);
  CHECK(e.minG == report->stats.minG);
  CHECK_FALSE(e.aborted);
  const auto nowS = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
  CHECK(std::abs(e.savedAtUnix - nowS) < 60);

  const LoadedMission loaded = loadNow(archive, e);
  CHECK(loaded.error.empty());
  REQUIRE(loaded.report);
  const MissionReport& r = *loaded.report;
  CHECK(r.id == 3);
  CHECK(r.label == report->label);
  CHECK(r.gMode == GMode::Total);
  CHECK(r.highGEnter == 6.5);
  CHECK(r.highGExit == 5.75);
  CHECK(r.extremeGThreshold == 15.0);
  CHECK(r.minExcursionS == 0.25);
  CHECK_FALSE(r.aborted);
  checkSameStats(r.stats, report->stats);
  checkSameTrace(r.trace, report->trace);
  // Stored as written (500 buckets = 25 s: the 30 s and 60 s windows are not valid).
  REQUIRE(report->windowed.size() == kDefaultAverageWindowsS.size());
  CHECK(report->windowed[2].valid);
  CHECK_FALSE(report->windowed[3].valid);
  checkSameWindows(r.windowed, report->windowed);

  // An empty trace (no valid metrics) round-trips as well.
  auto empty = std::make_shared<MissionReport>();
  empty->id = 4;
  empty->aborted = true;
  archive.saveAsync(empty);
  archive.flush();
  REQUIRE(archive.index().entries.size() == 2);
  const LoadedMission e2 = loadNow(archive, archive.index().entries[0]);
  REQUIRE(e2.report);
  CHECK(e2.report->aborted);
  CHECK(e2.report->trace.empty());
}

TEST_CASE("serialization helpers") {
  CHECK(missionFileStamp(0) == "19700101-000000");
  CHECK(missionFileStamp(1791468192) == "20261008-140312");
  CHECK(isoTimeUtc(1791468192) == "2026-10-08T14:03:12Z");

  const auto report = makeReport(1, 3);
  const std::string csv = traceToCsv(*report);
  CHECK(csv.find("t,g_mean,g_min,g_max,g_sustained,onset,aoa,speed,alt,high_g\n0.7500,") != std::string::npos);
  CHECK(csv.find(",,") != std::string::npos);  // NaN AoA is an empty field
  MissionTrace back;
  REQUIRE(traceFromCsv(csv, back, nullptr));
  CHECK(back.size() == 3);
  std::string error;
  CHECK_FALSE(traceFromCsv("t,g\n1,2\n", back, &error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(traceFromCsv("t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g\n1,2,3\n", back, &error));

  MissionReport r;
  MissionIndexEntry e;
  std::string traceName;
  std::size_t rows = 0;
  CHECK_FALSE(missionFromJson("{\"a\": 1}", r, e, traceName, rows, &error));
  CHECK_FALSE(missionFromJson("{\"format\": \"ac8-flight-telemetry-mission\", \"schema\": 99}", r, e, traceName,
                              rows, &error));
  // A trace name must not point outside the archive directory.
  std::string json = missionToJson(*report, 0, "../evil.csv");
  CHECK_FALSE(missionFromJson(json, r, e, traceName, rows, &error));
  json = missionToJson(*report, 0, "20261008-140312_m3.trace.csv");
  REQUIRE(missionFromJson(json, r, e, traceName, rows, &error));
  CHECK(traceName == "20261008-140312_m3.trace.csv");
  CHECK(rows == 3);
}

TEST_CASE("schema 1 files (before the sustained statistics) still load") {
  // Write a current pair, then turn it into what version 1 wrote: no sustained statistics, no
  // average windows, no min_excursion_s, no g_sustained column.
  const fs::path dir = freshDir(u8"schema1");
  auto report = makeReport(2, 40);
  {
    MissionArchive archive(dir);
    archive.saveAsync(report);
    archive.flush();
  }
  MissionArchive archive(dir);
  archive.refreshAsync();
  archive.flush();
  const MissionIndexEntry e = archive.index().entries.at(0);
  {
    std::ifstream in(e.path, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dropLine = [&](const std::string& key) {
      const std::size_t at = json.find("\"" + key + "\"");
      REQUIRE(at != std::string::npos);
      // From the comma before the key to the end of its value (its own comma stays).
      const std::size_t begin = json.rfind(',', at);
      std::size_t end = json.find('\n', at);
      if (json[end - 1] == ',') --end;
      json.erase(begin, end - begin);
    };
    for (const char* key : {"min_excursion_s", "has_sustained_data", "sustained_max_g", "sustained_max_g_at_s",
                            "sustained_min_g", "sustained_min_g_at_s"})
      dropLine(key);
    // average_g_windows spans several lines: cut from its key to the end of the stats object.
    const std::size_t from = json.rfind(",", json.find("\"average_g_windows\""));
    const std::size_t to = json.find("\n  },", from);
    json.erase(from, to - from);
    const std::size_t schema = json.find("\"schema\": 2");
    REQUIRE(schema != std::string::npos);
    json.replace(schema, 11, "\"schema\": 1");
    writeFile(e.path, json);
  }
  std::string csv = "t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g\n";
  for (std::size_t i = 0; i < report->trace.size(); ++i)
    csv += std::to_string(report->trace.timeAt(i)) + "," + std::to_string(report->trace.gMean[i]) + "," +
           std::to_string(report->trace.gMin[i]) + "," + std::to_string(report->trace.gMax[i]) + ",0,,250,3000,0\n";
  writeFile(e.tracePath, csv);

  archive.refreshAsync();
  archive.flush();
  const LoadedMission old = loadNow(archive, archive.index().entries.at(0));
  REQUIRE(old.report);
  CHECK(old.error.empty());
  const MissionReport& r = *old.report;
  CHECK(r.stats.maxG == report->stats.maxG);  // the peak values load as before
  CHECK_FALSE(r.stats.hasSustainedData);       // absent: the UI shows "--"
  CHECK(r.minExcursionS == MissionReport{}.minExcursionS);
  REQUIRE(r.trace.size() == 40);
  for (float v : r.trace.gSustained) CHECK(std::isnan(v));
  // The N-second averages are computed from the loaded trace.
  checkSameWindows(r.windowed, computeWindowedAverages(r.trace));
  CHECK(r.windowed.at(0).valid);  // 1 s of a 2 s trace

  // A version from the future is refused.
  std::string text = missionToJson(*report, 0, "20261008-140312_m2.trace.csv");
  text.replace(text.find("\"schema\": 2"), 11, "\"schema\": 3");
  MissionReport out;
  MissionIndexEntry entry;
  std::string traceName, error;
  std::size_t rows = 0;
  CHECK_FALSE(missionFromJson(text, out, entry, traceName, rows, &error));
}

TEST_CASE("the trace column t keeps double precision") {
  auto report = makeReport(1, 3);
  report->trace.startS = 9000.05;  // 2.5 h: a float is off by ~0.2 ms here
  const std::string csv = traceToCsv(*report);
  CHECK(csv.find("\n9000.0500,") != std::string::npos);
  CHECK(csv.find("\n9000.1000,") != std::string::npos);
  CHECK(csv.find("\n9000.1500,") != std::string::npos);
}

TEST_CASE("only the archive's own trace file names are accepted (Windows path tricks)") {
  const auto report = makeReport(1, 3);
  MissionReport r;
  MissionIndexEntry e;
  std::string traceName, error;
  std::size_t rows = 0;
  for (const char* good : {"20261008-140312_m3.trace.csv", "20261008-140312_m0.trace.csv",
                           "20261008-140312_m4294967295.trace.csv", "20261008-140312_m10-2.trace.csv"}) {
    CAPTURE(good);
    CHECK(missionFromJson(missionToJson(*report, 0, good), r, e, traceName, rows, &error));
  }
  for (const char* bad : {"", ".", "..", "../evil.csv", "..\\evil.csv", "sub/20261008-140312_m3.trace.csv",
                          "C:20261008-140312_m3.trace.csv", "C:x", "20261008-140312_m3.trace.csv:stream",
                          "20261008-140312_m3.trace.csv:stream:$DATA", "20261008-140312_m3:x.trace.csv",
                          "20261008-140312_m3.trace.csv.", "20261008-140312_m3.trace.csv ", "..trace.csv",
                          "ok.trace.csv", "20261008-140312_m.trace.csv", "20261008-140312_m03.trace.csv",
                          "20261008-140312_m3-1.trace.csv", "20261008-140312_m3-.trace.csv",
                          "20261008-140312_m3-2-3.trace.csv", "20261008-140312_m4294967296.trace.csv",
                          "20261008_140312_m3.trace.csv", "2026100a-140312_m3.trace.csv",
                          "20261008-140312_m3.json", "20261008-140312_m3.TRACE.CSV"}) {
    CAPTURE(bad);
    error.clear();
    CHECK_FALSE(missionFromJson(missionToJson(*report, 0, bad), r, e, traceName, rows, &error));
    CHECK(error == "bad trace file name");
  }
  const auto stem = parseMissionFileName("20261008-140312_m10-3.json", kMissionJsonSuffix);
  REQUIRE(stem);
  CHECK(stem->stamp == "20261008-140312");
  CHECK(stem->id == 10);
  CHECK(stem->collision == 3);
}

TEST_CASE("trace rows: only aoa may be missing, any other non-finite field fails the load") {
  const std::string header = "t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g\n";
  MissionTrace trace;
  std::string error;
  // aoa empty or "nan": no valid AoA in that bucket.
  REQUIRE(traceFromCsv(header + "0,1,1,1,0,,250,3000,0\n0.05,1,1,1,0,nan,250,3000,0\n0.1,1,1,1,0,2.5,250,3000,1\n",
                       trace, &error));
  REQUIRE(trace.size() == 3);
  CHECK(std::isnan(trace.aoaDeg[0]));
  CHECK(std::isnan(trace.aoaDeg[1]));
  CHECK(trace.aoaDeg[2] == 2.5f);
  const char* badRows[] = {
      ",1,1,1,0,2,250,3000,0",     "0,,1,1,0,2,250,3000,0",    "0,1,nan,1,0,2,250,3000,0",
      "0,1,1,inf,0,2,250,3000,0",  "0,1,1,1,-inf,2,250,3000,0", "0,1,1,1,0,inf,250,3000,0",
      "0,1,1,1,0,2,NaN,3000,0",    "0,1,1,1,0,2,250,,0",        "0,1,1,1,0,2,250,3000,",
      "0,1,1,1,0,2,250,3000,nan",  "infinity,1,1,1,0,2,250,3000,0", "0,1e999,1,1,0,2,250,3000,0",
  };
  for (const char* row : badRows) {
    CAPTURE(row);
    error.clear();
    CHECK_FALSE(traceFromCsv(header + "0,1,1,1,0,2,250,3000,0\n" + row + "\n", trace, &error));
    CHECK(error == "malformed trace row 3");
  }

  // Through the archive: the load fails cleanly with that error.
  const fs::path dir = freshDir(u8"nonfinite");
  MissionArchive archive(dir);
  archive.saveAsync(makeReport(1, 5));
  archive.flush();
  const MissionIndexEntry entry = archive.index().entries.at(0);
  writeFile(entry.tracePath, header + "0,1,1,1,0,,250,3000,0\n0.05,1,1,1,0,,inf,3000,0\n0.1,1,1,1,0,,250,3000,0\n"
                                      "0.15,1,1,1,0,,250,3000,0\n0.2,1,1,1,0,,250,3000,0\n");
  const LoadedMission broken = loadNow(archive, entry);
  CHECK_FALSE(broken.report);
  CHECK(broken.error == "malformed trace row 3");
}

TEST_CASE("saves are atomic and crash leftovers are cleaned up") {
  const fs::path dir = freshDir(u8"atomic");
  MissionArchive archive(dir);
  for (std::uint32_t id = 1; id <= 3; ++id) archive.saveAsync(makeReport(id));
  archive.flush();
  CHECK(countFiles(dir, ".json") == 3);
  CHECK(countFiles(dir, ".trace.csv") == 3);
  CHECK(countFiles(dir, ".tmp") == 0);

  // Leftovers of a crash mid-save: temp files, and a trace whose JSON was never written.
  writeFile(dir / "20200101-000000_m1.trace.csv.tmp", "partial");
  writeFile(dir / "20200101-000000_m2.trace.csv", "t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g\n");
  writeFile(dir / "20200101-000001_m3.trace.csv", "fresh: maybe another process is saving right now");
  makeOld(dir / "20200101-000000_m1.trace.csv.tmp");
  makeOld(dir / "20200101-000000_m2.trace.csv");
  // A JSON whose trace is gone is not listed.
  const MissionIndexEntry victim = archive.index().entries.back();
  fs::remove(victim.tracePath);

  archive.refreshAsync();
  archive.flush();
  CHECK(archive.index().entries.size() == 2);
  CHECK(countFiles(dir, ".tmp") == 0);
  CHECK_FALSE(fs::exists(dir / "20200101-000000_m2.trace.csv"));
  CHECK(fs::exists(dir / "20200101-000001_m3.trace.csv"));  // too recent to call it stale

  // Loading an entry whose files vanished reports an error instead of throwing.
  const LoadedMission gone = loadNow(archive, victim);
  CHECK_FALSE(gone.report);
  CHECK_FALSE(gone.error.empty());
}

TEST_CASE("corrupt or foreign JSON files are skipped") {
  const fs::path dir = freshDir(u8"corrupt");
  MissionArchive archive(dir);
  archive.saveAsync(makeReport(1));
  archive.flush();
  const MissionIndexEntry good = archive.index().entries.at(0);

  writeFile(dir / "garbage.json", "this is not json");
  writeFile(dir / "20261008-140312_m9.json", "{\"format\": \"ac8-flight-telemetry-mission\", \"schema\": 1, ");
  writeFile(dir / "settings.json", "{\"version\": 2, \"overlay\": {}}");
  writeFile(dir / "20261008-140313_m8.json", "");
  archive.refreshAsync();
  archive.flush();
  const MissionIndex index = archive.index();
  REQUIRE(index.entries.size() == 1);
  CHECK(index.entries[0].path == good.path);
  CHECK(index.error.empty());

  // A truncated trace is detected on load.
  {
    std::ifstream in(good.tracePath, std::ios::binary);
    std::string csv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    writeFile(good.tracePath, csv.substr(0, csv.size() / 2));
  }
  const LoadedMission broken = loadNow(archive, good);
  CHECK_FALSE(broken.report);
  CHECK_FALSE(broken.error.empty());
}

TEST_CASE("retention deletes the oldest missions beyond the limit") {
  const fs::path dir = freshDir(u8"retention");
  writeFile(dir / "notes.json", "{}");  // not ours: never deleted
  MissionArchive archive(dir, 3);
  for (std::uint32_t id = 1; id <= 5; ++id) archive.saveAsync(makeReport(id, 10));
  archive.flush();
  const MissionIndex index = archive.index();
  REQUIRE(index.entries.size() == 3);
  std::vector<std::uint32_t> ids;
  for (const auto& e : index.entries) ids.push_back(e.id);
  std::sort(ids.begin(), ids.end());
  CHECK(ids == std::vector<std::uint32_t>{3, 4, 5});
  CHECK(countFiles(dir, ".trace.csv") == 3);
  CHECK(fs::exists(dir / "notes.json"));

  archive.setMaxSaved(0);  // unlimited
  for (std::uint32_t id = 6; id <= 8; ++id) archive.saveAsync(makeReport(id, 10));
  archive.flush();
  CHECK(archive.index().entries.size() == 6);

  // Deleting one entry removes both of its files.
  const MissionIndexEntry victim = archive.index().entries.at(1);
  archive.removeAsync(victim);
  archive.flush();
  CHECK(archive.index().entries.size() == 5);
  CHECK_FALSE(fs::exists(victim.path));
  CHECK_FALSE(fs::exists(victim.tracePath));
}

TEST_CASE("retention and listing order missions by time, then id, then collision suffix") {
  const fs::path dir = freshDir(u8"retention_order");
  // Four missions saved within the same second by one process: ids 9, 10 (twice: a
  // collision) and 11. Name order would be _m10-2 < _m10 < _m11 < _m9.
  const std::int64_t savedAt = 1577836800;  // 2020-01-01T00:00:00Z
  for (const char* stem : {"20200101-000000_m9", "20200101-000000_m10", "20200101-000000_m10-2", "20200101-000000_m11"}) {
    const std::string s(stem);
    writeFile(dir / (s + ".trace.csv"), traceToCsv(*makeReport(1, 4)));
    writeFile(dir / (s + ".json"), missionToJson(*makeReport(1, 4), savedAt, s + ".trace.csv"));
  }
  MissionArchive archive(dir, 0);
  archive.refreshAsync();
  archive.flush();
  std::vector<std::string> names;
  for (const auto& e : archive.index().entries) names.push_back(e.path.filename().string());
  CHECK(names == std::vector<std::string>{"20200101-000000_m11.json", "20200101-000000_m10-2.json",
                                          "20200101-000000_m10.json", "20200101-000000_m9.json"});

  // A fifth save (now) with a limit of 3 deletes the two oldest: m9, then m10.
  archive.setMaxSaved(3);
  archive.saveAsync(makeReport(12, 4));
  archive.flush();
  names.clear();
  for (const auto& e : archive.index().entries) names.push_back(e.path.filename().string());
  REQUIRE(names.size() == 3);
  CHECK(names[1] == "20200101-000000_m11.json");
  CHECK(names[2] == "20200101-000000_m10-2.json");
  CHECK_FALSE(fs::exists(dir / "20200101-000000_m9.json"));
  CHECK_FALSE(fs::exists(dir / "20200101-000000_m9.trace.csv"));
  CHECK_FALSE(fs::exists(dir / "20200101-000000_m10.json"));
  CHECK_FALSE(fs::exists(dir / "20200101-000000_m10.trace.csv"));
  CHECK(countFiles(dir, ".trace.csv") == 3);
}

TEST_CASE("the same mission id within one second gets distinct names") {
  const fs::path dir = freshDir(u8"collision");
  MissionArchive archive(dir, 0);
  const auto report = makeReport(7, 20);
  for (int i = 0; i < 3; ++i) archive.saveAsync(report);
  archive.flush();
  const MissionIndex index = archive.index();
  REQUIRE(index.entries.size() == 3);
  CHECK(countFiles(dir, ".json") == 3);
  CHECK(countFiles(dir, ".trace.csv") == 3);
  for (const auto& e : index.entries) {
    const LoadedMission m = loadNow(archive, e);
    REQUIRE(m.report);
    CHECK(m.report->trace.size() == 20);
  }
}

TEST_CASE("concurrent saves from several threads") {
  const fs::path dir = freshDir(u8"concurrent");
  MissionArchive archive(dir, 0);
  std::vector<std::thread> threads;
  for (std::uint32_t t = 0; t < 4; ++t)
    threads.emplace_back([&archive, t] {
      for (std::uint32_t i = 0; i < 5; ++i) {
        archive.saveAsync(makeReport(t * 10 + i, 50));
        archive.refreshAsync();
        (void)archive.index();
        (void)archive.lastSave();
      }
    });
  for (auto& th : threads) th.join();
  archive.flush();
  const MissionIndex index = archive.index();
  CHECK(index.entries.size() == 20);
  CHECK_FALSE(index.refreshing);
  CHECK(countFiles(dir, ".tmp") == 0);
  for (const auto& e : index.entries) CHECK(loadNow(archive, e).report != nullptr);
}

TEST_CASE("the destructor finishes queued saves and joins the worker") {
  const fs::path dir = freshDir(u8"shutdown");
  {
    MissionArchive archive(dir, 0);
    for (std::uint32_t id = 1; id <= 4; ++id) archive.saveAsync(makeReport(id, 2000));
    archive.refreshAsync();
  }
  CHECK(countFiles(dir, ".json") == 4);
  CHECK(countFiles(dir, ".trace.csv") == 4);
  CHECK(countFiles(dir, ".tmp") == 0);
}

TEST_CASE("non-ASCII archive directory") {
  const fs::path dir = freshDir(u8"\xE4\xBB\xBB\xE5\x8A\xA1 Missi\xC3\xB6n");  // "任务 Missiön"
  fs::remove_all(dir);  // the archive creates it on the first save
  MissionArchive archive(dir / u8"m\xC3\xBC");
  archive.refreshAsync();
  archive.flush();
  CHECK(archive.index().entries.empty());  // missing directory: empty, not an error
  CHECK(archive.index().error.empty());
  archive.saveAsync(makeReport(2, 30));
  archive.flush();
  REQUIRE(archive.lastSave().state == SaveStatus::State::Saved);
  REQUIRE(archive.index().entries.size() == 1);
  const LoadedMission m = loadNow(archive, archive.index().entries[0]);
  REQUIRE(m.report);
  checkSameTrace(m.report->trace, makeReport(2, 30)->trace);
}

TEST_CASE("a failed save reports an error instead of throwing") {
  const fs::path dir = freshDir(u8"failure");
  writeFile(dir / "blocker", "a file where the archive directory should be");
  MissionArchive archive(dir / "blocker" / "missions");
  const auto report = makeReport(1, 5);
  archive.saveAsync(report);
  archive.flush();
  const SaveStatus st = archive.lastSave();
  CHECK(st.state == SaveStatus::State::Failed);
  CHECK(st.report == report);
  CHECK_FALSE(st.error.empty());
  CHECK(archive.index().entries.empty());
}
