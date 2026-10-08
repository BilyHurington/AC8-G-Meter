#include "core/pipeline.h"

#include <algorithm>
#include <utility>

#include "core/adaptive_windows.h"
#include "core/filters.h"
#include "core/gated_kinematics.h"
#include "core/high_g_timer.h"
#include "core/live_g.h"
#include "core/mission_session.h"
#include "core/noise_estimator.h"
#include "core/sample_ledger.h"
#include "core/sanitizer.h"

namespace ft {

namespace {

bool needsFilterReset(const PipelineConfig& a, const PipelineConfig& b) {
  return a.accelWindowS != b.accelWindowS || a.accelWindowPositionOnlyS != b.accelWindowPositionOnlyS ||
         a.onsetWindowS != b.onsetWindowS || a.medianWindow != b.medianWindow || a.gMode != b.gMode;
}

}  // namespace

// Orchestration only: each stage lives in its own unit (see pipeline.h for the order).
struct TelemetryPipeline::Impl {
  PipelineConfig cfg;
  std::vector<IMetricsConsumer*> consumers;
  MissionSession session{consumers};       // phases, missions, reports
  SampleLedger ledger{session.tracker()};  // accepted / rejected counts

  // Velocity input noise and sample rate: a property of the source, so it survives estimator
  // resets (cleared only by reset()). Shared by the spike gate and the adaptive windows.
  NoiseEstimator noise;
  AdaptiveWindows windows;
  // Estimators: everything below restarts on a gap, teleport, pause or resize.
  GatedKinematics kin;
  LiveG liveG;                // smoothing, G per GMode, onset rate
  HighGTimer highG;           // the live (HUD) high-G episode
  bool resetPending = false;  // restart estimators on the next accepted sample

  FlightMetrics current;
  RecentRecorder recent;

  explicit Impl(const PipelineConfig& c) : cfg(c) { applyConfig(true); }

  void applyConfig(bool resize) {
    highG.setThresholds(cfg.highGEnter, cfg.highGExit);
    session.configure(cfg);
    if (resize) {
      liveG.configure(cfg.medianWindow, cfg.smoothingTauS);
      resetEstimators();
    }
    selectWindows(0.0);
  }

  void notifyReset() {
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onReset();
  }

  void resetEstimators() {
    kin.restart();
    liveG.reset();
  }

  // Windows for the next sample (AdaptiveWindows), applied to the estimators.
  void selectWindows(double stepDt) {
    const FilterState& f = windows.select(cfg, noise, kin.inputKind(), stepDt);
    kin.setWindows(f, cfg);
    liveG.setTau(f.smoothingTauS);
  }

  // G mode changed: old and new `g` values are different quantities and must not share
  // extremes, histogram, high-G episodes or history.
  void resetGStats() {
    session.resetGStats();
    highG.reset();
    recent.clear();
  }

  // Discontinuity in the aircraft state (teleport, respawn): estimators and the current
  // high-G episode are meaningless across it.
  void discontinuity() {
    resetEstimators();
    highG.reset();
    session.restartSustained(true);
    notifyReset();
  }

  void onPhase(MissionPhase next) {
    const MissionSession::Transition tr = session.setPhase(next, cfg);
    ledger.setPhase(next, tr.changed);
    // Game time stops while paused, so the first post-pause dt can look perfectly normal even
    // though the aircraft state jumped: always restart the estimators.
    if (tr.resumed) resetPending = true;
    if (tr.started) highG.reset();
  }

  // Runs the spike gate and books its verdict. The caller drops the sample on Reject.
  SpikeGate::Verdict applyGate(const RawFlightSample& s, double dt) {
    const SpikeGate::Verdict verdict = kin.check(s, dt, cfg.spikeDeviationG, noise);
    switch (verdict) {
      case SpikeGate::Verdict::DropPrevious:
        ledger.heldWereGlitches(kin.gate().resolved());
        break;
      case SpikeGate::Verdict::Reject:
        ledger.rejectSpike(kin.gate().runSize());
        break;
      case SpikeGate::Verdict::Jump:
        restartFromRun(s);
        break;
      default:
        break;
    }
    if (kin.noiseSuspected(s, verdict)) windows.noiseAboveGate(cfg);
    return verdict;
  }

  // A run of rejected samples that agree: a real jump.
  void restartFromRun(const RawFlightSample& s) {
    if (s.velocity) {
      // The position agrees (no teleport): a velocity step, possibly a real manoeuvre beyond
      // spikeDeviationG. Restart only the acceleration window; G display, high-G episode and
      // consumers carry on.
      kin.restart();
      ++ledger.quality().resetsJump;
    } else {
      discontinuity();
      ++ledger.quality().resetsTeleport;
    }
    kin.startFromRun();
    ledger.runWasReal();
  }

  // Sanitising, time checks, gaps / teleports and the spike gate. Returns false when the
  // sample is dropped; otherwise it is in the kinematics window and stepDt / countTime say how
  // much time it advances (mission time skips gaps: unknown what happened in between).
  bool admit(RawFlightSample& s, double& stepDt, bool& countTime, SpikeGate::Verdict& verdict) {
    if (!isFiniteSample(s) || !sanitizeAttitude(s.attitude)) {
      ledger.reject(&DataQuality::rejectedNonFinite);
      return false;
    }
    const bool hasLast = kin.hasLast();
    const double dt = hasLast ? s.t - kin.lastT() : 0.0;
    // Duplicates and slightly out-of-order samples are dropped. A large backwards jump is the
    // game clock restarting (level reload): treat it like a gap, otherwise every later sample
    // would be rejected as "old".
    const bool clockRestart = hasLast && dt < -cfg.maxGapS;
    if (hasLast && dt <= 0.0 && !clockRestart) {
      ledger.reject(&DataQuality::rejectedTime);
      return false;
    }
    countTime = hasLast;
    const bool gap = hasLast && (dt > cfg.maxGapS || clockRestart);
    if (resetPending || gap) {
      resetEstimators();
      noise.restartSequence();
      session.restartSustained(false);  // unknown what happened in between
      if (!resetPending) ++ledger.quality().resetsGap;
      countTime = false;
    }
    // Before the gate, which depends on it: an estimate from gate-accepted samples only could
    // never see noise above the current gate.
    if (s.velocity) noise.add(s.t, *s.velocity);
    verdict = SpikeGate::Verdict::Accept;
    if (hasLast && !resetPending && !gap) {
      if (kin.isTeleport(s, dt, cfg.teleportToleranceM)) {
        discontinuity();
        ++ledger.quality().resetsTeleport;
      } else {
        verdict = applyGate(s, dt);
        if (verdict == SpikeGate::Verdict::Reject) return false;
      }
    }
    resetPending = false;
    stepDt = std::max(dt, 0.0);  // negative only on a clock restart
    kin.recordLast(s);
    selectWindows(stepDt);
    kin.add(s, verdict);
    return true;
  }

  // Metrics of an admitted sample. Starts from the previous metrics so fields that can't be
  // computed yet keep their last value; `valid` tells consumers whether to trust them.
  FlightMetrics measure(const RawFlightSample& s, double stepDt, bool& extremeG) {
    FlightMetrics m = current;
    m.t = s.t;
    m.valid = false;
    m.altitude = s.position.z;
    kin.airData(s, cfg.minSpeedForAoa, m);
    extremeG = false;
    if (const std::optional<Vec3> nBody = kin.loadFactorBody()) {
      // Used as measured, however large: glitches are caught by the spike gate.
      extremeG = length(*nBody) > cfg.extremeGThreshold;
      liveG.update(*nBody, s.t, stepDt, cfg.gMode, windows.state().onsetWindowS, m);
    }
    return m;
  }

  void push(const RawFlightSample& raw) {
    onPhase(raw.phase);
    if (raw.phase == MissionPhase::Paused) return;  // frozen: no metrics, no stats

    RawFlightSample s = raw;
    double stepDt = 0.0;
    bool countTime = false;
    SpikeGate::Verdict verdict = SpikeGate::Verdict::Accept;
    if (!admit(s, stepDt, countTime, verdict)) return;

    bool extremeG = false;
    FlightMetrics m = measure(s, stepDt, extremeG);
    if (m.valid) highG.update(m.g, stepDt);
    m.inHighG = highG.active();
    m.highGDurationS = highG.durationS();

    ledger.accept(verdict, kin.gate().held());
    if (extremeG) ledger.countExtremeG();
    session.add(m, stepDt, countTime ? stepDt : 0.0);
    if (m.valid) recent.record(m.g, stepDt);
    current = m;

    // Every phase but Paused (which returned above).
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i]->onMetrics(m, session.phase());
  }

  void reset() {
    resetEstimators();
    highG.reset();
    session.clear();
    recent.clear();
    ledger.reset();
    current = {};
    kin.forget();
    resetPending = false;
    noise.reset();
    windows.reset();
    selectWindows(0.0);
    notifyReset();
  }
};

TelemetryPipeline::TelemetryPipeline(const PipelineConfig& config) : impl_(std::make_unique<Impl>(config)) {}

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

void TelemetryPipeline::setMissionLabel(std::string label) { impl_->session.setLabel(std::move(label)); }

void TelemetryPipeline::reset() { impl_->reset(); }

MetricsSnapshot TelemetryPipeline::snapshot() const {
  MetricsSnapshot snap;
  snap.current = impl_->current;
  snap.phase = impl_->session.phase();
  snap.mission = impl_->session.stats();
  snap.lastReport = impl_->session.lastReport();
  snap.recent = impl_->recent.history();
  snap.quality = impl_->ledger.quality();
  snap.filter = impl_->windows.state();
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
