#include "core/spike_gate.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace ft {

namespace {

// Position-only gate: a new position further than this from the fit's prediction is a
// glitch. The noise term adapts to the input's jitter; the floor covers what the quadratic
// extrapolation cannot follow in hard manoeuvres (worst seen on clean simulated dogfights:
// 0.02 m at 60 fps, 0.4 m at 10 fps).
constexpr double kPositionGateSigmas = 8.0;
constexpr double kPositionGateFloorM = 1.0;
// Velocity gate: besides spikeDeviationG * g0 * dt, a residual is tolerated up to this many
// times the input's measured per-axis noise sigma. Per-step velocity noise grows into huge
// implied accelerations at high sample rates (0.2 m/s of noise reads as ~30 G rms per axis at
// 1000 Hz), so a pure G threshold would reject real samples there; this term keeps the gate
// above the noise at any rate. The residual against the prediction has ~1.5 sigma per axis
// (new sample, newest sample, trend), so 15 sigma leaves a wide margin even for heavy-tailed
// noise; game glitches are tens to hundreds of m/s. The floor covers start-up.
constexpr double kVelocityGateNoiseSigmas = 15.0;
constexpr double kVelocityGateFloorMS = 1.0;
// The suspect bound is at most this fraction of the gate limit.
constexpr double kLookBack = 0.5;
// A run of rejected samples is a jump if each one lies within this fraction of the gate limit
// of the line through the first and last (a real jump: they agree to within their noise,
// which the gate limit exceeds 15-fold, or follow a steady acceleration). Noise the gate
// underestimated scatters by about the limit itself.
constexpr double kRunAgreement = 0.25;
// Two rejections in a row scatter if their deviations from the prediction differ in direction
// by more than ~25 degrees (a real jump's agree within ~10 degrees: the jump exceeds the gate,
// which is at least 15 noise sigmas).
constexpr double kRunDirectionCos = 0.9;

Vec3 gatedValue(const RawFlightSample& s) { return s.velocity ? *s.velocity : s.position; }

}  // namespace

SpikeGate::Verdict SpikeGate::check(const RawFlightSample& s, double dt,
                                    const KinematicsEstimator& kinematics, double spikeDeviationG,
                                    double velocityNoiseSigma, bool noiseKnown) {
  const Verdict verdict = decide(s, dt, kinematics, spikeDeviationG, velocityNoiseSigma, noiseKnown);
  const bool event = verdict == Verdict::Hold || verdict == Verdict::Reject;
  bool& slot = events_[static_cast<std::size_t>(eventIndex_)];
  busyEvents_ += (event ? 1 : 0) - (slot ? 1 : 0);
  slot = event;
  eventIndex_ = (eventIndex_ + 1) % kBusyWindow;
  return verdict;
}

SpikeGate::Verdict SpikeGate::decide(const RawFlightSample& s, double dt,
                                     const KinematicsEstimator& kinematics, double spikeDeviationG,
                                     double velocityNoiseSigma, bool noiseKnown) {
  const bool velocity = s.velocity.has_value();
  const std::optional<Vec3> predicted =
      velocity ? kinematics.predictVelocity(s.t) : kinematics.predictPosition(s.t);
  if (!predicted) {
    clear();
    return Verdict::Accept;
  }
  const double limit =
      velocity ? std::max({spikeDeviationG * kG0 * dt, kVelocityGateNoiseSigmas * velocityNoiseSigma,
                           kVelocityGateFloorMS})
               : std::max(kPositionGateSigmas * kinematics.fitResidualSigma(), kPositionGateFloorM);
  const Vec3 deviation = gatedValue(s) - *predicted;
  const double miss = length(deviation);

  if (velocity && noiseKnown) {
    const double bound =
        std::min(kLookBack * limit, kSuspectG * kG0 * dt + kSuspectSigmas * velocityNoiseSigma);
    if (const std::optional<Verdict> decided = lookBack(s, miss, limit, bound, kinematics))
      return *decided;
  }
  if (miss <= limit) {
    clear();
    return Verdict::Accept;
  }

  // Rejected. Held samples (if any) stay held: the next accepted sample decides on them.
  run_[static_cast<std::size_t>(runSize_)] = s;
  runMiss_[static_cast<std::size_t>(runSize_)] = deviation;
  if (runSize_ == 1) {
    const Vec3& a = runMiss_[0];
    scatters_ = dot(a, deviation) < kRunDirectionCos * length(a) * miss;
  }
  if (++runSize_ < kRunLength) return Verdict::Reject;
  const bool jump = runAgrees(limit);
  clear();
  return jump ? Verdict::Jump : Verdict::Noise;
}

std::optional<SpikeGate::Verdict> SpikeGate::lookBack(const RawFlightSample& s, double miss,
                                                      double limit, double bound,
                                                      const KinematicsEstimator& kinematics) {
  if (held_ == 0) {
    if (miss > bound && miss <= limit) return hold(s);
    return std::nullopt;
  }
  // The trend from before the held samples (addHeld never trims the window, so it is the trend
  // each of them was held against; there is one, as each was held against a prediction).
  const int held = held_;
  const auto resolve = [&](Verdict v) {
    clear();
    resolved_ = held;
    return v;
  };
  const std::optional<Vec3> before = kinematics.predictVelocityWithoutNewest(s.t, held);
  if (!before) return resolve(Verdict::Confirm);
  // Deviations from that trend: of the held samples (the newest last) and of this one.
  double heldMin = std::numeric_limits<double>::max();
  Vec3 lastHeld;
  for (std::size_t k = 0; k < static_cast<std::size_t>(held); ++k) {
    lastHeld = heldV_[k] - *kinematics.predictVelocityWithoutNewest(heldT_[k], held);
    heldMin = std::min(heldMin, length(lastHeld));
  }
  const Vec3 e = *s.velocity - *before;
  const double d = length(e), dHeld = length(lastHeld);
  if (d <= 0.5 * heldMin) return resolve(Verdict::DropPrevious);  // back on the old trend: a glitch
  if (miss > limit) return std::nullopt;  // rejected; the next sample decides
  // Still at the held level (within half of it): a velocity step or a longer glitch. Holding
  // one more must leave a usable window if they all turn out to be a glitch.
  if (length(e - lastHeld) <= 0.5 * std::max(dHeld, bound)) {
    if (held < kMaxLookBack && kinematics.size() - held >= KinematicsEstimator::kMinPoints) return hold(s);
    return resolve(Verdict::Confirm);
  }
  // Further away in the same direction: a real change of acceleration (a G step).
  if (d >= dHeld + 0.5 * bound && dot(e, lastHeld) >= kRunDirectionCos * d * dHeld)
    return resolve(Verdict::Accept);
  // Scattered: noise above what the gate assumed, not a glitch.
  return resolve(Verdict::Noise);
}

SpikeGate::Verdict SpikeGate::hold(const RawFlightSample& s) {
  heldT_[static_cast<std::size_t>(held_)] = s.t;
  heldV_[static_cast<std::size_t>(held_)] = *s.velocity;
  ++held_;
  runSize_ = 0;
  return Verdict::Hold;
}

bool SpikeGate::runAgrees(double limit) const {
  const RawFlightSample& first = run_.front();
  const RawFlightSample& last = run_.back();
  for (std::size_t i = 1; i + 1 < run_.size(); ++i) {
    const double w = (run_[i].t - first.t) / (last.t - first.t);
    const Vec3 line = gatedValue(first) + (gatedValue(last) - gatedValue(first)) * w;
    if (length(gatedValue(run_[i]) - line) > kRunAgreement * limit) return false;
  }
  return true;
}

}  // namespace ft
