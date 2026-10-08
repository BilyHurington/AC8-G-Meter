#include "core/mission_tracker.h"

#include <algorithm>

namespace ft {

void MissionTracker::start(double timelineHz) {
  timelineHz_ = timelineHz;
  open_ = true;
  clearStats();
}

void MissionTracker::clearStats() {
  stats_ = {};
  timeline_.clear();
  timelineStartS_ = 0.0;
}

void MissionTracker::resetGStats() {
  const MissionStats old = stats_;
  stats_ = {};
  stats_.hasData = old.hasData;
  stats_.durationS = old.durationS;
  stats_.maxAoaDeg = old.maxAoaDeg;
  stats_.maxSpeed = old.maxSpeed;
  stats_.acceptedSamples = old.acceptedSamples;
  stats_.rejectedSamples = old.rejectedSamples;
  timeline_.clear();
  timelineStartS_ = old.durationS;
}

void MissionTracker::addTime(double dtS) {
  stats_.durationS += dtS;
  stats_.hasData = true;
}

void MissionTracker::add(const FlightMetrics& m, double dtS, bool highGEntered) {
  MissionStats& s = stats_;
  const double now = s.durationS;

  if (!s.hasGData) {
    s.hasGData = true;
    s.maxG = s.minG = m.g;
    s.maxGAtS = s.minGAtS = now;
    s.maxOnset = s.minOnset = m.onsetRate;
  }
  if (m.g > s.maxG) {
    s.maxG = m.g;
    s.maxGAtS = now;
  }
  if (m.g < s.minG) {
    s.minG = m.g;
    s.minGAtS = now;
  }
  s.maxOnset = std::max(s.maxOnset, m.onsetRate);
  s.minOnset = std::min(s.minOnset, m.onsetRate);
  if (m.aoaValid) s.maxAoaDeg = std::max(s.maxAoaDeg, m.aoaDeg);
  s.maxSpeed = std::max(s.maxSpeed, m.speed);
  s.timeInGBinS[gBinIndex(m.g)] += dtS;

  if (highGEntered) ++s.highGEpisodes;
  // The episode clock starts at 0 on the entering sample, so that dt is not counted.
  if (m.inHighG && !highGEntered) s.totalHighGS += dtS;
  s.longestHighGS = std::max(s.longestHighGS, m.highGDurationS);

  // Sample-and-hold resampling onto a fixed grid of mission time.
  if (timelineHz_ > 0.0) {
    while (timelineStartS_ + static_cast<double>(timeline_.size()) / timelineHz_ <= now + 1e-9 &&
           timeline_.size() < kMaxTimelinePoints) {
      timeline_.push_back(static_cast<float>(m.g));
    }
  }
}

MissionReport MissionTracker::finish(bool aborted) {
  open_ = false;
  MissionReport r;
  r.id = nextId_++;
  r.aborted = aborted;
  r.stats = stats_;
  r.timelineHz = timelineHz_;
  r.timelineStartS = timelineStartS_;
  r.gTimeline = timeline_;
  return r;
}

}  // namespace ft
