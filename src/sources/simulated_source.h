#pragma once
// Telemetry source backed by PointMassFlightModel. Emits samples at a simulated game frame
// rate (with jitter), optionally corrupted with the kinds of glitches a real game hook sees.
#include <cstdint>
#include <random>
#include <string_view>

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
  // game time, ignoring timeScale and pause.
  void generate(double durationS, std::vector<RawFlightSample>& out);

  // Restart from the initial state with the current options (re-seeds RNG).
  void restart();
  void setOptions(const SimulatedSourceOptions& options);  // restarts if scenario/seed changed
  const SimulatedSourceOptions& options() const { return options_; }

  void setTimeScale(double scale) { timeScale_ = scale; }   // 0.1 .. 8
  double timeScale() const { return timeScale_; }
  void setPaused(bool paused);                               // reports MissionPhase::Paused
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
  ControlInput scriptedInput(double t);
  void stepFrame(double frameDt, std::vector<RawFlightSample>& out);
  RawFlightSample makeSample();

  SimulatedSourceOptions options_;
  PointMassFlightModel model_;
  std::mt19937 rng_;
  ControlInput manualInput_;
  ControlInput lastInput_;
  MissionPhase phase_ = MissionPhase::Active;
  double timeScale_ = 1.0;
  bool paused_ = false;
  double accumulator_ = 0.0;     // real time not yet simulated
  double nextFrameDt_ = 0.0;     // jittered length of the next game frame
  double hitchRemaining_ = 0.0;
  bool pendingSpike_ = false;
  bool pendingTeleport_ = false;
  // Scenario-specific scratch state (DogfightMix manoeuvre schedule etc.)
  double scriptPhaseStart_ = 0.0;
  int scriptStep_ = 0;
  ControlInput scriptTarget_;
};

}  // namespace ft
