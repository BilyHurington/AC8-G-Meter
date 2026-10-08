#pragma once
// Minimal 3D vector / orientation types used across all layers.
//
// World axes follow Unreal Engine conventions so the UE4SS adapter needs no remapping:
//   +X = forward (north), +Y = right (east), +Z = up. Units are SI (metres, seconds).
// Only dot products and projections are used by the metrics, so the handedness of the
// frame never matters except inside Basis::fromUERotator().
#include <cmath>

namespace ft {

inline constexpr double kG0 = 9.80665;  // standard gravity, m/s^2
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kRadToDeg = 180.0 / kPi;
inline constexpr double kDegToRad = kPi / 180.0;

struct Vec3 {
  double x = 0.0, y = 0.0, z = 0.0;

  constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  constexpr Vec3 operator-() const { return {-x, -y, -z}; }
  constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
  constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  constexpr Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  constexpr Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
  constexpr bool operator==(const Vec3&) const = default;
};

constexpr Vec3 operator*(double s, const Vec3& v) { return v * s; }
constexpr double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
// Returns the zero vector for (near-)zero input instead of NaN.
inline Vec3 normalized(const Vec3& v) {
  const double len = length(v);
  return len > 1e-12 ? v / len : Vec3{};
}
inline bool isFinite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline constexpr Vec3 kWorldUp{0.0, 0.0, 1.0};
inline constexpr Vec3 kGravityWorld{0.0, 0.0, -kG0};

// Aircraft orientation as the body axes expressed in world coordinates.
// Using axis vectors (instead of Euler angles / quaternions) maps 1:1 onto UE's
// GetActorForwardVector / GetActorRightVector / GetActorUpVector.
struct Basis {
  Vec3 forward{1.0, 0.0, 0.0};
  Vec3 right{0.0, 1.0, 0.0};
  Vec3 up{0.0, 0.0, 1.0};

  // World vector -> body components (x = forward, y = right, z = up).
  Vec3 toBody(const Vec3& w) const { return {dot(w, forward), dot(w, right), dot(w, up)}; }
  // Body components -> world vector.
  Vec3 toWorld(const Vec3& b) const { return forward * b.x + right * b.y + up * b.z; }

  bool isOrthonormal(double tol = 1e-3) const;
  bool isFinite() const { return ft::isFinite(forward) && ft::isFinite(right) && ft::isFinite(up); }

  // Builds the basis from a UE FRotator (degrees), matching FRotationMatrix:
  // positive pitch = nose up, positive yaw = nose right, positive roll = right wing down.
  static Basis fromUERotator(double pitchDeg, double yawDeg, double rollDeg);
};

}  // namespace ft
