#include "core/spike_gate.h"

#include <algorithm>
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
// Look-back (velocity input): a sample that misses the prediction by more than this fraction
// of the gate limit is suspect, and the newest one or two window samples were a glitch if the
// suspect fits the trend without them within the same fraction. Noise (at most 1/15 of the
// limit per axis, once measured) never qualifies.
constexpr double kLookBack = 0.5;
// A run of rejected samples is a jump if each one lies within this fraction of the gate limit
// of the line through the first and last (a real jump: they agree to within their noise,
// which the gate limit exceeds 15-fold, or follow a steady acceleration). Noise the gate
// underestimated scatters by about the limit itself.
constexpr double kRunAgreement = 0.25;

Vec3 gatedValue(const RawFlightSample& s) { return s.velocity ? *s.velocity : s.position; }

}  // namespace

SpikeGate::Verdict SpikeGate::check(const RawFlightSample& s, double dt,
                                    const KinematicsEstimator& kinematics, double spikeDeviationG,
                                    double velocityNoiseSigma, bool noiseKnown) {
  const bool velocity = s.velocity.has_value();
  const std::optional<Vec3> predicted =
      velocity ? kinematics.predictVelocity(s.t) : kinematics.predictPosition(s.t);
  const double limit =
      velocity ? std::max({spikeDeviationG * kG0 * dt, kVelocityGateNoiseSigmas * velocityNoiseSigma,
                           kVelocityGateFloorMS})
               : std::max(kPositionGateSigmas * kinematics.fitResidualSigma(), kPositionGateFloorM);
  if (!predicted) {
    clear();
    return Verdict::Accept;
  }
  const double miss = length(gatedValue(s) - *predicted);
  const bool suspect = velocity && noiseKnown && miss > kLookBack * limit;
  // Removing two samples must leave enough for an estimate together with the new one.
  const int maxDrop = kinematics.size() > KinematicsEstimator::kMinPoints ? kMaxLookBack : 1;
  for (int count = 1; suspect && count <= maxDrop; ++count) {
    const std::optional<Vec3> without = kinematics.predictVelocityWithoutNewest(s.t, count);
    if (without && length(*s.velocity - *without) <= kLookBack * limit) {
      clear();
      dropCount_ = count;
      return Verdict::DropPrevious;
    }
  }
  if (miss <= limit) {
    clear();
    return suspect ? Verdict::Hold : Verdict::Accept;
  }
  run_[static_cast<std::size_t>(runSize_++)] = s;
  if (runSize_ < kRunLength) return Verdict::Reject;
  const bool jump = runAgrees(limit);
  clear();
  return jump ? Verdict::Jump : Verdict::Accept;
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
