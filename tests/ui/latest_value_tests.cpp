// LatestValue is header-only core; tested here because the UI -> pipeline settings hand-off
// (readIfNewer) is used by the UI hosts.
#include <doctest/doctest.h>

#include <cstdint>

#include "core/latest_value.h"
#include "core/pipeline.h"

using namespace ft;

TEST_CASE("LatestValue::readIfNewer reports each publish once") {
  LatestValue<int> box;
  std::uint64_t seen = 0;
  int out = -1;
  CHECK_FALSE(box.readIfNewer(seen, out));  // nothing published yet
  CHECK(out == -1);

  box.publish(7);
  CHECK(box.readIfNewer(seen, out));
  CHECK(out == 7);
  out = -1;
  CHECK_FALSE(box.readIfNewer(seen, out));  // already seen
  CHECK(out == -1);

  // Several publishes between reads: the consumer gets only the latest, once.
  box.publish(8);
  box.publish(9);
  CHECK(box.readIfNewer(seen, out));
  CHECK(out == 9);
  CHECK_FALSE(box.readIfNewer(seen, out));

  // Publishing an equal value still counts as new (the version changes, not the value).
  box.publish(9);
  CHECK(box.readIfNewer(seen, out));
  CHECK(box.read() == 9);

  // Independent consumers keep their own version.
  std::uint64_t other = 0;
  CHECK(box.readIfNewer(other, out));
  CHECK_FALSE(box.readIfNewer(seen, out));
}

TEST_CASE("LatestValue carries a PipelineConfig") {
  LatestValue<PipelineConfig> box;
  PipelineConfig c;
  c.gMode = GMode::Total;
  c.smoothingTauS = 0.3;
  box.publish(c);
  std::uint64_t seen = 0;
  PipelineConfig out;
  REQUIRE(box.readIfNewer(seen, out));
  CHECK(out.gMode == GMode::Total);
  CHECK(out.smoothingTauS == doctest::Approx(0.3));
}
