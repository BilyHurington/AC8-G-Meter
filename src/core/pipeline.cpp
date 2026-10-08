#include "core/pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "core/filters.h"
#include "core/high_g_timer.h"
#include "core/kinematics.h"
#include "core/mission_tracker.h"
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
    kinematics.setWindows(cfg.accelWindowS, cfg.accelWindowPositionOnlyS);
    highG.setThresholds(cfg.highGEnter, cfg.highGExit);
    if (resize) {
      for (Smoother& s : bodySmooth) s.configure(cfg.medianWindow, cfg.smoothingTauS);
      resetEstimators();
    } else {
      for (Smoother& s : bodySmooth) s.setTau(cfg.smoothingTauS);
    }
  }

  void resetEstimators() {
    kinematics.reset();
    attitudes.reset();
    for (Smoother& s : bodySmooth) s.reset();
    onsetWindow.reset();
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
    auto report = std::make_shared<const MissionReport>(mission.finish(aborted));
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
  // long that neither the median filter (it only removes isolated outliers) nor the
  // post-hoc |G| limit can catch. So each sample is checked *before* it enters the window:
  //   velocity input: the acceleration implied by the single step from the last sample,
  //     against the current trend. A deviation beyond plausibleGLimit cannot be flown.
  //   position only : the distance from where the window's quadratic fit predicts the
  //     aircraft, against a threshold scaled to the fit's residual noise. (A two-point
  //     second difference would amplify position noise by ~4 / dt^2: 1 cm of jitter
  //     reads as ~15 G at 60 fps.)
  // The median filter then handles smaller isolated outliers in the G signal.
  bool isSpike(const RawFlightSample& s) const {
    if (!s.velocity) {
      const std::optional<Vec3> predicted = kinematics.predictPosition(s.t);
      if (!predicted) return false;
      const double limit =
          std::max(kPositionGateSigmas * kinematics.fitResidualSigma(), kPositionGateFloorM);
      return length(s.position - *predicted) > limit;
    }
    const std::optional<Vec3> step = kinematics.stepAcceleration(s.t, s.velocity);
    if (!step) return false;
    const Vec3 trend = kinematics.hasAcceleration() ? kinematics.acceleration() : Vec3{};
    return length(*step - trend) / kG0 > cfg.plausibleGLimit;
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

    if (resetPending) {
      resetEstimators();
    } else if (hasLast) {
      if (dt > cfg.maxGapS || clockRestart) {
        resetEstimators();
        ++quality.resetsGap;
      } else if (isTeleport(s, dt)) {
        discontinuity();
        ++quality.resetsTeleport;
      } else if (isSpike(s)) {
        if (++consecutiveSpikes < kMaxConsecutiveSpikes) {
          reject(&DataQuality::rejectedImplausible);
          return;
        }
        discontinuity();
        ++quality.resetsTeleport;
      }
    }
    resetPending = false;
    consecutiveSpikes = 0;
    const double stepDt = std::max(dt, 0.0);  // negative only on a clock restart

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
    bool implausible = false;
    if (kinematics.hasAcceleration()) {
      // Load factor = specific force in G: what an accelerometer in the seat measures.
      // Projected on the attitude at the instant the acceleration estimate refers to.
      const Vec3 n = (kinematics.acceleration() - kGravityWorld) / kG0;
      const Vec3 nBody = attitudeAt(kinematics.accelerationTime()).toBody(n);
      implausible = length(nBody) > cfg.plausibleGLimit;
      if (implausible) {
        // Glitch that got past the spike gate: keep it out of the smoothers and hold the
        // last filtered values, but still account the sample's time (mission duration,
        // histogram, high-G clock) so a glitch burst does not shorten the mission.
        reject(&DataQuality::rejectedImplausible);
        m.valid = onsetWindow.size() >= kMinOnsetPoints;
      } else {
        m.gRaw = cfg.gMode == GMode::Nz ? nBody.z : length(nBody);
        m.loadFactorBody = {bodySmooth[0].update(nBody.x, stepDt),
                            bodySmooth[1].update(nBody.y, stepDt),
                            bodySmooth[2].update(nBody.z, stepDt)};
        m.gTotal = length(m.loadFactorBody);
        m.g = cfg.gMode == GMode::Nz ? m.loadFactorBody.z : m.gTotal;

        onsetWindow.push(s.t, m.g);
        onsetWindow.trim(cfg.onsetWindowS, kMinOnsetPoints);
        m.onsetRate = onsetWindow.slope();
        m.valid = onsetWindow.size() >= kMinOnsetPoints;
      }
    }

    if (m.valid) highGStep = highG.update(m.g, stepDt);
    m.inHighG = highG.active();
    m.highGDurationS = highG.durationS();

    if (!implausible) ++quality.accepted;
    if (phase == MissionPhase::Active) {
      if (!implausible) mission.countAccepted();
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
