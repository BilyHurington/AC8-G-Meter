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

bool needsFilterReset(const PipelineConfig& a, const PipelineConfig& b) {
  return a.accelWindowS != b.accelWindowS || a.onsetWindowS != b.onsetWindowS ||
         a.medianWindow != b.medianWindow || a.gMode != b.gMode;
}

}  // namespace

struct TelemetryPipeline::Impl {
  PipelineConfig cfg;
  std::vector<IMetricsConsumer*> consumers;

  // Estimators: everything here restarts on a gap, teleport, pause or resize.
  KinematicsEstimator kinematics;
  Smoother gSmooth;
  std::array<Smoother, 3> bodySmooth;  // Nx, Ny, Nz
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
    kinematics.setWindow(cfg.accelWindowS);
    highG.setThresholds(cfg.highGEnter, cfg.highGExit);
    if (resize) {
      gSmooth.configure(cfg.medianWindow, cfg.smoothingTauS);
      for (Smoother& s : bodySmooth) s.configure(cfg.medianWindow, cfg.smoothingTauS);
      resetEstimators();
    } else {
      gSmooth.setTau(cfg.smoothingTauS);
      for (Smoother& s : bodySmooth) s.setTau(cfg.smoothingTauS);
    }
  }

  void resetEstimators() {
    kinematics.reset();
    gSmooth.reset();
    for (Smoother& s : bodySmooth) s.reset();
    onsetWindow.reset();
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

  // Spike gate. The acceleration estimate is a least-squares slope over ~accelWindowS,
  // so a single bad velocity/position sample would bias every estimate for as long as it
  // stays in the window: a burst of plausible-looking garbage several samples long that
  // neither the median filter (it only removes isolated outliers) nor the post-hoc
  // |G| limit can catch. Instead, check the acceleration implied by the single step from
  // the last accepted sample against the current trend *before* the sample enters the
  // window. A deviation beyond plausibleGLimit cannot be flown, so the sample is dropped.
  // The median filter then handles smaller isolated outliers in the G signal.
  bool isSpike(const RawFlightSample& s) const {
    const std::optional<Vec3> step = kinematics.stepAcceleration(s.t, s.position, s.velocity);
    if (!step) return false;
    const Vec3 trend = kinematics.hasAcceleration() ? kinematics.acceleration() : Vec3{};
    return length(*step - trend) / kG0 > cfg.plausibleGLimit;
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
      } else if (isTeleport(lastPos, lastVel, s, dt, cfg.teleportToleranceM)) {
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
    if (kinematics.hasAcceleration()) {
      // Load factor = specific force in G: what an accelerometer in the seat measures.
      const Vec3 n = (kinematics.acceleration() - kGravityWorld) / kG0;
      const Vec3 nBody = s.attitude.toBody(n);
      const double gRaw = cfg.gMode == GMode::Nz ? nBody.z : length(nBody);
      if (std::abs(gRaw) > cfg.plausibleGLimit) {
        reject(&DataQuality::rejectedImplausible);
        return;
      }
      m.gRaw = gRaw;
      m.g = gSmooth.update(gRaw, stepDt);
      m.loadFactorBody = {bodySmooth[0].update(nBody.x, stepDt),
                          bodySmooth[1].update(nBody.y, stepDt),
                          bodySmooth[2].update(nBody.z, stepDt)};
      m.gTotal = length(m.loadFactorBody);

      onsetWindow.push(s.t, m.g);
      onsetWindow.trim(cfg.onsetWindowS, kMinOnsetPoints);
      m.onsetRate = onsetWindow.slope();
      m.valid = onsetWindow.size() >= kMinOnsetPoints;
    }

    if (m.valid) highGStep = highG.update(m.g, stepDt);
    m.inHighG = highG.active();
    m.highGDurationS = highG.durationS();

    ++quality.accepted;
    if (phase == MissionPhase::Active) {
      mission.countAccepted();
      if (m.valid) mission.add(m, stepDt, highGStep.entered);
    }
    if (m.valid) recordRecent(m.g, stepDt);
    current = m;

    if (phase == MissionPhase::Active || phase == MissionPhase::None) {
      for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onMetrics(m, phase);
    }
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
  impl_->cfg = config;
  impl_->applyConfig(resize);
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
