#pragma once
#include <string_view>
#include <vector>

#include "core/sample.h"

namespace ft {

// Producer of raw flight samples. Implementations: SimulatedSource and ReplaySource
// (src/sources) for the prototype; a UE4SS-backed source on Windows later.
class ITelemetrySource {
 public:
  virtual ~ITelemetrySource() = default;

  // Called once per host frame with the wall-clock time since the previous call.
  // Appends zero or more new samples (in time order) to `out`; never clears it.
  // A simulator may emit several samples per frame (fixed-rate sub-steps, time scale > 1).
  virtual void poll(double realDtS, std::vector<RawFlightSample>& out) = 0;

  virtual std::string_view name() const = 0;
};

}  // namespace ft
