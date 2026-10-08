#pragma once
// Telemetry source backed by PointMassFlightModel. Emits samples at a simulated game frame
// rate (with jitter), optionally corrupted with the kinds of glitches a real game hook sees.
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

#include "core/source.h"
#include "sources/flight_model.h"

namespace ft {

enum class Scenario : std::uint8_t {
  LevelCruise,     // straight and level, ~1 G
  SustainedTurn,   // roll into a ~7 G sustained turn, hold, roll out; repeats
  PullUpPushOver,  // pull to +5 G, unload, push to -2 G; repeats
  DogfightMix,     // seeded random sequence of rolls, pulls and unloads up to ~9 G
  Manual,          // driven by setManualInput() (keyboard flying in the prototype)
  Count
};
const char* scenarioName(Scenario s);

struct DirtyDataOptions {
  double positionNoiseM = 0.0;      // gaussian sigma on reported position
  double velocityNoiseMS = 0.0;     // gaussian sigma on reported velocity
  double frameJitter = 0.15;        // +- fraction of the nominal frame time
  double dropProbability = 0.0;     // chance a frame produces no sample
  double duplicateProbability = 0.0;// chance a sample is emitted twice (same timestamp)
};

struct SimulatedSourceOptions {
  Scenario scenario = Scenario::DogfightMix;
  double frameRateHz = 60.0;        // simulated game frame rate (sample rate)
  double physicsHz = 240.0;         // internal integration rate
  bool provideVelocity = true;      // false -> estimator must differentiate position
  DirtyDataOptions dirty;
  std::uint32_t seed = 1234;
};

class SimulatedSource final : public ITelemetrySource {
 public:
  explicit SimulatedSource(const SimulatedSourceOptions& options = {});

  void poll(double realDtS, std::vector<RawFlightSample>& out) override;
  std::string_view name() const override { return "Simulator"; }

  // Deterministic offline generation (tests, data files): advance by exactly durationS of
  // game time, ignoring timeScale and pause. If `truthOut` is given, the exact state behind
  // every emitted sample is appended to it (same count and order as the samples).
  void generate(double durationS, std::vector<RawFlightSample>& out,
                std::vector<FlightState>* truthOut = nullptr);

  // Restart from the initial state with the current options (re-seeds RNG).
  void restart();
  void setOptions(const SimulatedSourceOptions& options);  // restarts if scenario/seed changed
  const SimulatedSourceOptions& options() const { return options_; }

  void setTimeScale(double scale) { timeScale_ = scale; }   // 0.1 .. 8
  double timeScale() const { return timeScale_; }
  // While paused, poll() emits one sample per call at the frozen game time with phase
  // Paused (like a game that keeps rendering its pause menu), so consumers see the pause.
  void setPaused(bool paused);
  bool paused() const { return paused_; }
  void setManualInput(const ControlInput& input) { manualInput_ = input; }

  // Mission control (what the game's flow would report).
  void startMission();   // phase -> Active
  void endMission();     // phase -> Ended
  void quitToMenu();     // phase -> None (aborts an Active mission)
  MissionPhase missionPhase() const { return phase_; }

  // One-shot glitch injection, applied to the next emitted sample(s).
  void injectSpike();             // one sample with a wildly wrong velocity/position
  void injectHitch(double seconds);  // no samples for `seconds` of game time, then resume
  void injectTeleport();          // aircraft jumps several km (respawn / checkpoint)

  const FlightState& truth() const { return model_.state(); }  // latest exact state
  const ControlInput& lastInput() const { return lastInput_; }

 private:
  // One piece of a scripted manoeuvre sequence.
  struct ScriptSegment {
    enum class NzMode : std::uint8_t {
      AltitudeHold,    // nz from the altitude/flight-path controller, bank -> bankDeg
      Fixed,           // nz = nz, bank -> bankDeg
      FixedLevelBank,  // nz = nz, bank chosen to hold altitude (side from sign of bankDeg)
    };
    double durationS = 1.0;
    NzMode mode = NzMode::AltitudeHold;
    double nz = 1.0;
    double bankDeg = 0.0;           // positive = right wing down
    double maxRollRateDegS = 90.0;
    double nzMin = -2.0, nzMax = 7.0;  // clamp for AltitudeHold
    bool recovery = false;          // a level-off segment (never interrupted by the guard)
  };

  ControlInput scriptedInput(double t);
  ScriptSegment nextSegment();
  ControlInput controlFor(const ScriptSegment& seg) const;
  void stepFrame(double frameDt, std::vector<RawFlightSample>& out,
                 std::vector<FlightState>* truthOut);
  void advancePhysics(double dt);
  RawFlightSample makeSample(bool paused);
  double drawFrameDt();
  double uniform(std::mt19937& rng, double lo, double hi);
  double gaussian(double sigma);

  SimulatedSourceOptions options_;
  PointMassFlightModel model_;
  std::mt19937 rng_;             // frame timing + dirty data
  std::mt19937 scriptRng_;       // manoeuvre choice (independent of dirty-data settings)
  ControlInput manualInput_;
  ControlInput lastInput_;
  MissionPhase phase_ = MissionPhase::Active;
  double timeScale_ = 1.0;
  bool paused_ = false;
  double accumulator_ = 0.0;     // scaled real time not yet simulated, s
  double nextFrameDt_ = 0.0;     // jittered length of the next game frame
  double hitchRemaining_ = 0.0;
  bool pendingSpike_ = false;
  bool pendingTeleport_ = false;
  // Scenario script state.
  double scriptPhaseStart_ = 0.0;
  int scriptStep_ = 0;
  ScriptSegment segment_;
};

}  // namespace ft
