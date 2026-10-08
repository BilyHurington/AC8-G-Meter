#pragma once
// Stateless input checks applied before a sample reaches the estimators.
#include <optional>

#include "core/math.h"
#include "core/sample.h"

namespace ft {

// True if every numeric field of the sample is finite.
bool isFiniteSample(const RawFlightSample& s);

// Validates the attitude and repairs small drift (float round-off in the game's
// rotation, interpolation) by Gram-Schmidt. Returns false if the basis is degenerate,
// i.e. too far from orthonormal to be trusted.
bool sanitizeAttitude(Basis& b);

// True if the position change from `prev` to `cur` over dt is inconsistent with the
// motion: with velocities, |dp - 0.5 (v0 + v1) dt| > toleranceM (trapezoid
// integration); without, an implied speed no aircraft reaches (respawn, checkpoint).
bool isTeleport(const Vec3& prevPos, const std::optional<Vec3>& prevVel, const RawFlightSample& cur,
                double dtS, double toleranceM);

inline constexpr double kTeleportSpeedMps = 1500.0;  // position-only teleport threshold

}  // namespace ft
