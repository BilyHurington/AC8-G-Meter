#pragma once
// Turns the per-tick Lua frames into pipeline samples and mission phases (game thread).
//
// First version, until the game's own mission-flow signals are identified in an object dump
// (debriefing / results objects): a mission is Active while the player flies a plane, Paused
// while the game is paused, and Ended once no plane has been possessed for
// `missionEndAfterS` of unpaused time (results, debriefing, menus, loading). A respawn within
// that time continues the mission.
#include <optional>
#include <vector>

#include "core/sample.h"
#include "flight/lua_frame.h"

namespace ft::mod {

class FlightSampler {
 public:
  struct Options {
    // A newly possessed plane is ignored this long: its first transforms can be construction
    // defaults before the mission places it (observed by AC8 Mouse-Aim).
    double warmupS = 0.5;
    double missionEndAfterS = 3.0;
  };

  FlightSampler() = default;
  explicit FlightSampler(Options options) : opt_(options) {}

  // Appends the samples to push into the pipeline for this frame: usually one, none during
  // warm-up or outside missions, the closing Ended sample when a mission ends.
  void onFrame(const LuaFrame& frame, std::vector<RawFlightSample>& out);

  bool inMission() const { return inMission_; }

 private:
  Options opt_;
  bool inMission_ = false;
  std::optional<RawFlightSample> last_;  // last Active sample of the current mission
  double pawnId_ = 0.0;                  // 0 = no plane
  double warmupLeftS_ = 0.0;
  double absentS_ = 0.0;  // unpaused time without a plane during a mission
};

}  // namespace ft::mod
