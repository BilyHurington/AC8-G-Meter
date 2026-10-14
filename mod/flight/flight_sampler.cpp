#include "flight/flight_sampler.h"

#include <algorithm>

namespace ft::mod {

namespace {

constexpr double kCmToM = 0.01;
constexpr double kMaxStepS = 0.25;  // a hitch must not count as seconds of warm-up / absence
constexpr double kTimeEpsS = 1e-6;  // summed frame times: 30 x 1/60 s must make 0.5 s

}  // namespace

void FlightSampler::onFrame(const LuaFrame& f, std::vector<RawFlightSample>& out) {
  const double step = f.paused ? 0.0 : std::clamp(f.deltaS, 0.0, kMaxStepS);

  if (!f.hasPlane) {
    pawnId_ = 0.0;
    if (!inMission_) return;
    absentS_ += step;
    if (absentS_ < opt_.missionEndAfterS - kTimeEpsS) return;
    // Close the mission. The sample repeats the last state; its time moves on by the absence
    // so it is not rejected as out of order (the gap resets the estimators anyway).
    RawFlightSample end = *last_;
    end.t += absentS_;
    end.phase = MissionPhase::Ended;
    out.push_back(end);
    inMission_ = false;
    last_.reset();
    return;
  }

  if (f.pawnId != pawnId_) {
    pawnId_ = f.pawnId;
    warmupLeftS_ = opt_.warmupS;
  }
  // Not flying yet: AC8 spawns the plane at the origin, then places it with zero velocity, then
  // sets flight speed in one frame (a huge false G). Such frames are neither samples nor
  // warm-up, so the warm-up starts once the plane moves (on a runway: once it rolls).
  if (length(f.velocityCmS) * kCmToM < opt_.minFlyingSpeedMS) return;
  if (warmupLeftS_ > kTimeEpsS) {
    warmupLeftS_ -= step;
    return;
  }
  // Game time frozen without a pause (e.g. time dilation 0): the state repeats, nothing to add.
  if (!f.paused && last_ && f.worldTimeS == last_->t) return;

  RawFlightSample s;
  s.t = f.worldTimeS;
  s.position = f.locationCm * kCmToM;
  s.velocity = f.velocityCmS * kCmToM;
  s.attitude = f.attitude;
  s.phase = f.paused ? MissionPhase::Paused : MissionPhase::Active;
  out.push_back(s);
  if (!f.paused) {
    inMission_ = true;
    absentS_ = 0.0;
    last_ = s;
  }
}

}  // namespace ft::mod
