#pragma once
// The single input record of the whole system: one game-state observation.
// Every telemetry source (simulator, CSV replay, future UE4SS adapter) produces these.
#include <cstdint>
#include <optional>

#include "core/math.h"

namespace ft {

// Coarse game-flow state. Sources map whatever the game exposes onto this.
enum class MissionPhase : std::uint8_t {
  None,    // not in a mission (menus, hangar). Live metrics may still be shown.
  Active,  // flying a mission: statistics accumulate.
  Paused,  // pause menu: samples are ignored for statistics and differentiators.
  Ended,   // mission finished (results screen): triggers the mission report.
};

const char* toString(MissionPhase phase);

struct RawFlightSample {
  double t = 0.0;                  // s, monotonic game time (should not advance while paused)
  Vec3 position;                   // m, world space (UE axes, already converted from cm)
  std::optional<Vec3> velocity;    // m/s, world space. Absent -> derived from position.
  Basis attitude;                  // body axes in world space
  MissionPhase phase = MissionPhase::Active;
  std::optional<double> gameReportedG;  // if the game ever exposes its own G value
};

}  // namespace ft
