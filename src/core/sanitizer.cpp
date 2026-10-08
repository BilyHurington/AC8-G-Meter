#include "core/sanitizer.h"

#include <cmath>

namespace ft {

namespace {
constexpr double kOrthoTolerance = 1e-6;  // below this no repair is needed
constexpr double kRepairTolerance = 0.05; // above this the basis is rejected
}  // namespace

bool isFiniteSample(const RawFlightSample& s) {
  return std::isfinite(s.t) && isFinite(s.position) && (!s.velocity || isFinite(*s.velocity)) &&
         s.attitude.isFinite() && (!s.gameReportedG || std::isfinite(*s.gameReportedG));
}

bool sanitizeAttitude(Basis& b) {
  if (!b.isFinite() || !b.isOrthonormal(kRepairTolerance)) return false;
  if (b.isOrthonormal(kOrthoTolerance)) return true;
  // Re-orthonormalise keeping forward exact. `right` is rebuilt by projection rather
  // than a cross product so the source's handedness is preserved.
  const Vec3 f = normalized(b.forward);
  const Vec3 u = normalized(b.up - f * dot(b.up, f));
  const Vec3 r = normalized(b.right - f * dot(b.right, f) - u * dot(b.right, u));
  b = {f, r, u};
  return true;
}

bool isTeleport(const Vec3& prevPos, const std::optional<Vec3>& prevVel, const RawFlightSample& cur,
                double dtS, double toleranceM) {
  const Vec3 dp = cur.position - prevPos;
  if (prevVel && cur.velocity) {
    return length(dp - (*prevVel + *cur.velocity) * (0.5 * dtS)) > toleranceM;
  }
  return length(dp) > kTeleportSpeedMps * dtS + toleranceM;
}

}  // namespace ft
