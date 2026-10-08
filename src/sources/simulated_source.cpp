#include "sources/simulated_source.h"

#include <algorithm>
#include <cmath>

namespace ft {

namespace {

constexpr double kCruiseAltitudeM = 3000.0;
constexpr double kCruiseSpeedMS = 230.0;
constexpr double kMaxCatchUpS = 0.25;      // max game time simulated per poll (no spiral of death)
constexpr double kFlightPathTauS = 2.0;    // flight-path angle controller time constant
constexpr double kRollGain = 4.0;          // (deg/s) of roll rate per deg of bank error
constexpr double kSpikeVelocityMS = 300.0;
constexpr double kSpikePositionM = 30.0;   // used when the source provides no velocity
constexpr Vec3 kTeleportOffsetM{4000.0, -2500.0, 0.0};

double wrap180(double deg) {
  deg = std::fmod(deg + 180.0, 360.0);
  if (deg < 0.0) deg += 360.0;
  return deg - 180.0;
}

FlightState initialState() {
  FlightState s;
  s.position = {0.0, 0.0, kCruiseAltitudeM};
  s.velocity = {kCruiseSpeedMS, 0.0, 0.0};
  return s;
}

}  // namespace

const char* scenarioName(Scenario s) {
  switch (s) {
    case Scenario::LevelCruise: return "Level cruise";
    case Scenario::SustainedTurn: return "Sustained turn";
    case Scenario::PullUpPushOver: return "Pull-up / push-over";
    case Scenario::DogfightMix: return "Dogfight mix";
    case Scenario::Manual: return "Manual";
    case Scenario::Count: break;
  }
  return "?";
}

SimulatedSource::SimulatedSource(const SimulatedSourceOptions& options) : options_(options) {
  restart();
}

void SimulatedSource::restart() {
  model_.reset(initialState());
  rng_.seed(options_.seed);
  scriptRng_.seed(options_.seed ^ 0x9E3779B9u);
  lastInput_ = ControlInput{};
  phase_ = MissionPhase::Active;
  accumulator_ = 0.0;
  hitchRemaining_ = 0.0;
  pendingSpike_ = false;
  pendingTeleport_ = false;
  scriptStep_ = 0;
  scriptPhaseStart_ = 0.0;
  segment_ = nextSegment();
  nextFrameDt_ = drawFrameDt();
}

void SimulatedSource::setOptions(const SimulatedSourceOptions& options) {
  const bool restartNeeded = options.scenario != options_.scenario || options.seed != options_.seed;
  options_ = options;
  if (restartNeeded) restart();
}

void SimulatedSource::setPaused(bool paused) {
  paused_ = paused;
  accumulator_ = 0.0;  // don't catch up on the time spent in the pause menu
}

void SimulatedSource::startMission() { phase_ = MissionPhase::Active; }
void SimulatedSource::endMission() { phase_ = MissionPhase::Ended; }
void SimulatedSource::quitToMenu() { phase_ = MissionPhase::None; }

void SimulatedSource::injectSpike() { pendingSpike_ = true; }
void SimulatedSource::injectHitch(double seconds) { hitchRemaining_ = std::max(hitchRemaining_, seconds); }
void SimulatedSource::injectTeleport() { pendingTeleport_ = true; }

void SimulatedSource::poll(double realDtS, std::vector<RawFlightSample>& out) {
  if (paused_) {
    if (realDtS > 0.0) out.push_back(makeSample(true));
    return;
  }
  const double scale = std::clamp(timeScale_, 0.0, 8.0);
  accumulator_ = std::min(accumulator_ + std::max(realDtS, 0.0) * scale, kMaxCatchUpS);
  while (accumulator_ >= nextFrameDt_) {
    accumulator_ -= nextFrameDt_;
    stepFrame(nextFrameDt_, out, nullptr);
    nextFrameDt_ = drawFrameDt();
  }
}

void SimulatedSource::generate(double durationS, std::vector<RawFlightSample>& out,
                               std::vector<FlightState>* truthOut) {
  const double end = model_.state().t + durationS;
  while (model_.state().t + nextFrameDt_ <= end + 1e-9) {
    stepFrame(nextFrameDt_, out, truthOut);
    nextFrameDt_ = drawFrameDt();
  }
  // Simulate the remainder as the first part of the next (still pending) frame.
  const double rest = end - model_.state().t;
  if (rest > 0.0) {
    advancePhysics(rest);
    nextFrameDt_ -= rest;
  }
}

void SimulatedSource::advancePhysics(double dt) {
  const double hz = std::max(options_.physicsHz, 1.0);
  const int n = std::max(1, static_cast<int>(std::ceil(dt * hz - 1e-9)));
  const double h = dt / n;
  for (int i = 0; i < n; ++i) {
    lastInput_ = scriptedInput(model_.state().t);
    model_.step(h, lastInput_);
  }
}

void SimulatedSource::stepFrame(double frameDt, std::vector<RawFlightSample>& out,
                                std::vector<FlightState>* truthOut) {
  advancePhysics(frameDt);

  if (pendingTeleport_) {  // the jump is real (truth moves too), not a data error
    model_.translate(kTeleportOffsetM);
    pendingTeleport_ = false;
  }
  if (hitchRemaining_ > 0.0) {
    hitchRemaining_ -= frameDt;
    return;
  }
  const DirtyDataOptions& dirty = options_.dirty;
  if (!pendingSpike_ && dirty.dropProbability > 0.0 && uniform(rng_, 0.0, 1.0) < dirty.dropProbability)
    return;

  RawFlightSample sample = makeSample(false);
  if (pendingSpike_) {
    if (sample.velocity) sample.velocity->x += kSpikeVelocityMS;
    else sample.position.x += kSpikePositionM;
    pendingSpike_ = false;
  }
  const bool duplicate =
      dirty.duplicateProbability > 0.0 && uniform(rng_, 0.0, 1.0) < dirty.duplicateProbability;
  for (int i = 0; i < (duplicate ? 2 : 1); ++i) {
    out.push_back(sample);
    if (truthOut) truthOut->push_back(model_.state());
  }
}

RawFlightSample SimulatedSource::makeSample(bool paused) {
  const FlightState& st = model_.state();
  const DirtyDataOptions& dirty = options_.dirty;
  RawFlightSample s;
  s.t = st.t;
  s.position = st.position;
  if (dirty.positionNoiseM > 0.0)
    s.position += Vec3{gaussian(dirty.positionNoiseM), gaussian(dirty.positionNoiseM),
                       gaussian(dirty.positionNoiseM)};
  if (options_.provideVelocity) {
    Vec3 v = st.velocity;
    if (dirty.velocityNoiseMS > 0.0)
      v += Vec3{gaussian(dirty.velocityNoiseMS), gaussian(dirty.velocityNoiseMS),
                gaussian(dirty.velocityNoiseMS)};
    s.velocity = v;
  }
  s.attitude = st.attitude;
  s.phase = paused ? MissionPhase::Paused : phase_;
  return s;
}

double SimulatedSource::drawFrameDt() {
  const double base = 1.0 / std::max(options_.frameRateHz, 1.0);
  const double j = std::clamp(options_.dirty.frameJitter, 0.0, 0.9);
  return j > 0.0 ? base * (1.0 + uniform(rng_, -j, j)) : base;
}

// Hand-rolled distributions: std::uniform_real_distribution / normal_distribution differ
// between standard libraries, which would make generated logs platform dependent.
double SimulatedSource::uniform(std::mt19937& rng, double lo, double hi) {
  const double u = (static_cast<double>(rng()) + 0.5) / 4294967296.0;  // (0, 1)
  return lo + (hi - lo) * u;
}

double SimulatedSource::gaussian(double sigma) {
  const double u1 = uniform(rng_, 0.0, 1.0), u2 = uniform(rng_, 0.0, 1.0);
  return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

// ---------------------------------------------------------------------------------------
// Scenario scripts: a sequence of segments, each a simple autopilot target.

ControlInput SimulatedSource::scriptedInput(double t) {
  if (options_.scenario == Scenario::Manual) return manualInput_;

  const double alt = model_.state().position.z;
  if (!segment_.recovery && (alt < 800.0 || alt > 9000.0)) {
    // Altitude guard: abandon the manoeuvre and level off.
    segment_ = ScriptSegment{};
    segment_.durationS = 4.0;
    segment_.recovery = true;
    scriptPhaseStart_ = t;
  } else if (t - scriptPhaseStart_ >= segment_.durationS) {
    segment_ = nextSegment();
    scriptPhaseStart_ = t;
  }
  return controlFor(segment_);
}

SimulatedSource::ScriptSegment SimulatedSource::nextSegment() {
  using Mode = ScriptSegment::NzMode;
  const int step = scriptStep_++;
  ScriptSegment seg;  // default: wings-level altitude hold
  seg.recovery = true;
  auto urand = [this](double lo, double hi) { return uniform(scriptRng_, lo, hi); };

  switch (options_.scenario) {
    case Scenario::LevelCruise:
    case Scenario::Manual:
    case Scenario::Count:
      seg.durationS = 1e9;
      break;

    case Scenario::SustainedTurn: {
      const double dir = (step / 4) % 2 == 0 ? 1.0 : -1.0;  // alternate turn direction
      seg.maxRollRateDegS = 120.0;
      switch (step % 4) {
        case 0: seg.durationS = step == 0 ? 2.0 : 4.0; break;               // cruise
        case 1: seg.durationS = 1.0; seg.bankDeg = 82.0 * dir; break;        // roll in
        case 2:                                                               // 7 G hold
          seg.durationS = 8.0; seg.mode = Mode::FixedLevelBank; seg.nz = 7.0;
          seg.bankDeg = dir; seg.recovery = false;
          break;
        default: seg.durationS = 2.0; break;                                  // roll out
      }
      break;
    }

    case Scenario::PullUpPushOver:
      switch (step % 5) {
        case 0: seg.durationS = step == 0 ? 2.0 : 3.0; break;                  // cruise
        case 1: seg.durationS = 2.0; seg.mode = Mode::Fixed; seg.nz = 5.0; break;  // pull
        case 2: seg.durationS = 0.5; seg.mode = Mode::Fixed; seg.nz = 1.0; break;  // unload
        case 3: seg.durationS = 2.0; seg.mode = Mode::Fixed; seg.nz = -2.0; break; // push
        default: seg.durationS = 4.0; seg.nzMax = 4.0; break;                  // recover
      }
      if (seg.mode == Mode::Fixed) seg.recovery = false;
      break;

    case Scenario::DogfightMix: {
      const double alt = model_.state().position.z;
      if (step == 0) { seg.durationS = 2.0; break; }
      if (step % 2 == 1 || alt < 1500.0 || alt > 6000.0) {  // level off between manoeuvres
        seg.durationS = (alt < 1500.0 || alt > 6000.0) ? 4.0 : urand(1.0, 2.5);
        seg.maxRollRateDegS = 150.0;
        seg.nzMax = 6.0;
        break;
      }
      seg.recovery = false;
      const double side = urand(0.0, 1.0) < 0.5 ? -1.0 : 1.0;
      const double r = urand(0.0, 1.0);
      if (r < 0.45) {  // sustained hard turn
        seg.mode = Mode::FixedLevelBank;
        seg.nz = urand(5.0, 9.0);
        seg.bankDeg = side;
        seg.durationS = urand(2.0, 5.0);
        seg.maxRollRateDegS = urand(120.0, 240.0);
      } else if (r < 0.65) {  // snap pull at an arbitrary bank (climbing / diving)
        seg.mode = Mode::Fixed;
        seg.nz = urand(3.0, 8.0);
        seg.bankDeg = side * urand(30.0, 150.0);
        seg.durationS = urand(1.0, 2.5);
        seg.maxRollRateDegS = urand(120.0, 270.0);
      } else if (r < 0.82) {  // negative-G push (inverted when low, so it climbs)
        seg.mode = Mode::Fixed;
        seg.nz = urand(-3.0, -0.5);
        seg.bankDeg = alt < kCruiseAltitudeM ? 180.0 : 0.0;
        seg.durationS = urand(0.8, 2.0);
        seg.maxRollRateDegS = 180.0;
      } else {  // roll to a steep or inverted attitude and hold altitude there
        seg.bankDeg = side * urand(100.0, 180.0);
        seg.nzMin = -3.0;
        seg.nzMax = 4.0;
        seg.durationS = urand(1.5, 3.0);
        seg.maxRollRateDegS = urand(90.0, 240.0);
      }
      break;
    }
  }
  return seg;
}

ControlInput SimulatedSource::controlFor(const ScriptSegment& seg) const {
  using Mode = ScriptSegment::NzMode;
  const FlightState& s = model_.state();
  const FlightModelParams& p = model_.params();
  const double speed = std::max(length(s.velocity), 1.0);
  const Vec3 vhat = s.velocity / speed;
  const double gamma = std::asin(std::clamp(vhat.z, -1.0, 1.0));

  // Bank angle of the lift axis relative to the vertical plane through the velocity.
  // Undefined when flying vertically; the script then simply stops rolling.
  Vec3 rh = cross(kWorldUp, vhat);
  const double rhLen = length(rh);
  const bool bankKnown = rhLen > 0.05;
  double bank = 0.0;
  if (bankKnown) {
    rh = rh / rhLen;
    const Vec3 uh = cross(vhat, rh);
    bank = std::atan2(dot(model_.liftDir(), rh), dot(model_.liftDir(), uh)) * kRadToDeg;
  }

  // Vertical load factor that steers the flight path towards a gentle climb/descent back
  // to the cruise altitude.
  const double gammaCmd = std::clamp((kCruiseAltitudeM - s.position.z) * 0.003, -0.25, 0.25);
  const double nVert = std::cos(gamma) + speed * (gammaCmd - gamma) / (kFlightPathTauS * kG0);

  ControlInput in;
  double bankTarget = seg.bankDeg;
  switch (seg.mode) {
    case Mode::AltitudeHold: {
      double c = std::cos(bank * kDegToRad);
      if (std::abs(c) < 0.15) c = std::copysign(0.15, c);
      in.nzCmd = std::clamp(nVert / c, seg.nzMin, seg.nzMax);
      break;
    }
    case Mode::Fixed:
      in.nzCmd = seg.nz;
      break;
    case Mode::FixedLevelBank: {
      in.nzCmd = seg.nz;
      const double ratio = std::clamp(nVert / seg.nz, -1.0, 1.0);
      bankTarget = std::acos(ratio) * kRadToDeg * (seg.bankDeg < 0.0 ? -1.0 : 1.0);
      break;
    }
  }
  in.rollRateDegS = bankKnown ? std::clamp(kRollGain * wrap180(bankTarget - bank),
                                           -seg.maxRollRateDegS, seg.maxRollRateDegS)
                              : 0.0;

  // Auto-throttle: feed-forward of drag and climb plus a speed error term.
  const double ratio = p.refSpeed / speed;
  const double dragG = p.dragCoef * speed * speed +
                       p.inducedDragCoef * s.appliedNz * s.appliedNz * ratio * ratio;
  const double needG = dragG + std::sin(gamma) + 0.03 * (kCruiseSpeedMS - speed);
  in.throttle = std::clamp(needG / p.maxThrustG, 0.0, 1.0);
  return in;
}

}  // namespace ft
