#pragma once
// Point-mass flight model used to generate realistic, physically consistent telemetry
// with exact ground truth (true load factor and AoA) for testing the estimators.
#include "core/math.h"

namespace ft {

struct ControlInput {
  double nzCmd = 1.0;          // commanded load factor along the lift axis, G
  double rollRateDegS = 0.0;   // roll rate about the velocity vector, deg/s
  double throttle = 0.5;       // 0..1, drives speed via a crude thrust - drag balance
};

struct FlightModelParams {
  double minSpeed = 60.0;       // m/s, floor to keep the model well conditioned
  double maxSpeed = 600.0;      // m/s
  double maxThrustG = 1.2;      // longitudinal acceleration at full throttle, in G
  double dragCoef = 1.2e-5;     // drag accel (G) = dragCoef * v^2 (+ induced, see .cpp)
  double aoaPerG = 2.2;         // deg AoA per G at refSpeed (AoA ~ n / q)
  double refSpeed = 200.0;      // m/s
  double maxAoaDeg = 35.0;
  double nzSlewGPerS = 12.0;    // how fast the airframe follows nzCmd (onset limit)
};

// Exact state of the simulated aircraft at time t.
struct FlightState {
  double t = 0.0;
  Vec3 position{0.0, 0.0, 3000.0};
  Vec3 velocity{220.0, 0.0, 0.0};
  Basis attitude;               // body axes (nose offset from velocity by AoA about right axis)
  // Ground truth, computed analytically from the specific force (not from sampled data).
  double trueNz = 1.0;          // specific force . body up / g0
  double trueGTotal = 1.0;      // |specific force| / g0
  double trueAoaDeg = 0.0;
  double trueSideslipDeg = 0.0;
  double appliedNz = 1.0;       // slew-limited lift load factor actually applied
};

class PointMassFlightModel {
 public:
  explicit PointMassFlightModel(const FlightModelParams& params = {});
  void reset(const FlightState& initial);
  // Advances by dt (s). Callers should sub-step at <= 1/120 s for accuracy.
  void step(double dt, const ControlInput& input);
  const FlightState& state() const { return state_; }
  const FlightModelParams& params() const { return params_; }

 private:
  FlightModelParams params_;
  FlightState state_;
  Vec3 liftDir_{0.0, 0.0, 1.0};  // unit, perpendicular to velocity (parallel-transported)
};

}  // namespace ft
