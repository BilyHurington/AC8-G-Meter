#include "core/mission_feed.h"

namespace ft {
namespace {

constexpr int kMinOnsetPoints = 3;  // as for the live onset rate

}  // namespace

void MissionFeed::configure(const PipelineConfig& cfg, MissionTracker& mission) {
  highG_.setThresholds(cfg.highGEnter, cfg.highGExit);
  if (cfg.minExcursionS != filter_.width()) {
    flush(mission);
    filter_.setWidth(cfg.minExcursionS);
    highG_.reset();
    onset_.reset();
  }
}

void MissionFeed::add(MissionTracker& mission, const FlightMetrics& m, double dtS, bool liveHighGEntered,
                      double onsetWindowS) {
  const double now = mission.stats().durationS;
  if (filter_.width() <= 0.0) {
    mission.add(m, dtS, liveHighGEntered, now);
    return;
  }
  onsetWindowS_ = onsetWindowS;
  pending_.push_back({m, dtS});
  filter_.push(now, m.gRaw);
  drain(mission);
}

void MissionFeed::drain(MissionTracker& mission) {
  ExcursionFilter::Sample s;
  while (!pending_.empty() && filter_.pop(s)) {
    Pending p = pending_.front();
    pending_.pop_front();
    FlightMetrics& m = p.m;
    m.g = s.v;
    onset_.push(s.t, m.g);
    onset_.trim(onsetWindowS_, kMinOnsetPoints);
    m.onsetRate = onset_.size() >= kMinOnsetPoints ? onset_.slope() : 0.0;
    const HighGTimer::Step step = highG_.update(m.g, p.dtS);
    m.inHighG = highG_.active();
    m.highGDurationS = highG_.durationS();
    mission.add(m, p.dtS, step.entered, s.t);
  }
}

void MissionFeed::flush(MissionTracker& mission) {
  filter_.flush();
  if (mission.open()) drain(mission);
  clear();
}

void MissionFeed::clear() {
  filter_.reset();
  pending_.clear();
  highG_.reset();
  onset_.reset();
}

}  // namespace ft
