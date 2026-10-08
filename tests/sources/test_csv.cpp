#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "sources/csv_format.h"
#include "sources/replay_source.h"

using namespace ft;

namespace {

CsvRow sampleRow() {
  CsvRow r;
  r.sample.t = 12.345678;
  r.sample.position = {-1234.5, 0.000001, 3000.25};
  r.sample.velocity = Vec3{230.125, -4.5, 0.75};
  r.sample.attitude.forward = {0.6, 0.8, 0.0};
  r.sample.attitude.right = {-0.8, 0.6, 0.0};
  r.sample.attitude.up = {0.0, 0.0, 1.0};
  r.sample.phase = MissionPhase::Paused;
  r.trueNz = 6.789;
  r.trueAoaDeg = -2.5;
  return r;
}

void writeFile(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}

const std::string kTmp = FT_TEST_TMP_DIR;

}  // namespace

TEST_CASE("csv: format -> parse round trip") {
  const CsvRow in = sampleRow();
  const std::string line = formatCsvRow(in);
  CHECK(line.find("paused") != std::string::npos);
  std::string err;
  const std::optional<CsvRow> out = parseCsvRow(line, &err);
  REQUIRE_MESSAGE(out, err);
  CHECK(out->sample.t == doctest::Approx(in.sample.t).epsilon(1e-12));
  CHECK(length(out->sample.position - in.sample.position) < 1e-6);
  REQUIRE(out->sample.velocity);
  CHECK(length(*out->sample.velocity - *in.sample.velocity) < 1e-6);
  CHECK(out->sample.attitude.forward == in.sample.attitude.forward);
  CHECK(out->sample.attitude.right == in.sample.attitude.right);
  CHECK(out->sample.attitude.up == in.sample.attitude.up);
  CHECK((out->sample.phase == MissionPhase::Paused));
  CHECK(*out->trueNz == doctest::Approx(6.789));
  CHECK(*out->trueAoaDeg == doctest::Approx(-2.5));
  // Formatting is stable: the parsed row formats to the same text.
  CHECK(formatCsvRow(*out) == line);
}

TEST_CASE("csv: empty optional columns") {
  CsvRow in = sampleRow();
  in.sample.velocity.reset();
  in.trueNz.reset();
  in.trueAoaDeg.reset();
  const std::string line = formatCsvRow(in);
  CHECK(line.find(",,,") != std::string::npos);
  const std::optional<CsvRow> out = parseCsvRow(line);
  REQUIRE(out);
  CHECK(!out->sample.velocity);
  CHECK(!out->trueNz);
  CHECK(!out->trueAoaDeg);
  // Header column count matches the row column count.
  CHECK(std::count(line.begin(), line.end(), ',') ==
        std::count(csvHeader().begin(), csvHeader().end(), ','));
}

TEST_CASE("csv: malformed lines are rejected with a reason") {
  const std::string good = formatCsvRow(sampleRow());
  std::string err;
  CHECK(!parseCsvRow("1,2,3", &err));
  CHECK(err.find("columns") != std::string::npos);

  CHECK(!parseCsvRow(good + ",1", &err));
  CHECK(err.find("columns") != std::string::npos);

  std::string bad = good;
  bad.replace(0, bad.find(','), "12.3x");
  CHECK(!parseCsvRow(bad, &err));
  CHECK(err.find("'t'") != std::string::npos);

  bad = good;
  bad.replace(bad.find("paused"), 6, "flying");
  CHECK(!parseCsvRow(bad, &err));
  CHECK(err.find("phase") != std::string::npos);

  // Partially empty velocity.
  CsvRow r = sampleRow();
  r.sample.velocity.reset();
  bad = formatCsvRow(r);
  bad.replace(bad.find(",,,"), 3, ",1,,");
  CHECK(!parseCsvRow(bad, &err));
  CHECK(err.find("velocity") != std::string::npos);

  // Missing required value.
  bad = good;
  bad.replace(0, bad.find(','), "");
  CHECK(!parseCsvRow(bad, &err));
  CHECK(err.find("missing") != std::string::npos);

  // Tolerated: spaces, CR line ending, explicit '+' sign.
  CHECK(parseCsvRow(" +" + good + " \r"));
}

TEST_CASE("csv: loadCsvLog skips comments/blank lines and validates the header") {
  const std::string path = kTmp + "/csv_load.csv";
  const std::string row = formatCsvRow(sampleRow());
  writeFile(path, "# a comment\n\n" + std::string(csvHeader()) + "\r\n" + row + "\r\n   \n# check: x\n" +
                      row + "\n");
  std::vector<CsvRow> rows;
  std::string err;
  REQUIRE_MESSAGE(loadCsvLog(path, rows, &err), err);
  CHECK(rows.size() == 2);

  writeFile(path, "t,px,py\n" + row + "\n");
  CHECK(!loadCsvLog(path, rows, &err));
  CHECK(err.find("header") != std::string::npos);

  writeFile(path, std::string(csvHeader()) + "\n" + row + "\n1,2\n");
  CHECK(!loadCsvLog(path, rows, &err));
  CHECK(err.find(":3:") != std::string::npos);  // line number of the bad row

  writeFile(path, "# only comments\n");
  CHECK(!loadCsvLog(path, rows, &err));

  CHECK(!loadCsvLog(kTmp + "/does_not_exist.csv", rows, &err));
}
