// The Lua -> native frame protocol and its mapping onto pipeline samples / mission phases.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "flight/flight_sampler.h"
#include "flight/lua_frame.h"

using namespace ft;
using namespace ft::mod;

namespace {

struct FrameBuilder {
  std::vector<double> v = std::vector<double>(static_cast<std::size_t>(kLuaFieldCount), 0.0);
  FrameBuilder() {
    set(LuaField::Version, kLuaFrameVersion);
    set(LuaField::DeltaS, 1.0 / 60.0);
    set(LuaField::HasPlane, 1.0);
    set(LuaField::PawnId, 1234.0);
    set(LuaField::FwdX, 1.0);
    set(LuaField::RightY, 1.0);
    set(LuaField::UpZ, 1.0);
  }
  FrameBuilder& set(LuaField f, double value) {
    v[static_cast<std::size_t>(f)] = value;
    return *this;
  }
  LuaFrame frame() const {
    std::string error;
    const auto f = parseLuaFrame(v, &error);
    REQUIRE_MESSAGE(f, error);
    return *f;
  }
};

LuaFrame planeAt(double t, double pawn = 1234.0, bool paused = false) {
  return FrameBuilder()
      .set(LuaField::WorldTimeS, t)
      .set(LuaField::PawnId, pawn)
      .set(LuaField::Paused, paused ? 1.0 : 0.0)
      .set(LuaField::LocX, 100.0 * 250.0 * t)  // 250 m/s along X, in cm
      .set(LuaField::LocZ, 300000.0)
      .set(LuaField::VelX, 25000.0)
      .frame();
}

LuaFrame noPlane(bool paused = false) {
  return FrameBuilder().set(LuaField::HasPlane, 0.0).set(LuaField::Paused, paused ? 1.0 : 0.0).frame();
}

// Feeds `frames` 60 Hz frames from `t0`; returns all samples pushed.
std::vector<RawFlightSample> fly(FlightSampler& s, double t0, int frames, double pawn = 1234.0) {
  std::vector<RawFlightSample> out;
  for (int i = 0; i < frames; ++i) s.onFrame(planeAt(t0 + i / 60.0, pawn), out);
  return out;
}

}  // namespace

TEST_CASE("lua frame: validation") {
  std::string error;
  CHECK(parseLuaFrame(FrameBuilder().v, &error));

  std::vector<double> shortFrame = FrameBuilder().v;
  shortFrame.pop_back();
  CHECK_FALSE(parseLuaFrame(shortFrame, &error));
  CHECK(error.find("expected") != std::string::npos);

  CHECK_FALSE(parseLuaFrame(FrameBuilder().set(LuaField::Version, 99).v, &error));
  CHECK_FALSE(parseLuaFrame(FrameBuilder().set(LuaField::LocY, std::numeric_limits<double>::quiet_NaN()).v, &error));
  CHECK_FALSE(parseLuaFrame(FrameBuilder().set(LuaField::Paused, 0.5).v, &error));
  CHECK_FALSE(parseLuaFrame(FrameBuilder().set(LuaField::DeltaS, -0.1).v, &error));
  CHECK_FALSE(parseLuaFrame(FrameBuilder().set(LuaField::UpZ, 0.5).v, &error));

  // Without a plane the aircraft fields are placeholders and not checked.
  const auto menu = parseLuaFrame(FrameBuilder().set(LuaField::HasPlane, 0.0).set(LuaField::UpZ, 0.0).v, &error);
  REQUIRE(menu);
  CHECK_FALSE(menu->hasPlane);
}

TEST_CASE("lua frame: optional debug block") {
  std::vector<double> v = FrameBuilder().v;
  for (int i = 0; i < kLuaDebugFieldCount; ++i) v.push_back(i == 0 ? 12.5 : std::numeric_limits<double>::quiet_NaN());
  std::string error;
  const auto f = parseLuaFrame(v, &error);
  REQUIRE_MESSAGE(f, error);
  REQUIRE(f->debug);
  CHECK((*f->debug)[static_cast<std::size_t>(LuaDebugField::RealTimeS)] == 12.5);
  CHECK(std::isnan((*f->debug)[static_cast<std::size_t>(LuaDebugField::GameSpeedKmh)]));  // NaN = not provided
  CHECK_FALSE(parseLuaFrame(FrameBuilder().v, &error)->debug);
  v.pop_back();
  CHECK_FALSE(parseLuaFrame(v, &error));  // neither size
  CHECK(std::string(luaDebugFieldName(LuaDebugField::GameSpeedKmh)) == "game_speed_kmh");
}
TEST_CASE("flight sampler: warm-up, units and phases") {
  FlightSampler s;
  // 0.5 s warm-up on a new plane: the first 30 frames (0.5 s of 1/60 s steps) push nothing.
  std::vector<RawFlightSample> out = fly(s, 0.0, 30);
  CHECK(out.empty());
  CHECK_FALSE(s.inMission());

  out = fly(s, 0.5, 1);
  REQUIRE(out.size() == 1);
  CHECK(s.inMission());
  const RawFlightSample& first = out.front();
  CHECK((first.phase == MissionPhase::Active));
  CHECK(first.t == doctest::Approx(0.5));
  CHECK(first.position.x == doctest::Approx(125.0));  // cm -> m
  CHECK(first.position.z == doctest::Approx(3000.0));
  REQUIRE(first.velocity);
  CHECK(first.velocity->x == doctest::Approx(250.0));
  CHECK(first.attitude.up.z == doctest::Approx(1.0));

  out.clear();
  s.onFrame(planeAt(0.6, 1234.0, true), out);
  REQUIRE(out.size() == 1);
  CHECK((out.front().phase == MissionPhase::Paused));
}

TEST_CASE("flight sampler: mission ends after the plane is gone for a while") {
  FlightSampler s;
  fly(s, 0.0, 120);
  REQUIRE(s.inMission());
  std::vector<RawFlightSample> out;
  for (int i = 0; i < 179; ++i) s.onFrame(noPlane(), out);  // < 3 s
  CHECK(out.empty());
  CHECK(s.inMission());
  for (int i = 0; i < 600; ++i) s.onFrame(noPlane(true), out);  // paused time does not count
  CHECK(out.empty());
  s.onFrame(noPlane(), out);  // 3 s reached
  REQUIRE(out.size() == 1);
  CHECK((out.front().phase == MissionPhase::Ended));
  CHECK(out.front().t > 119.0 / 60.0 + 2.9);  // moves on, so it is not rejected as out of order
  CHECK_FALSE(s.inMission());

  out.clear();
  for (int i = 0; i < 600; ++i) s.onFrame(noPlane(), out);  // menus: nothing more
  CHECK(out.empty());
}

TEST_CASE("flight sampler: frozen game time adds no samples") {
  FlightSampler s;
  fly(s, 0.0, 60);
  std::vector<RawFlightSample> out;
  for (int i = 0; i < 10; ++i) s.onFrame(planeAt(1.0), out);  // time dilation 0: same time
  CHECK(out.size() == 1);
}

TEST_CASE("flight sampler: a respawn within the grace time continues the mission") {
  FlightSampler s;
  fly(s, 0.0, 120);
  std::vector<RawFlightSample> out;
  for (int i = 0; i < 60; ++i) s.onFrame(noPlane(), out);  // 1 s without a plane
  out = fly(s, 3.0, 60, 5678.0);                            // new pawn: warm-up, then samples
  CHECK(s.inMission());
  REQUIRE(out.size() == 30);
  for (const RawFlightSample& r : out) CHECK((r.phase == MissionPhase::Active));
}
