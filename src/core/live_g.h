#pragma once
// LiveG: the last stage of the live (HUD) G path. Smooths the body-axis load factor (median +
// EMA per axis), derives `g` for the GMode, and its onset rate (least-squares slope over the
// onset window). Metrics are valid once the onset window holds kMinOnsetPoints samples.
#include <array>

#include "core/filters.h"
#include "core/metrics.h"

namespace ft {

class LiveG {
 public:
  // Median length (samples) and EMA time constant; restarts the smoothers.
  void configure(int medianWindow, double tauS);
  void setTau(double tauS);  // adaptive filtering: no restart
  void reset();

  // nBody: unsmoothed load factor in body axes (G), at sample time t, dtS after the previous
  // one. Sets m.gRaw, loadFactorBody, gTotal, g, onsetRate and valid.
  void update(const Vec3& nBody, double t, double dtS, GMode mode, double onsetWindowS, FlightMetrics& m);

 private:
  std::array<Smoother, 3> axes_;  // Nx, Ny, Nz
  TimedWindow<double> onset_;
};

// Fixed-rate decimation of the live G into RecentHistory (time based, so the history spans the
// same seconds at any frame rate).
class RecentRecorder {
 public:
  RecentRecorder() { clear(); }
  void clear() {
    history_.clear();
    accumS_ = 1.0 / history_.sampleHz;  // the first valid sample is recorded immediately
  }
  void record(double g, double dtS);
  const RecentHistory& history() const { return history_; }

 private:
  RecentHistory history_;
  double accumS_ = 0.0;
};

}  // namespace ft
