// MissionArchive: save / list / load round trip, atomicity, corrupt files, retention,
// name collisions, concurrency, shutdown, non-ASCII paths.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "storage/mission_archive.h"

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
  MissionStats& s = r->stats;
  s.hasData = s.hasGData = true;
  s.durationS = 25.123456789012345;
  s.maxG = 8.765432101234567;
  s.maxGAtS = 12.0 / 7.0;
  s.minG = -1.0 / 3.0;
  s.minGAtS = 3.14159;
  s.maxOnset = 11.1;
  s.minOnset = -9.87654321;
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
    p.onset = 3.0f * std::cos(x * 0.05f);
    p.aoaDeg = i % 7 == 0 ? std::numeric_limits<float>::quiet_NaN() : 2.0f + 0.01f * x;
    p.speed = 250.0f + 0.1f * x;
    p.altitude = 3000.0f + 1.5f * x;
    p.highG = p.gMean > 4.0f;
    r->trace.push(p);
  }
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

void checkSameTrace(const MissionTrace& a, const MissionTrace& b) {
  CHECK(a.hz == b.hz);
  CHECK(a.startS == b.startS);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    const TracePoint p = a.at(i), q = b.at(i);
    CHECK(std::abs(p.gMean - q.gMean) <= 1e-3f);
    CHECK(std::abs(p.gMin - q.gMin) <= 1e-3f);
    CHECK(std::abs(p.gMax - q.gMax) <= 1e-3f);
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
  CHECK_FALSE(r.aborted);
  checkSameStats(r.stats, report->stats);
  checkSameTrace(r.trace, report->trace);

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
  CHECK(csv.find("t,g_mean,g_min,g_max,onset,aoa,speed,alt,high_g\n0.750,") != std::string::npos);
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
  json = missionToJson(*report, 0, "ok.trace.csv");
  REQUIRE(missionFromJson(json, r, e, traceName, rows, &error));
  CHECK(traceName == "ok.trace.csv");
  CHECK(rows == 3);
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
