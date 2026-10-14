#include <doctest/doctest.h>

#include <limits>

#include "ui/format.h"
#include "ui/layout.h"

using namespace ft;

TEST_CASE("layout at 1920x1080 matches the spec") {
  const OverlaySettings s;
  const OverlayLayout l = computeOverlayLayout(s, 1920.0f, 1080.0f);
  CHECK(l.x == doctest::Approx(86.4));
  CHECK(l.y == doctest::Approx(172.8));
  CHECK(l.fontPx == doctest::Approx(20.0));
  CHECK(l.lineHeightPx == doctest::Approx(26.25));
}

TEST_CASE("layout scales with resolution") {
  const OverlaySettings s;
  const OverlayLayout uhd = computeOverlayLayout(s, 3840.0f, 2160.0f);
  CHECK(uhd.x == doctest::Approx(172.8));
  CHECK(uhd.y == doctest::Approx(345.6));
  CHECK(uhd.fontPx == doctest::Approx(40.0));
  CHECK(uhd.lineHeightPx == doctest::Approx(52.5));

  const OverlayLayout hd = computeOverlayLayout(s, 1280.0f, 720.0f);
  CHECK(hd.x == doctest::Approx(57.6));
  CHECK(hd.y == doctest::Approx(115.2));
  CHECK(hd.fontPx == doctest::Approx(20.0 * 720.0 / 1080.0));
  CHECK(hd.lineHeightPx == doctest::Approx(17.5));
  CHECK(resolutionScale(0.0f) == 1.0f);  // degenerate screen does not divide by zero
}

TEST_CASE("storeOverlayPosition normalizes and keeps the box on screen") {
  OverlaySettings s;
  storeOverlayPosition(s, 960.0f, 540.0f, 100.0f, 60.0f, 1920.0f, 1080.0f);
  CHECK(s.posX == doctest::Approx(0.5));
  CHECK(s.posY == doctest::Approx(0.5));

  storeOverlayPosition(s, -50.0f, -10.0f, 100.0f, 60.0f, 1920.0f, 1080.0f);
  CHECK(s.posX == 0.0f);
  CHECK(s.posY == 0.0f);

  storeOverlayPosition(s, 5000.0f, 5000.0f, 100.0f, 60.0f, 1920.0f, 1080.0f);
  CHECK(s.posX * 1920.0f == doctest::Approx(1820.0));
  CHECK(s.posY * 1080.0f == doctest::Approx(1020.0));

  // Round trip through a different resolution keeps the relative position.
  storeOverlayPosition(s, 192.0f, 108.0f, 100.0f, 60.0f, 1920.0f, 1080.0f);
  const OverlayLayout l = computeOverlayLayout(s, 3840.0f, 2160.0f);
  CHECK(l.x == doctest::Approx(384.0));
  CHECK(l.y == doctest::Approx(216.0));
}

TEST_CASE("number formatting rounds and never prints -0.0") {
  CHECK(formatNumber(6.34, 1, true) == "+6.3");
  CHECK(formatNumber(-1.25, 1, true) == "-1.3");  // round half away from zero
  CHECK(formatNumber(-0.04, 1, true) == "+0.0");
  CHECK(formatNumber(6.3, 1, true, 5) == " +6.3");
  CHECK(formatNumber(850.4, 0, false, 4) == " 850");
  CHECK(formatNumber(std::numeric_limits<double>::quiet_NaN(), 1, true, 4) == "  --");
}

TEST_CASE("metric lines are aligned HUD rows") {
  MetricsSnapshot s;
  s.current.valid = true;
  s.current.g = 6.31;
  s.current.onsetRate = 4.08;
  s.current.aoaValid = true;
  s.current.aoaDeg = 12.44;
  s.current.sideslipDeg = -0.8;
  s.current.speed = 850.0 / 3.6;
  s.current.highGDurationS = 3.2;
  s.mission.hasData = true;
  s.mission.hasGData = true;
  s.mission.maxG = 7.9;
  s.mission.minG = -1.2;

  CHECK(formatMetricLine(MetricId::CurrentG, s) == "G      +6.3");
  CHECK(formatMetricLine(MetricId::MaxG, s) == "MAX G  +7.9");
  CHECK(formatMetricLine(MetricId::MinG, s) == "MIN G  -1.2");
  CHECK(formatMetricLine(MetricId::SustainedMaxG, s) == "SUS G    --");  // no sustained value yet
  s.mission.hasSustainedData = true;
  s.mission.sustainedMaxG = 6.84;
  CHECK(formatMetricLine(MetricId::SustainedMaxG, s) == "SUS G  +6.8");
  CHECK(formatMetricLine(MetricId::OnsetRate, s) == "ONSET  +4.1 G/s");
  CHECK(formatMetricLine(MetricId::Aoa, s) == "AOA    12.4\xC2\xB0");
  CHECK(formatMetricLine(MetricId::HighGTimer, s) == "HI-G   3.2 s");
  CHECK(formatMetricLine(MetricId::Sideslip, s) == "BETA   -0.8\xC2\xB0");
  CHECK(formatMetricLine(MetricId::Speed, s) == "SPD    850 km/h");

  s.current.g = 10.24;  // double digits widen to the left, decimal point stays put
  CHECK(formatMetricLine(MetricId::CurrentG, s) == "G     +10.2");
  s.current.g = 23.44;
  CHECK(formatMetricLine(MetricId::CurrentG, s) == "G     +23.4");
  s.mission.maxG = 23.44;
  CHECK(formatMetricLine(MetricId::MaxG, s) == "MAX G +23.4");
  // Three digits no longer fit the column: the line grows by one character (the HUD box
  // sizes itself to the text), nothing is cut off and the label keeps its separator.
  s.current.g = 123.44;
  s.mission.maxG = 123.44;
  CHECK(formatMetricLine(MetricId::CurrentG, s) == "G     +123.4");
  CHECK(formatMetricLine(MetricId::MaxG, s) == "MAX G +123.4");
  s.current.g = -104.96;
  CHECK(formatMetricLine(MetricId::CurrentG, s) == "G     -105.0");
}

TEST_CASE("invalid data shows dashes") {
  MetricsSnapshot s;  // valid = false, mission.hasData = false
  CHECK(formatMetricLine(MetricId::CurrentG, s) == "G        --");
  CHECK(formatMetricLine(MetricId::MaxG, s) == "MAX G    --");
  s.current.valid = true;
  s.current.aoaValid = false;
  CHECK(formatMetricLine(MetricId::Aoa, s) == "AOA      --");

  OverlaySettings o;  // defaults: three rows
  CHECK(formatHudLines(o, s).size() == 3);
  o.visible.fill(false);
  CHECK(formatHudLines(o, s).empty());
}

TEST_CASE("clock and histogram labels") {
  CHECK(formatClock(0.0) == "00:00");
  CHECK(formatClock(65.9) == "01:05");
  CHECK(formatClock(6000.0) == "100:00");
  CHECK(formatClock(-1.0) == "--:--");
  CHECK(formatSpeedKmh(100.0) == "360 km/h");
  CHECK(gBinLabel(0) == "< -1");
  CHECK(gBinLabel(1) == "-1..0");
  CHECK(gBinLabel(4) == "3..5");
  CHECK(gBinLabel(7) == "9..12");
  CHECK(gBinLabel(8) == "12..15");
  CHECK(gBinLabel(kGBinCount - 1) == ">= 15");
  for (int i = 0; i < kGBinCount; ++i) CHECK_FALSE(gBinLabel(i).empty());
}
