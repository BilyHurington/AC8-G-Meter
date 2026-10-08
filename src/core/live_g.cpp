#include "core/live_g.h"

namespace ft {

void LiveG::configure(int medianWindow, double tauS) {
  for (Smoother& s : axes_) s.configure(medianWindow, tauS);
}

void LiveG::setTau(double tauS) {
  for (Smoother& s : axes_) s.setTau(tauS);
}

void LiveG::reset() {
  for (Smoother& s : axes_) s.reset();
  onset_.reset();
}

void LiveG::update(const Vec3& nBody, double t, double dtS, GMode mode, double onsetWindowS, FlightMetrics& m) {
  m.gRaw = mode == GMode::Nz ? nBody.z : length(nBody);
  m.loadFactorBody = {axes_[0].update(nBody.x, dtS), axes_[1].update(nBody.y, dtS), axes_[2].update(nBody.z, dtS)};
  m.gTotal = length(m.loadFactorBody);
  m.g = mode == GMode::Nz ? m.loadFactorBody.z : m.gTotal;
  onset_.push(t, m.g);
  onset_.trim(onsetWindowS, kMinOnsetPoints);
  m.onsetRate = onset_.slope();
  m.valid = onset_.size() >= kMinOnsetPoints;
}

void RecentRecorder::record(double g, double dtS) {
  const double period = 1.0 / history_.sampleHz;
  accumS_ += dtS;
  int pushes = 0;
  while (accumS_ >= period && pushes < RecentHistory::kCapacity) {
    history_.push(static_cast<float>(g));
    accumS_ -= period;
    ++pushes;
  }
  if (pushes == RecentHistory::kCapacity) accumS_ = 0.0;
}

}  // namespace ft
