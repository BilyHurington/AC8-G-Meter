// MissionArchiveRecorder: every finished mission is saved from the pipeline itself, with no
// UI frame (no snapshot is ever read), including missions ending back to back and missions
// of a recreated pipeline whose ids restart at 1.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/pipeline.h"
#include "storage/mission_recorder.h"

using namespace ft;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const char* name) {
  const fs::path p = fs::path(FT_TEST_TMP_DIR) / "storage" / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  return p;
}

// Level flight at 200 m/s, 60 Hz, in the given phase. Returns the next sample time.
double fly(TelemetryPipeline& p, double t0, double seconds, MissionPhase phase) {
  double t = t0;
  for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i, t = t0 + (i / 60.0)) {
    RawFlightSample s;
    s.t = t;
    s.position = {200.0 * t, 0.0, 3000.0};
    s.velocity = Vec3{200.0, 0.0, 0.0};
    s.phase = phase;
    p.push(s);
  }
  return t;
}

std::vector<std::uint32_t> savedIds(MissionArchive& archive) {
  archive.refreshAsync();
  archive.flush();
  std::vector<std::uint32_t> ids;
  for (const auto& e : archive.index().entries) ids.push_back(e.id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace

TEST_CASE("the recorder saves every mission without any UI frame") {
  MissionArchive archive(freshDir("recorder"), 0);
  MissionArchiveRecorder recorder(archive);
  {
    TelemetryPipeline pipeline;
    pipeline.addConsumer(&recorder);
    // Two missions back to back, never reading a snapshot in between (as if no frame ran).
    double t = fly(pipeline, 0.0, 3.0, MissionPhase::Active);
    t = fly(pipeline, t, 0.2, MissionPhase::Ended);
    t = fly(pipeline, t, 3.0, MissionPhase::Active);
    t = fly(pipeline, t, 0.2, MissionPhase::Ended);
    // An aborted mission with G data is saved too.
    t = fly(pipeline, t, 3.0, MissionPhase::Active);
    fly(pipeline, t, 0.2, MissionPhase::None);
    REQUIRE(pipeline.snapshot().lastReport);
    CHECK(pipeline.snapshot().lastReport->id == 3);
    CHECK(pipeline.snapshot().lastReport->aborted);
  }
  archive.flush();
  CHECK(savedIds(archive) == std::vector<std::uint32_t>{1, 2, 3});

  // The host recreates the pipeline: ids restart at 1, and the new mission 1 is saved as well.
  {
    TelemetryPipeline pipeline;
    pipeline.addConsumer(&recorder);
    double t = fly(pipeline, 0.0, 3.0, MissionPhase::Active);
    fly(pipeline, t, 0.2, MissionPhase::Ended);
    const auto report = pipeline.snapshot().lastReport;
    REQUIRE(report);
    CHECK(report->id == 1);
    archive.flush();
    // The recorder shares the pipeline's report object: the save status is about this very
    // report (no copy), recognisable by pointer.
    const SaveStatus st = archive.lastSave();
    CHECK(st.state == SaveStatus::State::Saved);
    CHECK(st.report == report);
  }
  CHECK(savedIds(archive) == std::vector<std::uint32_t>{1, 1, 2, 3});

  // Disabled (settings: saving missions off): nothing is written.
  recorder.setEnabled(false);
  {
    TelemetryPipeline pipeline;
    pipeline.addConsumer(&recorder);
    double t = fly(pipeline, 0.0, 3.0, MissionPhase::Active);
    fly(pipeline, t, 0.2, MissionPhase::Ended);
    REQUIRE(pipeline.snapshot().lastReport);
  }
  archive.flush();
  CHECK(savedIds(archive).size() == 4);
}

TEST_CASE("missions without G data are not saved") {
  MissionArchive archive(freshDir("recorder_nog"), 0);
  MissionArchiveRecorder recorder(archive);
  MissionReport r;
  r.id = 1;
  r.stats.hasData = true;
  r.stats.hasGData = false;
  recorder.onMissionEnded(std::make_shared<const MissionReport>(r));
  archive.flush();
  CHECK(archive.lastSave().state == SaveStatus::State::None);
  recorder.onMissionEnded(nullptr);  // tolerated
  r.stats.hasGData = true;
  recorder.onMissionEnded(std::make_shared<const MissionReport>(r));
  archive.flush();
  CHECK(archive.lastSave().state == SaveStatus::State::Saved);
}
