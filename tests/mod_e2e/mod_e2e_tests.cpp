// End-to-end test of ac8gmeter.dll without the game: a fake UE4SS.dll provides the Lua
// wrapper exports, and the test calls the mod's lua_CFunctions the way main.lua does, with a
// simulated sortie. Checks the bridge (arguments in, results out), the frame protocol, the
// runtime's mission detection, mission archive and CSV recording.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/path_utf8.h"
#include "fake_lua.h"
#include "flight/lua_frame.h"
#include "sources/simulated_source.h"

namespace fs = std::filesystem;
using ft::mod::LuaField;

namespace {

using LuaFunction = int (*)(lua_State*);

std::vector<double> luaFrame(const ft::RawFlightSample* s, double worldTimeS) {
  std::vector<double> v(static_cast<std::size_t>(ft::mod::kLuaFieldCount), 0.0);
  auto set = [&v](LuaField f, double x) { v[static_cast<std::size_t>(f)] = x; };
  auto setVec = [&set](LuaField first, const ft::Vec3& x) {
    set(first, x.x);
    set(static_cast<LuaField>(static_cast<int>(first) + 1), x.y);
    set(static_cast<LuaField>(static_cast<int>(first) + 2), x.z);
  };
  set(LuaField::Version, ft::mod::kLuaFrameVersion);
  set(LuaField::WorldTimeS, worldTimeS);
  set(LuaField::DeltaS, 1.0 / 60.0);
  if (!s) return v;
  set(LuaField::HasPlane, 1.0);
  set(LuaField::PawnId, static_cast<double>(0x7ff612340000ull));  // a typical object address
  setVec(LuaField::LocX, s->position * 100.0);
  setVec(LuaField::VelX, *s->velocity * 100.0);
  setVec(LuaField::FwdX, s->attitude.forward);
  setVec(LuaField::RightX, s->attitude.right);
  setVec(LuaField::UpX, s->attitude.up);
  return v;
}

// Calls `fn` like Lua does: arguments on the stack; its results are the top `results` values
// (anything below them, e.g. unread arguments, is discarded by Lua).
std::vector<double> call(LuaFunction fn, std::vector<double> args) {
  lua_State state{std::move(args)};
  const int results = fn(&state);
  if (results < 0 || results > static_cast<int>(state.stack.size())) return {-1.0};  // never valid
  return {state.stack.end() - results, state.stack.end()};
}

bool waitFor(const auto& condition) {
  for (int i = 0; i < 100; ++i) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

std::vector<fs::path> filesIn(const fs::path& dir, const std::string& extension) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == extension) out.push_back(e.path());
  return out;
}

std::string readAll(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

TEST_CASE("mod DLL: a simulated sortie through the Lua bridge") {
  // Mod folder layout as installed: <mod>/Scripts/ac8gmeter.dll.
  const fs::path modDir = ft::pathFromUtf8(FT_TEST_TMP_DIR) / "mod_e2e" / "AC8GMeter";
  std::error_code ec;
  fs::remove_all(modDir, ec);
  fs::create_directories(modDir / "Scripts");
  fs::copy_file(ft::pathFromUtf8(FT_MOD_DLL), modDir / "Scripts" / "ac8gmeter.dll");

  REQUIRE(LoadLibraryW(ft::pathFromUtf8(FT_FAKE_UE4SS).c_str()));
  _putenv_s("EOS_USE_ANTICHEATCLIENTNULL", "1");
  const HMODULE mod = LoadLibraryW((modDir / "Scripts" / "ac8gmeter.dll").c_str());
  REQUIRE(mod);
  const auto start = reinterpret_cast<LuaFunction>(GetProcAddress(mod, "ac8gmeter_start"));
  const auto frame = reinterpret_cast<LuaFunction>(GetProcAddress(mod, "ac8gmeter_frame"));
  REQUIRE(start);
  REQUIRE(frame);

  CHECK(call(frame, luaFrame(nullptr, 0.0)).empty());  // before start: ignored
  // UE4SS runs main.lua (and so start) on its mod thread; frames come from the game thread.
  std::vector<double> started;
  std::thread([&] { started = call(start, {1.0, 1.0, 1.0}); }).join();  // record CSV, debug recording
  CHECK(started == std::vector<double>{1.0});
  CHECK(call(start, {99.0, 0.0}) == std::vector<double>{1.0});  // starts once; later calls report it
  CHECK(call(frame, {1.0, 2.0}) == std::vector<double>{0.0});   // malformed frame: rejected
  std::vector<double> otherThread{-1.0};
  std::thread([&] { otherThread = call(frame, luaFrame(nullptr, 0.0)); }).join();
  CHECK(otherThread.empty());  // frames only from the first frame's thread

  // 20 s of flight, then 4 s without a plane (results / menus) ends the mission. Like main.lua,
  // the debug block follows the base frame once the DLL answered 2.
  bool debugWanted = false;
  auto send = [&](std::vector<double> values) {
    if (debugWanted) {
      values.push_back(42.0);  // real time
      values.resize(values.size() + ft::mod::kLuaDebugFieldCount - 1, std::numeric_limits<double>::quiet_NaN());
    }
    const std::vector<double> result = call(frame, std::move(values));
    REQUIRE(result.size() == 1);
    REQUIRE((result[0] == 1.0 || result[0] == 2.0));
    debugWanted = result[0] == 2.0;
  };
  ft::SimulatedSource sim;
  std::vector<ft::RawFlightSample> samples;
  double t = 0.0;
  for (int i = 0; i < 20 * 60; ++i) {
    samples.clear();
    sim.poll(1.0 / 60.0, samples);
    for (const ft::RawFlightSample& s : samples) {
      t = s.t;
      send(luaFrame(&s, s.t));
    }
  }
  CHECK(debugWanted);
  for (int i = 0; i < 4 * 60; ++i) send(luaFrame(nullptr, t + i / 60.0));

  CHECK(waitFor([&] { return filesIn(modDir / "missions", ".json").size() == 1; }));
  const std::vector<fs::path> recordings = filesIn(modDir / "recordings", ".csv");
  REQUIRE(recordings.size() == 1);
  CHECK(readAll(recordings.front()).size() > 100000);  // ~1150 rows
  CHECK(waitFor([&] {
    const std::string log = readAll(modDir / "ac8gmeter.log");
    return log.find("mission started") != std::string::npos && log.find("mission ended") != std::string::npos &&
           log.find("debug recording to") != std::string::npos;
  }));

  // The debug file: a row per frame, and the plain finite-difference G agrees with the
  // pipeline's unsmoothed G on this smooth simulated flight.
  const std::vector<fs::path> debugFiles = filesIn(modDir / "debug", ".csv");
  REQUIRE(debugFiles.size() == 1);
  std::istringstream csv(readAll(debugFiles.front()));
  std::string line;
  std::vector<std::string> header;
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string cell; std::getline(ss, cell, ',');) out.push_back(cell);
    if (!s.empty() && s.back() == ',') out.emplace_back();
    return out;
  };
  int rows = 0, compared = 0;
  double worstDiff = 0.0;
  while (std::getline(csv, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::vector<std::string> cells = split(line);
    if (header.empty()) {
      header = cells;
      continue;
    }
    REQUIRE(cells.size() == header.size());
    ++rows;
    auto col = [&](const char* name) { return cells[static_cast<std::size_t>(std::find(header.begin(), header.end(), name) - header.begin())]; };
    if (rows > 1) CHECK(col("real_time_s") == "42.000000");  // the block is asked for after the first frame
    if (col("valid") == "1" && !col("fd_nz_g").empty() && rows > 120) {
      worstDiff = std::max(worstDiff, std::abs(std::stod(col("fd_nz_g")) - std::stod(col("g_raw"))));
      ++compared;
    }
  }
  CHECK(rows > 1300);  // every frame, also without a plane
  CHECK(compared > 1000);
  CHECK(worstDiff < 0.5);
}
