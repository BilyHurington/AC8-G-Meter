#pragma once
// Prototype-only stand-in for the game: sky/ground with a moving horizon, a boresight and
// flight-path marker, and a mock of the vanilla AC8 HUD block in the top-left corner, so the
// telemetry overlay can be judged in context. Drawn on ImGui's background draw list.
#include "core/math.h"
#include "core/sample.h"

struct ImDrawList;
struct ImFont;

namespace ft {

struct SceneState {
  double pitchDeg = 0.0;     // nose above the horizon
  double rollDeg = 0.0;      // positive = right wing down
  double aoaDeg = 0.0;       // flight-path marker sits this far below the boresight
  double sideslipDeg = 0.0;  // ... and this far sideways
  double speedKmh = 0.0;
  double altitudeM = 0.0;
  double missionTimeS = 0.0;
  MissionPhase phase = MissionPhase::Active;
};

// Pitch / roll from body axes: pitch = asin(forward.z), roll = atan2(-right.z, up.z).
void attitudeAngles(const Basis& attitude, double& pitchDeg, double& rollDeg);

class GameScene {
 public:
  void setFont(ImFont* font) { font_ = font; }
  void draw(ImDrawList* dl, float w, float h, const SceneState& state, double now);
  // A mouse click the game (not the overlay) received; flashes a marker there.
  void registerClick(float x, float y, double now);

 private:
  void drawWorld(ImDrawList* dl, float w, float h, const SceneState& s);
  void drawGameHud(ImDrawList* dl, float w, float h, const SceneState& s);
  void drawClickMarker(ImDrawList* dl, float h, double now);

  ImFont* font_ = nullptr;
  float clickX_ = 0.0f, clickY_ = 0.0f;
  double clickAt_ = -1.0e9;
};

}  // namespace ft
