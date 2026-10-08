#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "sources/csv_recorder.h"
#include "sources/replay_source.h"
#include "sources/simulated_source.h"

using namespace ft;

namespace {

// Rows at t = 0, 0.1, ..., 1.0 (11 rows, 1 s long).
std::vector<CsvRow> tenthRows() {
  std::vector<CsvRow> rows;
  for (int i = 0; i <= 10; ++i) {
    CsvRow r;
    r.sample.t = 0.1 * i;
    r.sample.position = {10.0 * i, 0.0, 1000.0};
    r.trueNz = static_cast<double>(i);
    rows.push_back(r);
  }
  return rows;
}

}  // namespace

TEST_CASE("replay: real-time playback, time scale, pause and progress") {
  ReplaySource rs;
  rs.setRows(tenthRows());
  CHECK(rs.durationS() == doctest::Approx(1.0));
  CHECK(rs.lastEmitted() == nullptr);

  std::vector<RawFlightSample> out;
  rs.poll(0.0, out);
  CHECK(out.size() == 1);  // first row immediately
  rs.poll(0.25, out);
  CHECK(out.size() == 3);  // t = 0.1, 0.2
  CHECK(rs.progress() == doctest::Approx(0.25));
  REQUIRE(rs.lastEmitted());
  CHECK(*rs.lastEmitted()->trueNz == doctest::Approx(2.0));

  rs.setPaused(true);
  rs.poll(1.0, out);
  CHECK(out.size() == 3);
  rs.setPaused(false);

  rs.setTimeScale(2.0);
  rs.poll(0.1, out);  // 0.2 s of log time -> t = 0.3, 0.4
  CHECK(out.size() == 5);
  CHECK(!rs.finished());

  rs.poll(10.0, out);
  CHECK(out.size() == 11);
  CHECK(rs.finished());
  CHECK(rs.progress() == doctest::Approx(1.0));
  rs.poll(1.0, out);
  CHECK(out.size() == 11);

  rs.restart();
  CHECK(!rs.finished());
  CHECK(rs.progress() == doctest::Approx(0.0));
}

TEST_CASE("replay: looping keeps emitted time strictly monotonic") {
  ReplaySource rs;
  rs.setRows(tenthRows());
  rs.setLoop(true);
  std::vector<RawFlightSample> out;
  for (int i = 0; i < 350; ++i) rs.poll(1.0 / 100.0, out);  // 3.5 s -> more than 3 loops
  CHECK(!rs.finished());
  CHECK(out.size() > 33);
  for (std::size_t i = 1; i < out.size(); ++i) REQUIRE(out[i].t > out[i - 1].t);
  // Each loop is one log duration plus one frame (1.1 s); positions repeat.
  CHECK(out[11].t == doctest::Approx(1.1));
  CHECK(out[11].position == out[0].position);
}

TEST_CASE("replay: recorder -> file -> replay round trip") {
  SimulatedSourceOptions opt;
  opt.scenario = Scenario::SustainedTurn;
  opt.provideVelocity = false;
  SimulatedSource sim(opt);
  std::vector<RawFlightSample> samples;
  std::vector<FlightState> truth;
  sim.generate(3.0, samples, &truth);

  const std::string path = std::string(FT_TEST_TMP_DIR) + "/roundtrip.csv";
  {
    CsvRecorder rec;
    std::string err;
    REQUIRE_MESSAGE(rec.open(path, &err), err);
    CHECK(rec.isOpen());
    rec.writeComment("round trip test");
    for (std::size_t i = 0; i < samples.size(); ++i)
      rec.write({samples[i], truth[i].trueNz, truth[i].trueAoaDeg});
    CHECK(rec.rowsWritten() == samples.size());
  }  // destructor closes and flushes

  ReplaySource rs;
  std::string err;
  REQUIRE_MESSAGE(rs.load(path, &err), err);
  CHECK(rs.path() == path);
  REQUIRE(rs.rows().size() == samples.size());
  std::vector<RawFlightSample> out;
  rs.setTimeScale(4.0);
  while (!rs.finished()) rs.poll(1.0 / 60.0, out);
  REQUIRE(out.size() == samples.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    CHECK(std::abs(out[i].t - samples[i].t) < 1e-6);
    CHECK(length(out[i].position - samples[i].position) < 1e-5);
    CHECK(!out[i].velocity);
    CHECK(length(out[i].attitude.up - samples[i].attitude.up) < 1e-5);
    CHECK(*rs.rows()[i].trueNz == doctest::Approx(truth[i].trueNz).epsilon(1e-5));
  }
}
