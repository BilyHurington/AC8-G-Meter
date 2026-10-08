#include "core/mission_feed.h"

namespace ft {

void MissionFeed::configure(const PipelineConfig& cfg, MissionTracker& mission) {
  highG_.setThresholds(cfg.highGEnter, cfg.highGExit);
  const double w = cfg.minExcursionS > 0.0 ? cfg.minExcursionS : 0.0;
  if (w != filter_.width()) {
    restart(mission, true, false);
    filter_.setWidth(w);
  }
}

void MissionFeed::add(MissionTracker& mission, const FlightMetrics& m, double stepDt, bool inMission,
                      const FilterState& filter) {
  const bool filtered = filter_.width() > 0.0;
  if (filtered) followNoise(filter, stepDt);
  streamT_ += stepDt;
  if (restarted_) {
    restarted_ = false;
    // The filter's look-back (W, and up to W / 4 of pre-smoothing) must lie after the restart.
    confirmFromT_ = streamT_ + (1.0 + 0.5 * SustainedFilter::kMaxPreRatio) * filter_.width();
  }
  Pending p;
  p.inMission = inMission && mission.open();
  if (p.inMission) {
    p.missionT = mission.stats().durationS;
    p.missionDt = stepDt;
    p.epoch = epoch_;
  }
  pending_.push_back(p);
  filter_.push(streamT_, filtered ? m.gRaw : m.g);
  drain(mission);
}

void MissionFeed::followNoise(const FilterState& filter, double stepDt) {
  const double want = sustainedPreSmoothingS(filter_.width(), filter);
  const double have = filter_.preSmoothing();
  if (want > have) {  // more noise: at once
    filter_.setPreSmoothing(want);
    narrowerForS_ = 0.0;
  } else if (want < have - 1e-9) {  // less: once it has lasted (the estimate fluctuates)
    narrowerForS_ += stepDt;
    if (narrowerForS_ >= kPreReleaseS) {
      filter_.setPreSmoothing(want);
      narrowerForS_ = 0.0;
    }
  } else {
    narrowerForS_ = 0.0;
  }
}

void MissionFeed::drain(MissionTracker& mission) {
  SustainedFilter::Sample s;
  while (!pending_.empty() && filter_.pop(s)) {
    const Pending p = pending_.front();
    pending_.pop_front();
    if (!p.inMission || p.epoch != epoch_ || !mission.open()) continue;
    // Episodes belong to one mission: the timer restarts with the mission's first sample.
    if (timerEpoch_ != epoch_) {
      highG_.reset();
      timerEpoch_ = epoch_;
    }
    SustainedSample x;
    x.g = s.v;
    x.atS = p.missionT;
    x.dtS = p.missionDt;
    x.confirmed = s.t >= confirmFromT_ - 1e-9;
    // Unconfirmed values can carry an episode on or end it, but not start one.
    if (x.confirmed || highG_.active()) x.highGEntered = highG_.update(x.g, x.dtS).entered;
    x.inHighG = highG_.active();
    x.highGDurationS = highG_.durationS();
    mission.addSustained(x);
  }
}

void MissionFeed::restart(MissionTracker& mission, bool keepPending, bool endEpisode) {
  if (keepPending) {
    filter_.flush();
    drain(mission);
  }
  filter_.reset();
  pending_.clear();
  if (endEpisode) highG_.reset();
  restarted_ = true;
}

void MissionFeed::clear() {
  filter_.reset();
  pending_.clear();
  highG_.reset();
  restarted_ = true;
}

}  // namespace ft
