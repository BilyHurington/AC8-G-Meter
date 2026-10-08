#include "core/pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include "core/filters.h"
#include "core/high_g_timer.h"
#include "core/kinematics.h"
#include "core/mission_tracker.h"
#include "core/noise_estimator.h"
#include "core/sanitizer.h"

namespace ft {

namespace {

constexpr int kMinOnsetPoints = 3;
// A jump that persists for this many samples is a real state change (respawn, mission
// restart), not a glitch: accept it and restart the estimators.
constexpr int kMaxConsecutiveSpikes = 3;
// Position-only spike gate: a new position further than this from the fit's prediction is
// a glitch. The noise term adapts to the input's jitter; the floor covers what the
// quadratic extrapolation cannot follow in hard manoeuvres (worst seen on clean simulated
// dogfights: 0.02 m at 60 fps, 0.4 m at 10 fps).
constexpr double kPositionGateSigmas = 8.0;
constexpr double kPositionGateFloorM = 1.0;
// Velocity spike gate: besides spikeDeviationG * g0 * dt, a residual is tolerated up to
// this many times the input's measured per-axis noise sigma. Per-step velocity noise grows
// into huge implied accelerations at high sample rates (0.2 m/s of noise reads as ~30 G rms
// per axis at 1000 Hz), so a pure G threshold would reject real samples there; this term
// keeps the gate above the noise at any rate. The residual against the prediction has ~1.5
// sigma per axis (new sample, newest sample, trend), so 15 sigma leaves a wide margin even
// for heavy-tailed noise; game glitches are tens to hundreds of m/s. The floor covers
// start-up, before the noise estimate exists.
constexpr double kVelocityGateNoiseSigmas = 15.0;
constexpr double kVelocityGateFloorMS = 1.0;
// Adaptive filtering: the window never spans fewer than this many sample intervals, and
// follows the ideal window from the noise estimate with this time constant (no jumps).
constexpr double kAdaptiveMinWindowSamples = 2.5;
constexpr double kAdaptiveOnsetMinSamples = 4.0;
constexpr double kAdaptiveWindowTauS = 1.0;

bool needsFilterReset(const PipelineConfig& a, const PipelineConfig& b) {
  return a.accelWindowS != b.accelWindowS ||
         a.accelWindowPositionOnlyS != b.accelWindowPositionOnlyS ||
         a.onsetWindowS != b.onsetWindowS || a.medianWindow != b.medianWindow || a.gMode != b.gMode;
}

}  // namespace

struct TelemetryPipeline::Impl {
  PipelineConfig cfg;
  std::vector<IMetricsConsumer*> consumers;

  // Estimators: everything here restarts on a gap, teleport, pause or resize.
  KinematicsEstimator kinematics;
  TimedWindow<Basis> attitudes;        // attitude history spanning the kinematics window
  std::array<Smoother, 3> bodySmooth;  // Nx, Ny, Nz; `g` is derived from these
  TimedWindow<double> onsetWindow;
  // Velocity input noise and sample rate: a property of the source, so it survives estimator
  // resets (cleared only by reset()). Shared by the spike gate and the adaptive windows.
  NoiseEstimator noise;
  double adaptiveWindowS = 0.0;  // smoothed ideal velocity window, 0 until the noise is known
  FilterState filter;            // windows in use (published in the snapshot)

  HighGTimer highG;
  MissionTracker mission;

  // Last accepted sample (survives estimator resets; needed for dt and teleport checks).
  bool hasLast = false;
  double lastT = 0.0;
  Vec3 lastPos;
  std::optional<Vec3> lastVel;
  bool resetPending = false;  // restart estimators on the next accepted sample
  int consecutiveSpikes = 0;

  FlightMetrics current;
  MissionPhase phase = MissionPhase::None;
  std::shared_ptr<const MissionReport> lastReport;
  RecentHistory recent;
  double recentAccumS = 0.0;
  DataQuality quality;

  explicit Impl(const PipelineConfig& c) : cfg(c) {
    applyConfig(true);
    recentAccumS = 1.0 / recent.sampleHz;  // first valid sample is recorded immediately
  }

  void applyConfig(bool resize) {
    highG.setThresholds(cfg.highGEnter, cfg.highGExit);
    if (resize) {
      for (Smoother& s : bodySmooth) s.configure(cfg.medianWindow, cfg.smoothingTauS);
      resetEstimators();
    }
    selectWindows(lastVel.has_value(), 0.0);
  }

  void resetEstimators() {
    kinematics.reset();
    attitudes.reset();
    for (Smoother& s : bodySmooth) s.reset();
    onsetWindow.reset();
    noise.restartSequence();
  }

  // Chooses the filter windows for the next sample (see PipelineConfig::adaptiveFiltering).
  // Only the windows change, never the estimator state: every window trims by time, so a
  // new length simply applies from the next sample on.
  void selectWindows(bool velocityInput, double stepDt) {
    if (velocityInput && noise.ready() && noise.sampleInterval() > 0.0 && cfg.adaptiveTargetG > 0.0) {
      const double dt = noise.sampleInterval();
      const double sigma = noise.sigma();
      const double target = cfg.adaptiveTargetG * kG0;
      // The sample floor wins at very low rates (< ~21 Hz with the default clamp).
      const double ideal = std::max(std::min(std::cbrt(12.0 * sigma * sigma * dt / (target * target)),
                                             cfg.adaptiveMaxWindowS),
                                    kAdaptiveMinWindowSamples * dt);
      // Tracked even while adaptive filtering is off, so switching it on is seamless.
      adaptiveWindowS = adaptiveWindowS > 0.0
                            ? adaptiveWindowS + (1.0 - std::exp(-stepDt / kAdaptiveWindowTauS)) *
                                                    (ideal - adaptiveWindowS)
                            : ideal;
    }
    filter.sampleRateHz = noise.sampleInterval() > 0.0 ? 1.0 / noise.sampleInterval() : 0.0;
    filter.velocityNoiseMS = noise.sigma();
    filter.adaptive = cfg.adaptiveFiltering && velocityInput && adaptiveWindowS > 0.0;
    if (filter.adaptive) {
      const double dt = noise.sampleInterval();
      filter.accelWindowS = adaptiveWindowS;
      filter.smoothingTauS = cfg.adaptiveTauRatio * adaptiveWindowS;
      filter.onsetWindowS = std::max(std::min(cfg.adaptiveOnsetRatio * adaptiveWindowS, cfg.onsetWindowS),
                                     kAdaptiveOnsetMinSamples * dt);
    } else {
      filter.accelWindowS = velocityInput ? cfg.accelWindowS : cfg.accelWindowPositionOnlyS;
      filter.smoothingTauS = cfg.smoothingTauS;
      filter.onsetWindowS = cfg.onsetWindowS;
    }
    kinematics.setWindows(velocityInput ? filter.accelWindowS : cfg.accelWindowS,
                          cfg.accelWindowPositionOnlyS);
    for (Smoother& s : bodySmooth) s.setTau(filter.smoothingTauS);
  }

  // G mode changed: old and new `g` values are different quantities and must not share
  // extremes, histogram, high-G episodes or history.
  void resetGStats() {
    if (mission.open()) mission.resetGStats();
    highG.reset();
    recent.clear();
    recentAccumS = 1.0 / recent.sampleHz;
  }

  // Discontinuity in the aircraft state (teleport, respawn): estimators and the current
  // high-G episode are meaningless across it.
  void discontinuity() {
    resetEstimators();
    highG.reset();
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onReset();
  }

  // ---- mission state machine ------------------------------------------------------

  void onPhase(MissionPhase next) {
    const MissionPhase prev = phase;
    phase = next;
    // Game time stops while paused, so the first post-pause dt can look perfectly
    // normal even though the aircraft state jumped: always restart the estimators.
    if (prev == MissionPhase::Paused && next != MissionPhase::Paused) resetPending = true;

    switch (next) {
      case MissionPhase::Active:
        if (!mission.open()) startMission();
        break;
      case MissionPhase::Ended:
        if (mission.open()) finishMission(false);
        break;
      case MissionPhase::None:
        if (mission.open()) finishMission(true);
        break;
      case MissionPhase::Paused:
        break;
    }
  }

  void startMission() {
    mission.start(cfg.timelineHz);
    highG.reset();
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onMissionStarted();
  }

  void finishMission(bool aborted) {
    MissionReport r = mission.finish(aborted);
    r.extremeGThreshold = cfg.extremeGThreshold;
    auto report = std::make_shared<const MissionReport>(std::move(r));
    lastReport = report;
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onMissionEnded(*report);
  }

  // ---- per-sample processing ------------------------------------------------------

  void reject(std::uint64_t DataQuality::*counter) {
    ++(quality.*counter);
    if (phase == MissionPhase::Active) mission.countRejected();
  }

  // Spike gate. The acceleration estimate is a least-squares fit over the kinematics
  // window, so a single bad velocity/position sample would bias every estimate for as
  // long as it stays in the window: a burst of plausible-looking garbage several samples
  // long that the median filter (it only removes isolated outliers) cannot catch. So each
  // sample is checked for consistency with the recent motion *before* it enters the window.
  // Only consistency: the G value itself is never capped.
  //   velocity input: the distance of the new velocity from the one predicted by the last
  //     sample and the current acceleration trend, against the larger of
  //     spikeDeviationG * g0 * dt (an acceleration change no aircraft makes in one step)
  //     and a multiple of the input's measured velocity noise (NoiseEstimator).
  //   position only : the distance from where the window's quadratic fit predicts the
  //     aircraft, against a threshold scaled to the fit's residual noise. (A two-point
  //     second difference would amplify position noise by ~4 / dt^2: 1 cm of jitter
  //     reads as ~15 G at 60 fps.)
  // The median filter (if enabled) then handles smaller isolated outliers in the G signal.
  bool isSpike(const RawFlightSample& s, double dt) const {
    if (!s.velocity) {
      const std::optional<Vec3> predicted = kinematics.predictPosition(s.t);
      if (!predicted) return false;
      const double limit =
          std::max(kPositionGateSigmas * kinematics.fitResidualSigma(), kPositionGateFloorM);
      return length(s.position - *predicted) > limit;
    }
    const std::optional<Vec3> predicted = kinematics.predictVelocity(s.t);
    if (!predicted) return false;
    const double limit = std::max({cfg.spikeDeviationG * kG0 * dt,
                                   kVelocityGateNoiseSigmas * noise.sigma(),
                                   kVelocityGateFloorMS});
    return length(*s.velocity - *predicted) > limit;
  }

  // Position jump inconsistent with the motion (respawn, checkpoint). Position-only input
  // is measured against the same fit prediction as the spike gate.
  bool isTeleport(const RawFlightSample& s, double dt) const {
    if (!s.velocity) {
      if (const std::optional<Vec3> predicted = kinematics.predictPosition(s.t))
        return length(s.position - *predicted) > cfg.teleportToleranceM;
    }
    return ft::isTeleport(lastPos, lastVel, s, dt, cfg.teleportToleranceM);
  }

  // Attitude at time t, interpolated between the two history samples around it. The
  // acceleration is a window average that refers to ~windowS / 2 ago; projecting it on
  // the newest attitude would turn every roll into spurious side force (Ny). Linear
  // interpolation of the axes + re-orthonormalisation is plenty for frame-to-frame
  // rotations; if the step is too large for that, the nearer sample is used.
  Basis attitudeAt(double t) const {
    const int n = attitudes.size();
    if (n == 1) return attitudes.value(0);
    int i = 1;
    while (i < n - 1 && attitudes.time(i) < t) ++i;
    const double t0 = attitudes.time(i - 1), t1 = attitudes.time(i);
    const double w = std::clamp((t - t0) / (t1 - t0), 0.0, 1.0);
    const Basis& a = attitudes.value(i - 1);
    const Basis& b = attitudes.value(i);
    Basis lerp{a.forward + (b.forward - a.forward) * w, a.right + (b.right - a.right) * w,
               a.up + (b.up - a.up) * w};
    if (!sanitizeAttitude(lerp)) return w < 0.5 ? a : b;
    return lerp;
  }

  void push(const RawFlightSample& raw) {
    onPhase(raw.phase);
    if (raw.phase == MissionPhase::Paused) return;  // frozen: no metrics, no stats

    RawFlightSample s = raw;
    if (!isFiniteSample(s) || !sanitizeAttitude(s.attitude)) {
      reject(&DataQuality::rejectedNonFinite);
      return;
    }
    const double dt = hasLast ? s.t - lastT : 0.0;
    // Duplicates and slightly out-of-order samples are dropped. A large backwards jump
    // is the game clock restarting (level reload): treat it like a gap, otherwise every
    // later sample would be rejected as "old".
    const bool clockRestart = hasLast && dt < -cfg.maxGapS;
    if (hasLast && dt <= 0.0 && !clockRestart) {
      reject(&DataQuality::rejectedTime);
      return;
    }

    // Mission time skips pauses and gaps (unknown what happened in between) but counts
    // everything else, including estimator warm-up.
    bool countTime = hasLast;
    if (resetPending) {
      resetEstimators();
      countTime = false;
    } else if (hasLast) {
      if (dt > cfg.maxGapS || clockRestart) {
        resetEstimators();
        ++quality.resetsGap;
        countTime = false;
      } else if (isTeleport(s, dt)) {
        discontinuity();
        ++quality.resetsTeleport;
      } else if (isSpike(s, dt)) {
        if (++consecutiveSpikes < kMaxConsecutiveSpikes) {
          reject(&DataQuality::rejectedSpike);
          return;
        }
        discontinuity();
        ++quality.resetsTeleport;
      }
    }
    resetPending = false;
    consecutiveSpikes = 0;
    const double stepDt = std::max(dt, 0.0);  // negative only on a clock restart

    if (s.velocity) noise.add(s.t, *s.velocity);
    selectWindows(s.velocity.has_value(), stepDt);
    kinematics.add(s.t, s.position, s.velocity);
    attitudes.push(s.t, s.attitude);
    attitudes.trim(kinematics.windowS(), KinematicsEstimator::kMinPoints);
    hasLast = true;
    lastT = s.t;
    lastPos = s.position;
    lastVel = s.velocity;

    // Start from the previous metrics so fields that can't be computed yet keep their
    // last value; `valid` tells consumers whether to trust them.
    FlightMetrics m = current;
    m.t = s.t;
    m.valid = false;
    m.altitude = s.position.z;
    computeAirData(s, m);

    HighGTimer::Step highGStep;
    bool extremeG = false;
    if (kinematics.hasAcceleration()) {
      // Load factor = specific force in G: what an accelerometer in the seat measures.
      // Projected on the attitude at the instant the acceleration estimate refers to.
      // Used as measured, however large: glitches are caught by the spike gate above.
      const Vec3 n = (kinematics.acceleration() - kGravityWorld) / kG0;
      const Vec3 nBody = attitudeAt(kinematics.accelerationTime()).toBody(n);
      extremeG = length(nBody) > cfg.extremeGThreshold;
      m.gRaw = cfg.gMode == GMode::Nz ? nBody.z : length(nBody);
      m.loadFactorBody = {bodySmooth[0].update(nBody.x, stepDt),
                          bodySmooth[1].update(nBody.y, stepDt),
                          bodySmooth[2].update(nBody.z, stepDt)};
      m.gTotal = length(m.loadFactorBody);
      m.g = cfg.gMode == GMode::Nz ? m.loadFactorBody.z : m.gTotal;

      onsetWindow.push(s.t, m.g);
      onsetWindow.trim(filter.onsetWindowS, kMinOnsetPoints);
      m.onsetRate = onsetWindow.slope();
      m.valid = onsetWindow.size() >= kMinOnsetPoints;
    }

    if (m.valid) highGStep = highG.update(m.g, stepDt);
    m.inHighG = highG.active();
    m.highGDurationS = highG.durationS();

    ++quality.accepted;
    if (extremeG) ++quality.extremeG;
    if (phase == MissionPhase::Active) {
      mission.countAccepted();
      if (extremeG) mission.countExtremeG();
      mission.addTime(countTime ? stepDt : 0.0);
      if (m.valid) mission.add(m, stepDt, highGStep.entered);
    }
    if (m.valid) recordRecent(m.g, stepDt);
    current = m;

    // Every phase but Paused (which returned above).
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onMetrics(m, phase);
  }

  void computeAirData(const RawFlightSample& s, FlightMetrics& m) const {
    const Vec3 v = kinematics.hasVelocity() ? kinematics.velocity() : Vec3{};
    m.speed = length(v);
    m.aoaValid = kinematics.hasVelocity() && m.speed >= cfg.minSpeedForAoa;
    if (!m.aoaValid) return;
    const Vec3 vb = s.attitude.toBody(v);
    // Nose above the velocity vector <=> velocity points below the nose (vb.z < 0).
    m.aoaDeg = std::atan2(-vb.z, vb.x) * kRadToDeg;
    // Sideslip sign: a velocity component along +right means the aircraft is moving to
    // the right of where its nose points, so the relative wind (-v) arrives from the
    // right. Positive v.right therefore gives positive sideslip ("air from the right").
    m.sideslipDeg = std::asin(std::clamp(vb.y / m.speed, -1.0, 1.0)) * kRadToDeg;
  }

  // Time-based decimation so the history spans the same seconds at any frame rate.
  void recordRecent(double g, double dt) {
    const double period = 1.0 / recent.sampleHz;
    recentAccumS += dt;
    int pushes = 0;
    while (recentAccumS >= period && pushes < RecentHistory::kCapacity) {
      recent.push(static_cast<float>(g));
      recentAccumS -= period;
      ++pushes;
    }
    if (pushes == RecentHistory::kCapacity) recentAccumS = 0.0;
  }

  void reset() {
    resetEstimators();
    highG.reset();
    mission.clearStats();
    recent.clear();
    recentAccumS = 1.0 / recent.sampleHz;
    quality = {};
    current = {};
    hasLast = false;
    resetPending = false;
    consecutiveSpikes = 0;
    noise.reset();
    adaptiveWindowS = 0.0;
    selectWindows(false, 0.0);
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onReset();
  }
};

TelemetryPipeline::TelemetryPipeline(const PipelineConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

TelemetryPipeline::~TelemetryPipeline() = default;

void TelemetryPipeline::setConfig(const PipelineConfig& config) {
  const bool resize = needsFilterReset(impl_->cfg, config);
  const bool gModeChanged = config.gMode != impl_->cfg.gMode;
  impl_->cfg = config;
  impl_->applyConfig(resize);
  if (gModeChanged) impl_->resetGStats();
}

const PipelineConfig& TelemetryPipeline::config() const { return impl_->cfg; }

void TelemetryPipeline::push(const RawFlightSample& sample) { impl_->push(sample); }

void TelemetryPipeline::reset() { impl_->reset(); }

MetricsSnapshot TelemetryPipeline::snapshot() const {
  MetricsSnapshot snap;
  snap.current = impl_->current;
  snap.phase = impl_->phase;
  snap.mission = impl_->mission.stats();
  snap.lastReport = impl_->lastReport;
  snap.recent = impl_->recent;
  snap.quality = impl_->quality;
  snap.filter = impl_->filter;
  return snap;
}

void TelemetryPipeline::addConsumer(IMetricsConsumer* consumer) {
  auto& list = impl_->consumers;
  if (consumer && std::find(list.begin(), list.end(), consumer) == list.end()) list.push_back(consumer);
}

void TelemetryPipeline::removeConsumer(IMetricsConsumer* consumer) {
  auto& list = impl_->consumers;
  list.erase(std::remove(list.begin(), list.end(), consumer), list.end());
}

}  // namespace ft
