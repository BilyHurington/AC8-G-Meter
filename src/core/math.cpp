#include "core/math.h"

namespace ft {

bool Basis::isOrthonormal(double tol) const {
  auto unit = [tol](const Vec3& v) { return std::abs(length(v) - 1.0) <= tol; };
  return unit(forward) && unit(right) && unit(up) && std::abs(dot(forward, right)) <= tol &&
         std::abs(dot(forward, up)) <= tol && std::abs(dot(right, up)) <= tol;
}

Basis Basis::fromUERotator(double pitchDeg, double yawDeg, double rollDeg) {
  // Same element layout as UE's FRotationMatrix (rows are the X, Y, Z axes).
  const double sp = std::sin(pitchDeg * kDegToRad), cp = std::cos(pitchDeg * kDegToRad);
  const double sy = std::sin(yawDeg * kDegToRad), cy = std::cos(yawDeg * kDegToRad);
  const double sr = std::sin(rollDeg * kDegToRad), cr = std::cos(rollDeg * kDegToRad);
  Basis b;
  b.forward = {cp * cy, cp * sy, sp};
  b.right = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
  b.up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
  return b;
}

}  // namespace ft
